#!/usr/bin/env python3
"""Real cancellation/deadline regressions without any decoder access."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

RUNNER = Path(__file__).with_name('concurrent-process.py')
spec = importlib.util.spec_from_file_location('runner', RUNNER)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for escaped in (False, True):
        pidfile = root / ('escaped.pid' if escaped else 'worker.pid')
        child = "import os,time;from pathlib import Path;Path(%r).write_text(str(os.getpid()));time.sleep(30)" % str(pidfile)
        worker = ("import subprocess,sys;subprocess.Popen([sys.executable,'-c',%r],start_new_session=True)" % child) if escaped else child
        launch = "import importlib.util,sys;s=importlib.util.spec_from_file_location('r',%r);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);sys.exit(m.run_processes([sys.executable,'-c',%r],1,4,1,1,10))" % (str(RUNNER), worker)
        proc = subprocess.Popen([sys.executable, '-c', launch], stdout=subprocess.DEVNULL)
        handle = None
        try:
            end = time.monotonic() + 5
            # write_text() creates the file before it writes the pid.
            while time.monotonic() < end:
                text = pidfile.read_text() if pidfile.exists() else ''
                if text.strip().isdigit():
                    break
                time.sleep(.01)
            assert text.strip().isdigit()
            pid = int(text)
            # pidfd binds fixture cleanup to this process identity where available.
            if hasattr(os, 'pidfd_open'):
                handle = os.pidfd_open(pid)
            if not escaped:
                proc.send_signal(signal.SIGTERM)
                assert proc.wait(timeout=7) == 143
                end = time.monotonic() + 3
                while Path('/proc', str(pid), 'stat').exists() and time.monotonic() < end:
                    if Path('/proc', str(pid), 'stat').read_text().split()[2] == 'Z':
                        break
                    time.sleep(.01)
                assert not Path('/proc', str(pid), 'stat').exists() or Path('/proc', str(pid), 'stat').read_text().split()[2] == 'Z'
            else:
                # No pipe EOF wait: even an escaped stdout holder cannot hang us.
                assert proc.wait(timeout=3) == 1
        finally:
            if handle is not None:
                try:
                    signal.pidfd_send_signal(handle, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.close(handle)
            elif escaped and pidfile.exists():
                # The still-running fixture remains alive for 30s; this test is bounded.
                try:
                    os.kill(int(pidfile.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=3)

# An already reaped/unowned Popen must never result in a numeric group signal.
proc = subprocess.Popen([sys.executable, '-c', 'pass'])
proc.wait(timeout=3)
calls = []
real_killpg = m.os.killpg
m.os.killpg = lambda *args: calls.append(args)
try:
    m.kill_process_group(proc)
    m.kill_process_group(proc)
    assert calls == []
    proc._group_owned = True  # synthetic ownership token, no real signal
    m.kill_process_group(proc)
    m.kill_process_group(proc)
    assert len(calls) == 1
finally:
    m.os.killpg = real_killpg
print('process cancellation, escaped stdout and one-time ownership: PASS')
