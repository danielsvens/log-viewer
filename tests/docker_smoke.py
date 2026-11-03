"""Optional real-Docker smoke test. Uses an already available image; never pulls.

Run: DOCKER_SMOKE_IMAGE=alpine:latest python3 tests/docker_smoke.py
"""
import os
import pty
import select
import subprocess
import time

BIN = os.path.abspath(os.environ.get('LOG_VIEWER_BIN', 'log-viewer'))
IMAGE = os.environ.get('DOCKER_SMOKE_IMAGE')
if not IMAGE:
    raise SystemExit('Set DOCKER_SMOKE_IMAGE to an installed image with sh and sleep.')


def docker(*args):
    return subprocess.check_output(['docker', *args], text=True).strip()


containers = []
try:
    for token in ('docker-smoke-api', 'docker-smoke-worker'):
        containers.append(docker('run', '-d', '--pull=never', IMAGE, 'sh', '-c',
                                 f'echo {token}-history; sleep 2; echo {token}-follow; sleep 2'))
    history = subprocess.run([BIN, '--dump', '--docker', containers[0], '--docker', containers[1]],
                             capture_output=True, text=True, timeout=15, check=True)
    assert 'docker-smoke-api-history' in history.stdout
    assert 'docker-smoke-worker-history' in history.stdout

    master, slave = pty.openpty()
    viewer = subprocess.Popen([BIN, '--docker', containers[0], '--docker', containers[1]],
                              stdin=slave, stdout=slave, stderr=slave,
                              env=dict(os.environ, TERM='xterm-256color'))
    os.close(slave)
    captured = bytearray()

    def read_until(token):
        deadline = time.monotonic() + 20
        while token not in captured and time.monotonic() < deadline:
            if select.select([master], [], [], 0.1)[0]:
                captured.extend(os.read(master, 65536))
        assert token in captured, captured.decode(errors='replace')

    try:
        read_until(b'docker-smoke-api-follow')
        read_until(b'docker-smoke-worker-follow')
        read_until(b'stopped')
        os.write(master, b'R')
        read_until(b'duplicates possible')
    finally:
        viewer.terminate()
        viewer.wait(timeout=5)
        os.close(master)
finally:
    for container in containers:
        subprocess.run(['docker', 'rm', '-f', container], capture_output=True, check=True)

print('Real Docker history, follow, container exit, simultaneous sources, and reconnect passed.')
