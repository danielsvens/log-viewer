#!/usr/bin/env python3
"""Measure real terminal key response while a full JSON scan is active."""
import argparse
import fcntl
import json
import os
import pty
import select
import struct
import subprocess
import termios
import time

parser = argparse.ArgumentParser()
parser.add_argument('dataset')
parser.add_argument('--binary', default='./log-viewer')
parser.add_argument('--output', default='benchmarks/tui-results.json')
args = parser.parse_args()
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 32, 120, 0, 0))
def setup():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
proc = subprocess.Popen([args.binary, '--filter', 'json | http.status >= 500', args.dataset],
                        stdin=slave, stdout=slave, stderr=slave, preexec_fn=setup,
                        env={**os.environ, 'TERM': 'xterm-256color'})
os.close(slave)
def until(marker, timeout=5):
    data = b''
    end = time.monotonic()+timeout
    while time.monotonic()<end:
        if select.select([master], [], [], .01)[0]:
            data += os.read(master, 65536)
            if marker in data:
                return data
    raise AssertionError((marker, data[-2000:]))
latencies = []
try:
    screen = until(b'SEARCHING')
    assert b'SEARCHING' in screen, 'Expected a scan to be active'
    for _ in range(5):
        start = time.perf_counter()
        os.write(master, b'?')
        until(b'Keys & queries')
        latencies.append((time.perf_counter()-start)*1000)
        os.write(master, b'q')
        time.sleep(.03)
    # Supersede the in-progress search and confirm navigation reaches the old history.
    start = time.perf_counter()
    os.write(master, b'cgg')
    until(b'0000000000')
    cancel_and_jump_ms = (time.perf_counter()-start)*1000
    os.write(master, b'\rq')  # inspect then close detail
    time.sleep(.05)
    os.write(master, b'q')
    proc.wait(timeout=5)
    assert proc.returncode == 0
    assert max(latencies) < 250, latencies
    result = dict(dataset=args.dataset, key_response_ms=latencies,
                  max_key_response_ms=max(latencies), cancel_and_jump_ms=cancel_and_jump_ms)
finally:
    if proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=5)
    os.close(master)


# Measure the first actual rendered record, independently of query/footer text.
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 32, 120, 0, 0))
start = time.perf_counter()
proc = subprocess.Popen([args.binary, '--filter', 'RARE_MARKER', args.dataset],
                        stdin=slave, stdout=slave, stderr=slave, preexec_fn=setup,
                        env={**os.environ, 'TERM': 'xterm-256color'})
os.close(slave)
try:
    until(b'error timeout payment RARE_MARKER')
    result['first_visible_record_ms'] = (time.perf_counter()-start)*1000
    os.write(master, b'q')
    proc.wait(timeout=5)
    assert proc.returncode == 0
finally:
    if proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=5)
    os.close(master)
open(args.output, 'w').write(json.dumps(result, indent=2)+'\n')
print(json.dumps(result))
