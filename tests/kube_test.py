"""Fake kubectl history and asynchronous lifecycle checks; no cluster required."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import pty
import select
import signal
import fcntl
import termios
import struct

BIN = os.path.abspath(os.environ.get('LOG_VIEWER_BIN', 'log-viewer'))
FAKE = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
args = sys.argv[1:]
root = Path(os.environ['KUBE_FIXTURE'])
with (root / 'calls').open('a') as f:
    f.write(json.dumps([os.getpid(), args]) + '\n')
state = json.loads((root / 'state').read_text())
if args[:2] == ['config', 'view']:
    print(json.dumps(state['config']))
    sys.exit(0)
if state.get('timeout') and 'get' in args:
    time.sleep(20)
if state.get('denied') and 'get' in args:
    print('Forbidden: selected workload', file=sys.stderr)
    sys.exit(1)
if state.get('malformed') and 'get' in args:
    print('{"metadata":')
    sys.exit(0)
if 'get' in args:
    i = args.index('get')
    kind = args[i+1]
    if kind == 'deployment': result = state['deployment']
    elif kind == 'replicasets': result = {'items': state['replicasets']}
    elif kind == 'pods': result = {'items': state['pods']}
    elif kind == 'pod':
        result = next((p for p in state['pods'] if p['metadata']['name'] == args[i+2]), None)
        if result is None: sys.exit(1)
    if state.get('recreate_on_verify') and kind == 'pod':
        result['metadata']['uid'] = 'different'
    print(json.dumps(result))
else:
    pod = args[args.index('logs')+1]
    container = args[args.index('--container')+1]
    print('kubectl warning is not a log', file=sys.stderr, flush=True)
    tail = args[args.index('--tail')+1]
    if tail != '0':
        print(pod + '/' + container + ':' + state.get('token', 'history'), flush=True)
        print('partial', end='', flush=True)
    if state.get('logs_fail'): sys.exit(1)
    if '--follow' in args:
        while True: time.sleep(1)
'''


def owner(kind, uid):
    return [{'kind': kind, 'uid': uid, 'controller': True}]


def pod(name, uid, rs='rs1', pending=False, restart=0):
    return {'metadata': {'name': name, 'uid': uid, 'ownerReferences': owner('ReplicaSet', rs)},
            'spec': {'containers': [{'name': 'app'}, {'name': 'sidecar'}],
                     'initContainers': [{'name': 'init'}], 'ephemeralContainers': [{'name': 'debug'}]},
            'status': {'containerStatuses': [
                {'name': c, 'containerID': f'container://{uid}-{restart}-{c}', 'restartCount': restart,
                 'state': {'waiting': {}} if pending else {'running': {}}}
                for c in ('app', 'sidecar')]}}


BASE = {'config': {'current-context': 'local', 'contexts': [
            {'name': 'local', 'context': {'namespace': 'development'}},
            {'name': 'staging', 'context': {'namespace': 'production'}}]},
        'deployment': {'metadata': {'name': 'api', 'uid': 'deployment1'},
                       'spec': {'selector': {'matchLabels': {'app': 'api'}}}},
        'replicasets': [{'metadata': {'name': 'api-old', 'uid': 'rs1', 'ownerReferences': owner('Deployment', 'deployment1')}},
                        {'metadata': {'name': 'api-new', 'uid': 'rs2', 'ownerReferences': owner('Deployment', 'deployment1')}}],
        'pods': [pod('api-a', 'pod1'), pod('api-b', 'pod2', 'rs2'), pod('unrelated', 'pod3', 'other')]}

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'kubectl').write_text(FAKE)
    (root / 'kubectl').chmod(0o700)
    env = {**os.environ, 'PATH': directory + os.pathsep + os.environ['PATH'], 'KUBE_FIXTURE': directory}

    def state(value):
        (root / 'next').write_text(json.dumps(value))
        (root / 'next').replace(root / 'state')

    def calls():
        return [json.loads(line)[1] for line in (root / 'calls').read_text().splitlines()]

    def run(*args, fixture=None, expected=0, timeout=15):
        state(copy.deepcopy(BASE) if fixture is None else fixture)
        (root / 'calls').write_text('')
        result = subprocess.run([BIN, '--dump', *args], env=env, input='stdin-record\n', text=True, capture_output=True, timeout=timeout)
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        assert 'kubectl warning' not in result.stdout
        return result

    r = run('--kube', 'deployment/api')
    assert all(f'{p}/{c}:history' in r.stdout for p in ('api-a', 'api-b') for c in ('app', 'sidecar')), r
    assert 'unrelated' not in r.stdout and 'init' not in r.stdout and 'debug' not in r.stdout
    assert r.stdout.count('partial') == 4
    assert len([a for a in calls() if 'logs' in a]) == 4
    assert all('--follow' not in a for a in calls())
    for a in calls()[1:]:
        assert a[a.index('--context')+1] == 'local' and a[a.index('--namespace')+1] == 'development', a
    assert '--selector' in next(a for a in calls() if 'replicasets' in a)
    r = run('--kube', 'deploy/api', '--kube', 'pod/api-a', '-')
    assert r.stdout.count('api-a/app:history') == 1 and 'stdin-record' in r.stdout
    assert len([a for a in calls() if 'logs' in a]) == 4
    run('--kube', 'pod/api-a', '--kube-context', 'staging', '--kube-tail', 'all')
    for a in calls()[1:]:
        assert a[a.index('--context')+1] == 'staging' and a[a.index('--namespace')+1] == 'production'
    assert all(a[a.index('--tail')+1] == '-1' for a in calls() if 'logs' in a)
    assert not run('--kube', 'pod/api-a', '--kube-tail', '0').stdout
    run('--kube', 'pod/api-a', '--kube-namespace', 'override')
    assert all(a[a.index('--namespace')+1] == 'override' for a in calls()[1:])
    empty = copy.deepcopy(BASE); empty['pods'] = []
    assert not run('--kube', 'deployment/api', fixture=empty).stdout
    pending = copy.deepcopy(BASE); pending['pods'] = [pod('api-a', 'pod1', pending=True)]
    run('--kube', 'deployment/api', fixture=pending, expected=1)
    assert not any('logs' in a for a in calls())
    for failure in ('denied', 'malformed', 'recreate_on_verify', 'logs_fail', 'timeout'):
        fixture = copy.deepcopy(BASE); fixture[failure] = True
        run('--kube', 'deployment/api', fixture=fixture, expected=1)
    for arg in ('api', 'service/api', 'pod/', 'deployment/api/x', 'deployment/--all-namespaces', 'pod/.bad', 'pod/a..b'):
        run('--kube', arg, expected=2)
    run('--kube', 'pod/api-a', '--kube-tail', '-1', expected=2)

    driver = os.environ.get('KUBE_TEST_BIN')
    if driver:
        single = copy.deepcopy(BASE); single['pods'] = [pod('api-a', 'pod1')]
        state(single)
        (root / 'scaled').write_text(json.dumps(BASE))
        restart = copy.deepcopy(BASE); restart['pods'][0] = pod('api-a', 'pod1', restart=1); restart['token'] = 'restarted'
        (root / 'restarted').write_text(json.dumps(restart))
        removed = copy.deepcopy(BASE); removed['pods'] = []
        (root / 'removed').write_text(json.dumps(removed))
        subprocess.run([os.path.abspath(driver)], env=env, check=True, timeout=25)

    # A missing CLI must finish even with multiple attached targets and no sources.
    (root / 'kubectl').rename(root / 'disabled-kubectl')
    try:
        run('--kube', 'deployment/api', '--kube', 'pod/api-a', expected=1)
    finally:
        (root / 'disabled-kubectl').rename(root / 'kubectl')

    expressions = copy.deepcopy(BASE)
    expressions['deployment']['spec']['selector']['matchExpressions'] = [
        {'key': 'tier', 'operator': 'In', 'values': ['api', 'worker']},
        {'key': 'retired', 'operator': 'DoesNotExist'}]
    run('--kube', 'deployment/api', fixture=expressions)
    selector = next(a[a.index('--selector')+1] for a in calls() if 'replicasets' in a)
    assert selector == 'app=api,tier in (api,worker),!retired', selector

    # Follow: pending -> available -> scale/rollout -> restart -> retained history.
    state(pending)
    (root / 'calls').write_text('')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 35, 160, 0, 0))
    def setup_terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    viewer = subprocess.Popen([BIN, '--kube', 'deployment/api'], stdin=slave, stdout=slave, stderr=slave,
                              env={**env, 'TERM': 'xterm-256color'}, preexec_fn=setup_terminal)
    os.close(slave)
    captured = bytearray()
    def wait_until(predicate, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if select.select([master], [], [], .05)[0]:
                try: captured.extend(os.read(master, 65536))
                except OSError: break
            if predicate(): return
            assert viewer.poll() is None, captured[-4096:]
        raise AssertionError(captured[-4096:])
    try:
        wait_until(lambda: b'waiting' in captured)
        assert not any('logs' in a for a in calls())
        active = copy.deepcopy(BASE); active['pods'] = [pod('api-a', 'pod1')]
        state(active)
        wait_until(lambda: len([a for a in calls() if 'logs' in a]) == 2)
        wait_until(lambda: b'history' in captured)
        state(copy.deepcopy(BASE))
        wait_until(lambda: len([a for a in calls() if 'logs' in a]) == 4)
        restarted = copy.deepcopy(BASE); restarted['pods'][0] = pod('api-a', 'pod1', restart=1); restarted['token'] = 'restarted'
        state(restarted)
        wait_until(lambda: len([a for a in calls() if 'logs' in a]) == 6)
        wait_until(lambda: b'duplicates' in captured)
        # Discovery failures retain all previously attached sources.
        denied = copy.deepcopy(restarted); denied['denied'] = True
        state(denied)
        wait_until(lambda: b'Forbidden' in captured)
        # R in the query editor is text, not a refresh or reconnect action.
        count = len([a for a in calls() if 'logs' in a])
        os.write(master, b'/R')
        time.sleep(.1)
        assert len([a for a in calls() if 'logs' in a]) == count
        os.write(master, b'\x1b')
        # UID recreation must leave the pinned target in error.
        recreated = copy.deepcopy(restarted); recreated['deployment']['metadata']['uid'] = 'replacement'
        state(recreated)
        os.write(master, b'R')
        wait_until(lambda: b'UID changed' in captured)
    finally:
        viewer.terminate()
        viewer.wait(timeout=5)
        os.close(master)
    # Disconnected readers retry automatically; R overrides their backoff.
    broken = copy.deepcopy(BASE); broken['pods'] = [pod('api-a', 'pod1')]; broken['logs_fail'] = True
    state(broken)
    (root / 'calls').write_text('')
    captured = bytearray()
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 35, 160, 0, 0))
    viewer = subprocess.Popen([BIN, '--kube', 'deployment/api'], stdin=slave, stdout=slave, stderr=slave,
                              env={**env, 'TERM': 'xterm-256color'}, preexec_fn=setup_terminal)
    os.close(slave)
    try:
        wait_until(lambda: len([a for a in calls() if 'logs' in a]) >= 4)
        recovered = copy.deepcopy(broken); recovered['logs_fail'] = False; recovered['token'] = 'recovered'
        state(recovered)
        os.write(master, b'R')
        wait_until(lambda: b'recovered' in captured)
        wait_until(lambda: b'duplicates' in captured)
    finally:
        viewer.terminate()
        viewer.wait(timeout=5)
        os.close(master)

    # Every viewer-owned fake kubectl process was reaped/terminated at shutdown.
    for line in (root / 'calls').read_text().splitlines():
        pid = json.loads(line)[0]
        try: os.kill(pid, 0)
        except ProcessLookupError: continue
        raise AssertionError(f'kubectl process {pid} survived shutdown')
    if os.environ.get('KUBE_ALLOCATIONS'):
        state(BASE)
        failed = 0
        for allocation in range(1, 121):
            result = subprocess.run([BIN, '--dump', '--kube', 'deployment/api', '--kube', 'pod/api-a'],
                env={**env, 'LOG_VIEWER_FAIL_ALLOCATION': str(allocation)}, capture_output=True, timeout=8)
            assert result.returncode in (0, 1, 2), (allocation, result.stderr)
            assert b'Unreleased application allocations' not in result.stderr, (allocation, result.stderr)
            failed += result.returncode != 0
        assert failed >= 40, failed
        print(f'Kubernetes allocation failure cleanup passed ({failed} failures injected).')
print('Kubernetes fake CLI history, ownership, overlapping inputs, lifecycle, UID pinning, errors, and cleanup passed.')
