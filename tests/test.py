"""End-to-end filtering, terminal interaction, and live input tests."""
import fcntl
import os
import pty
import select
import struct
import subprocess
import tempfile
import termios
import time

BIN = os.path.abspath(os.environ.get('LOG_VIEWER_BIN', 'log-viewer'))

def run(data, query=''):
    return subprocess.run([BIN, '--dump', '--filter', query], input=data,
                          text=True, capture_output=True)

# Resources are compiled into the binary, with an explicit C-string terminator.
with open('resources/usage.txt', 'rb') as resource:
    expected_usage = resource.read() + b'\n'
with tempfile.TemporaryDirectory() as directory:
    result = subprocess.run([BIN, '--help'], cwd=directory, capture_output=True)
    assert result.returncode == 0 and result.stdout == expected_usage
    assert b'\0' not in result.stdout


records = '{"level":"error","http":{"status":503},"message":"timeout"}\n{"level":"info","http":{"status":200}}\nplain warning\n'
for query, expected in [('', 3), ('ERROR', 1), ('~error|warning', 2),
                        ('json | level == "error"', 1),
                        ('json | http.status >= 500', 1),
                        ('json | message contains "time"', 1),
                        ('json | missing != 3', 0)]:
    result = run(records, query)
    assert result.returncode == 0, result.stderr
    assert len(result.stdout.splitlines()) == expected, (query, result.stdout)
for malformed in ['{"x":01}', '{"x":true,}', '{"x":"\\u12"}', '[1,]', '{"x":NaN}']:
    assert run(malformed, 'json | x == true').stdout == ''
assert run('last without newline').stdout == 'last without newline\n'
assert run('\x1b[31merror\x1b[0m\n').stdout == 'error\n'
assert run(records, '~[').returncode == 2
assert run(records, 'json | bad').returncode == 2
assert run('{"message":"\\u0065rror"}\n', 'json | message == "error"').stdout
assert run('{"message":"error"}\n', 'json | message == "\\u0065rror"').stdout
assert not run('[' * 200 + '0' + ']' * 200, 'json | x == 1').stdout


# If terminal initialization fails, borrowed stdin flags must still be restored.
reader, writer = os.pipe()
try:
    flags = fcntl.fcntl(reader, fcntl.F_GETFL)
    result = subprocess.run([BIN], stdin=reader, capture_output=True,
                            start_new_session=True, timeout=3)
    assert result.returncode == 1, result.stderr
    assert fcntl.fcntl(reader, fcntl.F_GETFL) == flags
finally:
    os.close(reader)
    os.close(writer)


class Terminal:
    def __init__(self, args):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 28, 110, 0, 0))
        def setup():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        self.proc = subprocess.Popen([BIN] + args, stdin=slave, stdout=slave,
                                     stderr=slave, preexec_fn=setup,
                                     env={**os.environ, 'TERM': 'xterm-256color'})
        os.close(slave)
    def read(self, duration=.3):
        out = b''
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if select.select([self.master], [], [], .03)[0]:
                try:
                    out += os.read(self.master, 65536)
                except OSError:
                    break
        return out
    def key(self, keys):
        os.write(self.master, keys)
        return self.read()
    def close(self):
        self.key(b'q')
        self.proc.wait(timeout=3)
        assert self.proc.returncode == 0
        os.close(self.master)

with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, 'app.log')
    with open(path, 'w') as f:
        f.write(records)
    t = Terminal([path])
    assert b'LOG VIEWER' in t.read()
    help_screen = t.key(b'?')
    assert b'Keys & queries' in help_screen and b'Move selection' in help_screen
    t.key(b'q')
    t.key(b'gg')
    screen = t.key(b'\r')
    assert b'Log detail' in screen and b'503' in screen, screen
    with open(path, 'a') as f:
        f.write('LIVE_NEW_RECORD\n')
    t.read()
    t.key(b'q')
    assert b'LIVE_NEW_RECORD' in t.key(b'G')
    assert b'No matching logs' in t.key(b'/NEVER_MATCH')
    assert b'LIVE_NEW_RECORD' in t.key(b'\x1b')
    os.rename(path, path + '.old')
    with open(path, 'w') as f:
        f.write('ROTATED_RECORD\n')
    assert b'ROTATED_RECORD' in t.read(.5)
    with open(path, 'w') as f:
        f.write('SHORT\n')
    assert b'SHORT' in t.read(.5)
    t.close()

# Pipeline has separate controlling terminal input, including after producer EOF.
with tempfile.TemporaryDirectory() as directory:
    fifo = os.path.join(directory, 'stream')
    os.mkfifo(fifo)
    # A shell pipeline reproduces real stdin versus /dev/tty routing.
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
    def setup():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    proc = subprocess.Popen(['sh', '-c', 'printf \'{"message":"PIPE_LIVE"}\\n\' | "$1"', 'sh', BIN],
                            stdin=slave, stdout=slave, stderr=slave, preexec_fn=setup,
                            env={**os.environ, 'TERM': 'xterm-256color'})
    os.close(slave)
    time.sleep(.2)
    output = os.read(master, 65536)
    assert b'PIPE_LIVE' in output
    os.write(master, b'\rq')  # detail then close
    time.sleep(.1)
    os.write(master, b'q')
    proc.wait(timeout=3)
    assert proc.returncode == 0
    os.close(master)
print('All filtering, JSON, live file, rotation, modal, and pipe tests passed.')

# Giant records cross ingestion/read windows; no 1 MiB truncation is allowed.
long_record = 'a' * (1024 * 1024 - 3) + 'error timeout' + 'b' * (1024 * 1024) + ' payment'
result = run(long_record, 'error AND timeout AND payment')
assert result.returncode == 0 and result.stdout == long_record + '\n'
assert run('', 'error AND timeout').stdout == ''
assert run('error timeout payment\nerror payment\n', 'error AND timeout AND payment').stdout == 'error timeout payment\n'
# ANSI CSI and a search term can each span a streaming search chunk boundary.
long_ansi = 'x' * (1024 * 1024 - 1) + '\x1b[31merror\x1b[0m' + 'y' * (1024 * 1024) + ' timeout payment'
result = run(long_ansi, 'error AND timeout AND payment')
assert result.returncode == 0 and 'error' in result.stdout and '\x1b' not in result.stdout
print('Long-record, window-boundary, empty-input, and AND regression tests passed.')


# Redirected regular stdin respects and advances its shared file position.
with tempfile.TemporaryFile() as file:
    file.write(b'skip\nkeep\n')
    file.seek(5)
    result = subprocess.run([BIN, '--dump'], stdin=file, capture_output=True)
    assert result.returncode == 0 and result.stdout == b'keep\n'
    assert file.tell() == 10

assert run('foo\n foo \n', ' foo ').stdout == ' foo \n'
assert run('aaa\nbbb\n', '~^bbb$').stdout == 'bbb\n'

assert run('data\n', 'x' * 512).returncode == 2
