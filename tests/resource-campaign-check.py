#!/usr/bin/env python3
"""Exercise real software decoding and fail-closed campaign/cleanup paths."""
import json
import importlib.util
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent


def interrupt_wrapper_check(root):
    """A guard stop must clean up the wrapper's coordinator, even if stalled."""
    fixture = root / 'interrupt-wrapper'
    fixture.mkdir()
    shutil.copyfile(HERE / 'resource-interrupt.py', fixture / 'resource-interrupt.py')
    # No hardware or actual campaign: the coordinator only records its identity
    # and waits. The second case forces the bounded SIGKILL cleanup path.
    (fixture / 'resource-campaign.py').write_text(
        "import os, signal, time\n"
        "from pathlib import Path\n"
        "if os.environ.get('FIXTURE_IGNORE_TERM'):\n"
        "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "Path(os.environ['FIXTURE_PID']).write_text(str(os.getpid()))\n"
        "time.sleep(60)\n")
    for stubborn in (False, True):
        pidfile = fixture / ('stubborn.pid' if stubborn else 'responsive.pid')
        output = fixture / ('stubborn.jsonl' if stubborn else 'responsive.jsonl')
        env = dict(os.environ, LIBVA_HW_GUARD_LEASE='offline-fixture', FIXTURE_PID=str(pidfile))
        env.pop('FIXTURE_IGNORE_TERM', None)
        if stubborn:
            env['FIXTURE_IGNORE_TERM'] = '1'
        process = subprocess.Popen([sys.executable, str(fixture / 'resource-interrupt.py'),
                                    '--directory', str(fixture), '--output', str(output)], env=env)
        try:
            deadline = time.monotonic() + 5
            while not pidfile.exists() and time.monotonic() < deadline and process.poll() is None:
                time.sleep(0.02)
            assert pidfile.exists(), 'fixture coordinator did not start'
            child = int(pidfile.read_text())
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=8) == 128 + signal.SIGTERM
            assert not Path('/proc', str(child)).exists(), 'wrapper left its coordinator alive'
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def main():
    spec = importlib.util.spec_from_file_location('campaign', HERE / 'resource-campaign.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    baseline = {'mapped_bytes': 10000, 'allocator': {'allocated': 1000, 'mmap': 0, 'arena': 5000}}
    retained = {'mapped_bytes': 12000, 'allocator': {'allocated': 1100, 'mmap': 0, 'arena': 7000}}
    assert not module.allocator_violations(baseline, retained, 200)
    unexplained = {'mapped_bytes': 13000, 'allocator': retained['allocator']}
    assert module.allocator_violations(baseline, unexplained, 200)
    leaked = {'mapped_bytes': 12000, 'allocator': {'allocated': 1300, 'mmap': 0, 'arena': 7000}}
    assert module.allocator_violations(baseline, leaked, 200)
    unknown = {'mapped_bytes': 12000, 'allocator': {'status': 'unavailable'}}
    assert module.allocator_violations(baseline, unknown, 200)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        interrupt_wrapper_check(root)
        base = [sys.executable, str(HERE / 'resource-campaign.py')]
        subprocess.run(base + ['prepare', '--directory', str(root)], check=True, timeout=90)
        run = base + ['run', '--directory', str(root), '--cycles', '4']
        good = root / 'good.jsonl'
        subprocess.run(run + ['--output', str(good)], check=True, timeout=30)
        records = [json.loads(line) for line in good.read_text().splitlines()]
        assert records[-1]['passed'] and records[-1]['cycles'] == 4
        assert records[-1]['decoded_frames'] == 24 * 2 * 24
        assert [r['cycle'] for r in records if r['type'] == 'sample'] == list(range(5))
        original = (root / 'inputs.json').read_text()
        manifest = json.loads(original)
        manifest['inputs'][0]['md5'] = '0' * 32
        (root / 'inputs.json').write_text(json.dumps(manifest))
        bad = subprocess.run(run + ['--output', str(root / 'bad-pixels.jsonl')], capture_output=True, text=True, timeout=30)
        assert bad.returncode and 'pixels differ from independent reference' in bad.stderr
        assert not json.loads((root / 'bad-pixels.jsonl').read_text().splitlines()[-1])['passed']
        (root / 'inputs.json').write_text(original)
        manifest = json.loads(original)
        manifest['inputs'][0]['sha256'] = '0' * 64
        (root / 'inputs.json').write_text(json.dumps(manifest))
        bad = subprocess.run(run + ['--output', str(root / 'bad-input.jsonl')], capture_output=True, text=True, timeout=10)
        assert bad.returncode and 'input checksum changed' in bad.stderr
        assert not (root / 'bad-input.jsonl').exists()
        (root / 'inputs.json').write_text(original)
        env = os.environ.copy()
        env.pop('LIBVA_HW_GUARD_LEASE', None)
        bad = subprocess.run(run + ['--mode', 'normal', '--output', str(root / 'unguarded.jsonl')], env=env,
                             capture_output=True, text=True, timeout=10)
        assert bad.returncode and 'requires hwguard' in bad.stderr
        assert not (root / 'unguarded.jsonl').exists()
        output = root / 'interrupt.jsonl'
        with (root / 'interrupt.log').open('w') as log:
            process = subprocess.Popen(base + ['run', '--directory', str(root), '--cycles', '1000000', '--output', str(output)], stdout=log, stderr=log)
            child = None
            try:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    if output.exists():
                        for line in output.read_text().splitlines():
                            try:
                                record = json.loads(line)
                            except json.JSONDecodeError:
                                continue
                            if record.get('type') == 'sample':
                                child = record['pid']
                                break
                    if child is not None:
                        break
                    if process.poll() is not None:
                        raise AssertionError('workload exited before interruption')
                    time.sleep(0.02)
                assert child is not None, 'no checkpoint before deadline'
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=10) == 128 + signal.SIGTERM
                assert not Path('/proc', str(child)).exists(), 'interrupted workload survived'
                assert not json.loads(output.read_text().splitlines()[-1])['passed']
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
        print('PASS: resource pixels/checkpoints, bad reference/input, guard refusal, interrupted-child cleanup')


if __name__ == '__main__':
    main()
