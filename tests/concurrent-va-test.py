#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Falsify the real runner's oracle and owned-process cleanup; no device access."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time

RUNNER=Path(__file__).with_name('concurrent-va-run.py')
spec=importlib.util.spec_from_file_location('runner',RUNNER)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
PYTHON=sys.executable


def require(value,label):
    if not value:raise AssertionError(label)


def exited(pid):
    try:return Path('/proc',str(pid),'stat').read_text().rsplit(')',1)[1].split()[0]=='Z'
    except FileNotFoundError:return True


def published_pid(path):
    try:pid=int(path.read_text())
    except (FileNotFoundError,ValueError):return None
    # Never turn an unfinished/invalid publication into a process-group signal.
    return pid if pid>1 else None


def wait_file(path,timeout=4):
    limit=time.monotonic()+timeout
    while True:
        pid=published_pid(path)
        if pid is not None:return pid
        if time.monotonic()>=limit:break
        time.sleep(.01)
    require(False,'fixture did not publish a valid PID')



def pid_file_tests(root):
    # Force the exact observed create-before-write race: publication cannot
    # finish until the reader has actually observed the empty file.
    path=root/'publishing.pid';path.write_text('')
    observed=threading.Event()
    class PublishingPath:
        def exists(self):return path.exists()
        def read_text(self):
            text=path.read_text()
            if not text:observed.set()
            return text
    def publish():
        if observed.wait(timeout=2):path.write_text(str(os.getpid()))
    writer=threading.Thread(target=publish)
    writer.start()
    try:require(wait_file(PublishingPath())==os.getpid(),'incomplete PID publication was not retried')
    finally:
        observed.set();writer.join(timeout=2)
    require(not writer.is_alive(),'PID fixture writer leaked')
    for text in ('', 'not a PID', '0', '-7', '1'):
        path.write_text(text)
        require(published_pid(path) is None,'invalid PID accepted')
        try:wait_file(path,timeout=.03)
        except AssertionError as error:
            require(str(error)=='fixture did not publish a valid PID','wrong publication failure')
        else:raise AssertionError('invalid PID wait unexpectedly passed')
    path.unlink()
    require(published_pid(path) is None,'missing PID accepted')
    try:wait_file(path,timeout=.03)
    except AssertionError:pass
    else:raise AssertionError('missing PID wait unexpectedly passed')
    path.write_text(str(os.getpid()))
    require(published_pid(path)==os.getpid(),'valid PID rejected')


def oracle_tests():
    clips=[dict(frames=8,full='1'*32,prefix='2'*32,held='3'*32),
           dict(frames=8,full='4'*32,prefix='5'*32,held='6'*32)]
    normal='schedule normal seed=7 threads=2\nworker 0 frames=8 MD5='+clips[0]['full']+'\nworker 1 frames=8 MD5='+clips[1]['full']+'\n'
    held=normal.replace('normal','failure').replace('worker 0 frames=8 MD5='+clips[0]['full'],
        'worker 0 frames=4 MD5='+clips[0]['prefix'])+'retained 0 MD5='+clips[0]['held']+' client_error=1\n'
    m.validate(normal,clips,'normal',7);m.validate(held,clips,'failure',7)
    bad=[normal.replace('1'*32,'0'*32),normal.replace('frames=8','frames=7',1),
         normal+normal.splitlines()[1]+'\n',normal.replace(normal.splitlines()[2]+'\n',''),
         normal.replace('1'*32,'4'*32),normal.replace('seed=7','seed=8'),
         held.replace('3'*32,'0'*32),held.replace('client_error=1','client_error=0'),
         held.replace(held.splitlines()[-1]+'\n',''),normal+'runtime error: fixture\n']
    for text in bad:
        try:m.validate(text,clips,'failure' if 'schedule failure' in text else 'normal',7)
        except ValueError:continue
        raise AssertionError('oracle mutation passed')
    return len(bad)


