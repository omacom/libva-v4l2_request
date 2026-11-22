#!/usr/bin/env python3
"""Finite resource campaign with quiescent checkpoints and independent pixels."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import signal
import shlex
import subprocess
import time

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('resource_sampler', HERE / 'resource-churn.py')
sampler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sampler)
FORMATS = ['yuv420p', 'yuv420p', 'yuv420p', 'yuv420p10le']
NAMES = ['h264.mkv', 'hevc.mkv', 'vp9-8.webm', 'vp9-10.webm']


def run(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    if result.returncode:
        raise RuntimeError('{} failed ({}):\n{}'.format(Path(cmd[0]).name, result.returncode, result.stdout))
    return result.stdout


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if not compiler:
        raise ValueError('CC must name a compiler')
    cflags = run(['pkg-config', '--cflags', '--libs', 'libavformat', 'libavcodec',
                  'libavutil', 'libswscale', 'libva']).split()
    for source, output in [('resource-workload.c', 'workload'), ('frame-check.c', 'reference')]:
        run(compiler + ['-Wall', '-Wextra', '-Werror', '-O2', '-o', str(root / output), str(HERE / source), *cflags, '-ldl'])
    run(compiler + ['-shared', '-fPIC', '-o', str(root / 'early.so'), str(HERE / 'early-export.c'),
         *run(['pkg-config', '--cflags', '--libs', 'libva']).split(), '-ldl'])
    manifest = {'inputs': [], 'commands': [], 'reference_source_sha256': digest(HERE / 'frame-check.c'),
                'workload_source_sha256': digest(HERE / 'resource-workload.c')}
    for i, (name, fmt) in enumerate(zip(NAMES, FORMATS)):
        codec = [['libx264', '-preset', 'ultrafast', '-bf', '2'],
                 ['libx265', '-preset', 'ultrafast', '-x265-params', 'log-level=error:pools=2'],
                 ['libvpx-vp9', '-deadline', 'good', '-cpu-used', '5', '-threads', '2', '-lag-in-frames', '8'],
                 ['libvpx-vp9', '-deadline', 'good', '-cpu-used', '5', '-threads', '2', '-lag-in-frames', '8']][i]
        cmd = ['ffmpeg', '-nostdin', '-v', 'error', '-y', '-f', 'lavfi', '-i',
               'testsrc2=size=640x360:rate={},format={}'.format(24 + i, fmt),
               '-frames:v', '24', '-c:v', *codec, '-g', '12', '-pix_fmt', '+' + fmt, str(root / name)]
        # Ubuntu 20.04's libvpx build has no 10-bit encoder. Keep the full
        # decode/checkpoint coverage using our pinned synthetic fixture.
        if i == 3:
            fixture = HERE / 'fixtures/resource/vp9-10.webm'
            if digest(fixture) != 'cfc1d27ad161696e574020b910cd952725cf67d12e0913dedec7d202aae84463':
                raise ValueError('pinned VP9 10-bit fixture checksum changed')
            (root / name).write_bytes(fixture.read_bytes())
        else:
            run(cmd)
        reference = run([str(root / 'reference'), 'software', str(root / name), fmt])
        (root / (name + '.reference')).write_text(reference)
        frames = [line for line in reference.splitlines() if line.startswith('frame ')]
        md5 = re.findall(r'^MD5=([0-9a-f]{32})$', reference, re.M)
        if len(frames) != 24 or len(md5) != 1:
            raise ValueError('invalid independent reference')
        manifest['inputs'].append({'name': name, 'format': fmt, 'sha256': digest(root / name),
                                   'frames': frames, 'md5': md5[0]})
        manifest['commands'].append(['copy', 'tests/fixtures/resource/vp9-10.webm', name]
                                    if i == 3 else cmd[:-1] + [name])
    manifest['ffmpeg'] = run(['ffmpeg', '-version']).splitlines()[0]
    manifest['compiler'] = run(compiler + ['--version']).splitlines()[0]
    manifest['workload_sha256'] = digest(root / 'workload')
    manifest['early_export_sha256'] = digest(root / 'early.so')
    (root / 'inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def allocator_violations(baseline, sample, maximum):
    """A larger allocator arena is acceptable only with bounded live/cache use.

    mallinfo includes tcache chunks as allocated. It is not a driver ownership
    counter; preserve both these values and the stricter FD/dma-buf checks.
    """
    old, new = baseline['allocator'], sample['allocator']
    mapped_growth = sample['mapped_bytes'] - baseline['mapped_bytes']
    if 'allocated' not in old or 'allocated' not in new:
        return ['mapping growth has no allocator explanation'] if mapped_growth > 0 else []
    violations = []
    live_growth = new['allocated'] + new['mmap'] - old['allocated'] - old['mmap']
    if live_growth > maximum:
        violations.append('allocator-accounted allocation growth exceeds {} bytes'.format(maximum))
    arena_growth = max(0, new['arena'] - old['arena']) + max(0, new['mmap'] - old['mmap'])
    if mapped_growth > arena_growth:
        violations.append('mapping growth exceeds allocator arena accounting')
    return violations


def lines(process, deadline):
    pending = b''
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('campaign deadline')
        if not select.select([process.stdout], [], [], min(30, remaining))[0]:
            raise TimeoutError('workload made no progress for 30 seconds')
        chunk = os.read(process.stdout.fileno(), 65536)
        if not chunk:
            if pending:
                raise ValueError('incomplete workload record')
            return
        pending += chunk
        if len(pending) > 1024 * 1024:
            raise ValueError('oversized workload record')
        while b'\n' in pending:
            line, pending = pending.split(b'\n', 1)
            yield line.decode('ascii')


def campaign(args):
    root = args.directory.resolve()
    manifest = json.loads((root / 'inputs.json').read_text())
    if args.mode != 'software' and not os.environ.get('LIBVA_HW_GUARD_LEASE'):
        raise ValueError('hardware campaign requires hwguard')
    if args.cycles < 1 or args.cycles > 1000000 or args.seconds < 0 or args.seconds > 86400:
        raise ValueError('invalid finite cycle/duration bound')
    if min(args.max_map_growth, args.max_mapped_growth_kib, args.max_rss_growth_kib, args.max_heap_growth_kib) < 0:
        raise ValueError('resource bounds must be nonnegative')
    for item in manifest['inputs']:
        if digest(root / item['name']) != item['sha256']:
            raise ValueError('input checksum changed')
    if digest(root / 'workload') != manifest['workload_sha256']:
        raise ValueError('workload checksum changed')
    env = os.environ.copy()
    env.pop('LD_PRELOAD', None)
    if args.mode == 'early':
        if digest(root / 'early.so') != manifest['early_export_sha256']:
            raise ValueError('early-export helper checksum changed')
        env['LD_PRELOAD'] = str(root / 'early.so')
    warmup = 20  # five complete lifecycles for each of the four codecs
    cmd = [str(root / 'workload'), 'software' if args.mode == 'software' else 'vaapi',
           str(args.cycles), str(warmup)]
    for item in manifest['inputs']:
        cmd.extend([str(root / item['name']), item['format']])
    limits = {'fds': 0, 'maps': args.max_map_growth, 'mapped_bytes': args.max_mapped_growth_kib * 1024,
              'vmrss_kib': args.max_rss_growth_kib, 'dmabuf_references': 0,
              'dmabuf_objects': 0, 'dmabuf_bytes': 0, 'heap_allocated_bytes': args.max_heap_growth_kib * 1024}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    samples, frames, result_count, held_count, last_checkpoint = [], [], 0, 0, -1
    started = time.monotonic()
    baseline_time = None
    initial = None
    allocator = None
    stop_sent = False
    summary = {'passed': False, 'mode': args.mode, 'limits': limits}
    process = None
    with args.output.open('x') as output, args.output.with_suffix('.workload.log').open('x') as raw, \
            args.output.with_suffix('.stderr.log').open('x') as errors:
        try:
            sampler.write_record(output, {'type': 'metadata', 'mode': args.mode, 'warmup_cycles': warmup,
                'requested_cycles': args.cycles, 'requested_seconds': args.seconds, 'limits': limits,
                'source_commit': env.get('V4L2R_SOURCE_COMMIT'), 'inputs_sha256': digest(root / 'inputs.json'),
                'driver_sha256': digest(Path(env['LIBVA_DRIVERS_PATH']) / 'v4l2_request_drv_video.so')
                    if args.mode != 'software' else None})
            process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, env=env,
                                       start_new_session=True)
            identity = sampler._read_identity(process.pid)[1]
            deadline = started + (args.seconds + 600 if args.seconds else max(600, args.cycles * 5))
            for line in lines(process, deadline):
                raw.write(line + '\n')
                if line == 'INITIAL':
                    if initial is not None or result_count:
                        raise ValueError('duplicate/out-of-order initial state')
                    initial = sampler.snapshot(process.pid)
                    if initial['process_start_time'] != identity:
                        raise ValueError('workload identity changed')
                    initial['type'] = 'initial'
                    sampler.write_record(output, initial)
                    process.stdin.write(b'go\n')
                    process.stdin.flush()
                elif line.startswith('frame '):
                    frames.append(line)
                elif line.startswith('RESULT '):
                    match = re.fullmatch(r'RESULT cycle=(\d+) stream=(\d+) pass=(\d+) frames=(\d+) MD5=([0-9a-f]{32})', line)
                    if not match:
                        raise ValueError('malformed result')
                    cycle, stream, part, count = map(int, match.groups()[:4])
                    if (cycle, stream, part) != (result_count // 2 + 1, (result_count // 2) % 4, result_count % 2):
                        raise ValueError('missing/duplicate/out-of-order result')
                    expected = manifest['inputs'][stream]
                    if frames != expected['frames'] or count != len(frames) or match[5] != expected['md5']:
                        raise ValueError('pixels differ from independent reference at cycle {}'.format(cycle))
                    frames = []
                    result_count += 1
                elif line.startswith('HELD_IMAGE_PASS '):
                    held_count += 1
                    if line != 'HELD_IMAGE_PASS cycle={}'.format(held_count):
                        raise ValueError('invalid held-image sequence')
                elif line.startswith('ALLOCATOR '):
                    match = re.fullmatch(r'ALLOCATOR arena=(\d+) mmap=(\d+) allocated=(\d+) free=(\d+) releasable=(\d+)', line)
                    if allocator is not None or (not match and line != 'ALLOCATOR unavailable'):
                        raise ValueError('invalid allocator record')
                    allocator = dict(zip(['arena', 'mmap', 'allocated', 'free', 'releasable'], map(int, match.groups()))) if match else {'status': 'unavailable'}
                elif line.startswith('CHECKPOINT '):
                    cycle = int(line.split()[1])
                    if cycle != last_checkpoint + 1 or result_count != (warmup + cycle) * 2 or frames:
                        raise ValueError('incomplete checkpoint')
                    if args.mode != 'software' and held_count != warmup + cycle:
                        raise ValueError('missing surviving-image checks')
                    raw.flush()
                    os.fsync(raw.fileno())
                    sample = sampler.snapshot(process.pid)
                    if sample['process_start_time'] != identity:
                        raise ValueError('workload identity changed')
                    if initial is None:
                        raise ValueError('missing initial resource state')
                    for key in ['fd_count', 'dmabuf_references', 'dmabuf_objects', 'dmabuf_bytes']:
                        if sample[key] is None or sample[key] != initial[key]:
                            raise ValueError('{} differs from initialized-display state'.format(key))
                    sample.update(cycle=cycle, decoded_frames=result_count * 24)
                    if allocator is None:
                        raise ValueError('missing allocator record')
                    sample['allocator'] = allocator
                    allocator = None
                    sampler.write_record(output, sample)
                    samples.append(sample)
                    last_checkpoint = cycle
                    now = time.monotonic()
                    if baseline_time is None:
                        baseline_time = now
                    current = sampler.summarize([samples[0], sample], limits, acceptance=True)
                    current['violations'].extend(allocator_violations(samples[0], sample, limits['heap_allocated_bytes']))
                    if current['violations']:
                        raise ValueError('; '.join(current['violations']))
                    stop_sent = bool(args.seconds and now - baseline_time >= args.seconds)
                    process.stdin.write(b'stop\n' if stop_sent else b'go\n')
                    process.stdin.flush()
            rc = process.wait(timeout=10)
            errors.flush()
            error_lines = args.output.with_suffix('.stderr.log').read_text().splitlines()
            if any('Failed to destroy' in line or 'EARLY_EXPORT_FAIL' in line for line in error_lines):
                raise ValueError('client cleanup or early export failed')
            if args.mode == 'early' and sum(line.startswith('EARLY_EXPORT_PASS:') for line in error_lines) != result_count * 24:
                raise ValueError('missing early-export identity/layout checks')
            elapsed = time.monotonic() - baseline_time if baseline_time else 0
            if rc or frames or len(samples) < 2 or (args.seconds and not stop_sent) or (not args.seconds and last_checkpoint != args.cycles):
                raise ValueError('incomplete/nonzero campaign')
            summary = sampler.summarize(samples, limits, acceptance=True)
            if all('allocated' in s['allocator'] for s in samples):
                summary['allocator_bytes'] = {key: {'initial': samples[0]['allocator'][key],
                    'minimum': min(s['allocator'][key] for s in samples),
                    'maximum': max(s['allocator'][key] for s in samples),
                    'final': samples[-1]['allocator'][key]} for key in samples[0]['allocator']}
            summary.update(mode=args.mode, cycles=last_checkpoint, warmup_cycles=warmup, elapsed_seconds=elapsed,
                           sample_span_seconds=(samples[-1]['monotonic_ns'] - samples[0]['monotonic_ns']) / 1e9,
                           decoded_frames=result_count * 24, held_image_checks=held_count, returncode=rc)
        except BaseException as exc:
            summary.update(type='summary', error=str(exc), cycles=last_checkpoint)
            raise
        finally:
            for sig in (signal.SIGINT, signal.SIGTERM):
                signal.signal(sig, signal.SIG_IGN)
            if process is not None:
                # The C workload has no children. Popen tracks/reaps its exact
                # child; never signal a numeric group after its leader exits.
                sampler.stop_workload(process, 3)
                for pipe in (process.stdin, process.stdout):
                    pipe.close()
            sampler.write_record(output, summary)
    print(json.dumps(summary, sort_keys=True), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'run'])
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--mode', choices=['software', 'normal', 'early'], default='software')
    parser.add_argument('--cycles', type=int, default=1000)
    parser.add_argument('--seconds', type=int, default=0)
    parser.add_argument('--max-map-growth', type=int, default=0)
    parser.add_argument('--max-mapped-growth-kib', type=int, default=0)
    parser.add_argument('--max-rss-growth-kib', type=int, default=4096)
    parser.add_argument('--max-heap-growth-kib', type=int, default=64)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.action == 'prepare':
        prepare(args.directory.resolve())
    elif args.output is None:
        parser.error('run needs --output')
    else:
        def interrupted(signum, frame):
            raise SystemExit(128 + signum)
        previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            campaign(args)
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)


if __name__ == '__main__':
    main()