def process_tests(root):
    for label,commands,deadline,reason in [
        ('early',[[PYTHON,'-c','raise SystemExit(7)']],3,'child-error'),
        ('launch',[[PYTHON,'-c','import time;time.sleep(30)'],['/nonexistent-concurrent-va-worker']],3,'launch:'),
        ('deadline',[[PYTHON,'-c','import time;time.sleep(30)']],.15,'deadline'),
        ('overflow',[[PYTHON,'-c','import os,time;os.write(1,b"x"*1100000);time.sleep(30)']],3,'output-limit')]:
        start=time.monotonic();result=m.run_group(commands,root/label,deadline)
        require(not result['ok'] and result['reason'].startswith(reason),label+str(result))
        require(time.monotonic()-start<6,label+' cleanup not bounded')
    # Cancel this runner while it owns a real child and grandchild. Check both die.
    for escaped in (False,True):
        label='escaped' if escaped else 'cancel';pidfile=root/(label+'.pid');parentfile=root/(label+'-parent.pid')
        child="import os,time;from pathlib import Path;Path(%r).write_text(str(os.getpid()));time.sleep(30)"%str(pidfile)
        parent=("import os,subprocess,sys,time;from pathlib import Path;Path(%r).write_text(str(os.getpid()));"
                "subprocess.Popen([sys.executable,'-c',%r],start_new_session=%r);time.sleep(.3);%s") % (
                    str(parentfile),child,escaped,'raise SystemExit(7)' if escaped else 'time.sleep(30)')
        launch=("import importlib.util,sys;s=importlib.util.spec_from_file_location('r',%r);m=importlib.util.module_from_spec(s);"
                "s.loader.exec_module(m);r=m.run_group([[sys.executable,'-c',%r]],%r,5);sys.exit(0 if r['ok'] else 1)") % (
                    str(RUNNER),parent,str(root/label))
        proc=subprocess.Popen([PYTHON,'-c',launch],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        handle=None
        try:
            pid=wait_file(pidfile);parentpid=wait_file(parentfile)
            if hasattr(os,'pidfd_open'):handle=os.pidfd_open(pid)
            if not escaped:proc.send_signal(signal.SIGTERM)
            require(proc.wait(timeout=6)==1,label+' unexpectedly passed')
            result=json.loads((root/label/'result.json').read_text())
            require(result['reason']==('child-error' if escaped else 'cancelled'),label+str(result))
            require(exited(parentpid),label+' owned parent alive')
            if not escaped:
                limit=time.monotonic()+2
                while not exited(pid) and time.monotonic()<limit:time.sleep(.01)
                require(exited(pid),'owned descendant survived cancellation')
            # Detached processes are deliberately outside group ownership. The
            # failed parent + inherited stdout must not cause a pipe-EOF wait.
        finally:
            if handle is not None:
                try:signal.pidfd_send_signal(handle,signal.SIGKILL)
                except ProcessLookupError:pass
                os.close(handle)
            elif escaped:
                published=published_pid(pidfile)
                if published is not None:
                    try:os.kill(published,signal.SIGKILL)
                    except ProcessLookupError:pass
            if proc.poll() is None:proc.kill()
            proc.communicate(timeout=3)


def worker_tests(binary,root):
    clip=root/'eight.mkv'
    m.successful(['ffmpeg','-nostdin','-v','error','-f','lavfi','-i','testsrc2=size=128x96:rate=8',
                  '-frames:v','8','-c:v','libx264','-preset','ultrafast',clip],root/'encode')
    short=root/'short.mkv'
    m.successful(['ffmpeg','-nostdin','-v','error','-i',clip,'-frames:v','2','-c','copy',short],root/'shorten')
    bad=root/'bad.mkv';bad.write_bytes(b'not a video')
    fifo=root/'fifo';os.mkfifo(fifo)
    for i,args in enumerate([
        ['--threads','2',str(clip),'yuv420p'],[str(clip),'not-a-format'],
        ['--frames','5',str(clip),'yuv420p'],[str(fifo),'yuv420p'],[str(root/'missing'),'yuv420p'],
        ['--threads','2',str(clip),'yuv420p',str(bad),'yuv420p'],
        ['--schedule','teardown','--threads','2',str(clip),'yuv420p',str(short),'yuv420p']]):
        r=m.run_group([[str(binary),'software',*args]],root/f'invalid-{i}',5)
        require(not r['ok'] and r['reason']=='child-error','invalid input hung or passed: '+str(r))
        require(all('Sanitizer' not in x['text'] and 'runtime error:' not in x['text'] for x in r['records']),
                'sanitizer finding: '+str(r))
    # Stop the actual binary while blocked at the deliberate finite outer start
    # barrier. No FIFO decoder access, no unrelated sleep presented as a decode.
    r=m.run_group([[str(binary),'software','--wait-start',str(clip),'yuv420p']],root/'worker-eof',2)
    require(not r['ok'] and r['reason']=='child-error','start-barrier EOF accepted')
    # Give the actual binary an open but silent start pipe. The retained write
    # fd prevents EOF; the runner's finite deadline must kill/reap that worker.
    stalled="import os;r,w=os.pipe();os.dup2(r,0);os.set_inheritable(w,True);os.execv(%r,[%r,'software','--wait-start',%r,'yuv420p'])" % (str(binary),str(binary),str(clip))
    r=m.run_group([[PYTHON,'-c',stalled]],root/'worker-stalled',.3)
    require(not r['ok'] and r['reason']=='deadline','actual stalled worker passed')



if __name__=='__main__':
    mutations=oracle_tests()
    with tempfile.TemporaryDirectory(prefix='concurrent-va-negative-') as tmp:
        root=Path(tmp);pid_file_tests(root);process_tests(root)
        if len(sys.argv)>1:worker_tests(Path(sys.argv[1]).resolve(),root)
    worker_result='worker invalid/short-input paths' if len(sys.argv)>1 else 'worker binary checks not run'
    print(f'PASS: PID publication controls; {mutations} oracle mutations; owned launch/exit/deadline/output/cancel/descendant cleanup; {worker_result}')
