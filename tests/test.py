"""End-to-end filtering, terminal interaction, and live input tests."""
import fcntl
import os
import pty
import select
import signal
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
                        ('json | message ~ "time"', 1),
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
    def __init__(self, args, *, command=None, rows=28, columns=110):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        def setup():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        self.proc = subprocess.Popen(command if command is not None else [BIN] + args, stdin=slave, stdout=slave,
                                     stderr=slave, preexec_fn=setup,
                                     env={**os.environ, 'TERM': 'xterm-256color'})
        os.close(slave)
    def read(self, duration=.3):
        out = b''
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if select.select([self.master], [], [], .03)[0]:
                try:
                    chunk = os.read(self.master, 65536)
                    if not chunk:
                        break
                    out += chunk
                except OSError:
                    break
        return out
    def key(self, keys):
        os.write(self.master, keys)
        return self.read()
    def wait_for(self, expected, timeout=5):
        output = b''
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            output += self.read(min(.1, max(0, deadline - time.monotonic())))
            if expected in output:
                return output
            if self.proc.poll() is not None:
                break
        raise AssertionError(f'Terminal did not display {expected!r}; exit={self.proc.poll()}; output={output[-8192:]!r}')

    def wait_for_exit(self, timeout=5):
        output = b''
        deadline = time.monotonic() + timeout
        while self.proc.poll() is None:
            if time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(self.proc.args, timeout, output=output[-8192:])
            # Drain output while waiting: a full PTY can block rendering or endwin().
            output = (output + self.read(.05))[-8192:]
        return self.proc.returncode

    def close(self):
        try:
            if self.proc.poll() is None:
                self.key(b'q')
            assert self.wait_for_exit() == 0
        finally:
            if self.proc.poll() is None:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=3)
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
pipeline = ['sh', '-c', 'printf \'{"message":"PIPE_LIVE"}\\n\' | "$1"', 'sh', BIN]
terminal = Terminal([], command=pipeline, rows=24, columns=100)
try:
    terminal.wait_for(b'PIPE_LIVE')
    os.write(terminal.master, b'\r')
    terminal.wait_for(b'Log detail')
    os.write(terminal.master, b'q')
    terminal.wait_for(b'PIPE_LIVE')
finally:
    terminal.close()

# A child can emit more than the PTY buffer holds while shutting down.
# Waiting for exit must keep consuming output rather than block the writer.
flood = Terminal([], command=['python3', '-u', '-c',
    'import sys, tty; tty.setcbreak(0); print("DRAIN_READY", flush=True); '
    'assert sys.stdin.read(1) == "q"; sys.stdout.write("x" * (1024 * 1024)); sys.stdout.flush()'])
try:
    flood.wait_for(b'DRAIN_READY')
    os.write(flood.master, b'q')
    assert flood.wait_for_exit() == 0
finally:
    flood.close()

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

# Multiple inputs preserve argument order and each file's final partial record.
with tempfile.TemporaryDirectory() as directory:
    from pathlib import Path
    root = Path(directory)
    (root / 'nested').mkdir()
    first = root / 'a.log'
    second = root / 'nested' / 'b.log'
    first.write_text('first\n{"level":"error","message":"from first"}')
    second.write_text('{"level":"error","message":"from second"}\nlast')
    (root / 'no-extension').write_text('extensionless\n')
    os.link(first, root / 'hardlink')
    os.symlink(root, root / 'cycle')
    os.mkfifo(root / 'unused-fifo')

    def dump(*paths, query='', data=None, **kwargs):
        return subprocess.run([BIN, '--dump', '--filter', query, *map(str, paths)],
                              input=data, text=True, capture_output=True, timeout=5, **kwargs)

    expected = first.read_text() + '\n' + second.read_text() + '\n'
    result = dump(first, second)
    assert result.returncode == 0 and result.stdout == expected, (result.stdout, result.stderr)
    result = dump(second, first)
    assert result.returncode == 0 and result.stdout == second.read_text() + '\n' + first.read_text() + '\n'
    result = dump(root)
    assert result.returncode == 0 and result.stdout == expected + 'extensionless\n', (result.stdout, result.stderr)
    result = dump(root, first, second)
    assert result.returncode == 0 and result.stdout == expected + 'extensionless\n'

    for query in ('from', '~from (first|second)', 'json | level == "error"'):
        result = dump(root, query=query)
        assert result.returncode == 0 and len(result.stdout.splitlines()) == 2, result.stderr
        assert 'from first' in result.stdout and 'from second' in result.stdout

    result = dump(first, '-', data='stdin record\n')
    assert result.returncode == 0 and result.stdout == first.read_text() + '\nstdin record\n'
    assert dump(root / 'missing').returncode == 1
    assert dump(first, root / 'missing').returncode == 1

    empty = root / 'empty'
    empty.mkdir()
    assert dump(empty).stdout == ''
    special = root / '-dash'
    special.write_text('dash\n')
    result = subprocess.run([BIN, '--dump', '--', '-dash'], cwd=root,
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0 and result.stdout == 'dash\n'

# A directory opened empty discovers new files and subdirectories while running.
with tempfile.TemporaryDirectory() as directory:
    from pathlib import Path
    root = Path(directory)
    terminal = Terminal([str(root)])
    try:
        terminal.read(.2)
        (root / 'new').mkdir()
        active = root / 'new' / 'app.log'
        active.write_text('DIRECTORY_DISCOVERED\n')
        screen = terminal.read(.9)
        assert b'DIRECTORY_DISCOVERED' in screen and b'app.log' in screen
        with active.open('a') as file:
            file.write('DIRECTORY_APPENDED\n')
        assert b'DIRECTORY_APPENDED' in terminal.read(.5)
    finally:
        terminal.close()

print('Multiple-file, recursive-directory, deduplication, and discovery tests passed.')

# Existing inputs continue following independently, including partial lines and rotations.
with tempfile.TemporaryDirectory() as directory:
    from pathlib import Path
    root = Path(directory)
    a, b = root / 'a.log', root / 'b.log'
    a.write_text('PARTIAL_A')
    b.write_text('SECOND_B\n')
    terminal = Terminal([str(root)])
    try:
        screen = terminal.read(.5)
        assert b'PARTIAL_A' in screen and b'SECOND_B' in screen
        with a.open('a') as file:
            file.write('_COMPLETE\nAFTER_A\n')
        screen = terminal.read(.5)
        # Force a complete repaint; ncurses may only emit the changed suffix otherwise.
        screen += terminal.key(b'\x0c')
        screen += terminal.key(b'gg')
        screen += terminal.key(b'\r')
        assert b'PARTIAL_A_COMPLETE' in screen, screen
        terminal.key(b'q')
        terminal.key(b'G')

        with b.open('a') as file:
            file.write('APPEND_B\n')
        assert b'APPEND_B' in terminal.read(.5)
        a.rename(root / 'a.log.1')
        a.write_text('ROTATED_A\n')
        assert b'ROTATED_A' in terminal.read(.7)
        # Directory rescans must not import the rotated inode again.
        screen = terminal.key(b'/~PARTIAL_A|SECOND_B|AFTER_A|APPEND_B|ROTATED_A\r')
        screen += terminal.read(.6)
        screen += terminal.key(b'?')
        screen += terminal.key(b'q')
        assert b'5/5' in screen, screen
        terminal.key(b'c')
        a.write_text('X\n')
        assert b'X' in terminal.read(.5)
        # Historical records from the other file survive truncation of A.
        screen = terminal.key(b'/SECOND_B\r')
        screen += terminal.key(b'\r')
        assert b'SECOND_B' in screen
        terminal.key(b'q')
    finally:
        terminal.close()
print('Interleaved follow, partial records, rotation deduplication, and source-local truncation passed.')

# Directory search must cover every record of every file, beyond the old history cap.
with tempfile.TemporaryDirectory() as directory:
    from pathlib import Path
    root = Path(directory)
    expected = []
    for source in range(3):
        with (root / f'{source}.log').open('w') as file:
            for line in range(120003):
                if line in (0, 60001, 120002):
                    record = f'error timeout payment source={source} line={line}'
                    expected.append(record)
                else:
                    record = 'normal background record'
                file.write(record + '\n')
    result = subprocess.run([BIN, '--dump', '--filter', 'error AND timeout AND payment', str(root)],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0 and result.stdout.splitlines() == expected, result.stderr
print('Complete multi-file history search passed (360,009 records across three inputs).')

# Source tree scopes both browsing and queries without losing other sources' history.
with tempfile.TemporaryDirectory() as directory:
    from pathlib import Path
    root = Path(directory)
    (root / 'nested').mkdir()
    a, b = root / 'a.log', root / 'nested' / 'b.log'
    a.write_text('{"message":"TREE_ALPHA","level":"error"}\n')
    b.write_text('{"message":"TREE_BRAVO","level":"info"}\n')
    terminal = Terminal([str(root)])
    try:
        screen = terminal.read(.5)
        assert b'Sources' in screen and b'All sources' in screen and b'nested/' in screen
        assert b'a.log' in screen and b'b.log' in screen
        terminal.key(b'\tgj\r')  # All sources -> a.log -> select
        terminal.key(b'\t')       # back to log navigation
        screen = terminal.key(b'\r')
        assert b'TREE_ALPHA' in screen and b'TREE_BRAVO' not in screen
        terminal.key(b'q')
        terminal.key(b'/error\r')
        terminal.key(b'\tjj\r')  # nested directory -> b.log -> select
        screen = terminal.read(.3) + terminal.key(b'\t')
        # A filter remains active across source changes.
        screen += terminal.key(b'?') + terminal.key(b'q')
        assert b'No matching logs' in screen
        terminal.key(b'c')
        screen = terminal.key(b'\r')
        assert b'TREE_BRAVO' in screen and b'TREE_ALPHA' not in screen
        terminal.key(b'q')

        # Rotation keeps the same logical source selected.
        b.rename(b.with_suffix('.old'))
        b.write_text('{"message":"TREE_ROTATED"}\n')
        terminal.read(.5)
        screen = terminal.key(b'\r')
        assert b'TREE_ROTATED' in screen
        terminal.key(b'q')

        # A newly discovered file appears without changing the selected source.
        (root / 'new.log').write_text('TREE_NEW_FILE\n')
        screen = terminal.read(.7)
        assert b'new.log' in screen
        screen = terminal.key(b'\r')
        assert b'TREE_ROTATED' in screen and b'TREE_NEW_FILE' not in screen
        terminal.key(b'q')
        terminal.key(b'\ta\t')   # read all sources again
        screen = terminal.key(b'\r')
        assert b'TREE_NEW_FILE' in screen
        terminal.key(b'q')

        # Hidden tree and narrow terminals retain keyboard access to sources.
        terminal.key(b's')
        terminal.key(b'\t')
        fcntl.ioctl(terminal.master, termios.TIOCSWINSZ, struct.pack('HHHH', 12, 45, 0, 0))
        os.kill(terminal.proc.pid, __import__('signal').SIGWINCH)
        screen = terminal.read(.4)
        assert b'Sources' in screen and b'All sources' in screen
        terminal.key(b'\x1b')
    finally:
        terminal.close()
print('Source-tree selection, scoped filters, rotation, discovery, and narrow-terminal tests passed.')

# JSON expressions: substring spelling, chains, precedence, and quoted operators.
json_records = '\n'.join([
    '{"message":"error response 500","level":"error","http":{"status":500}}',
    '{"message":"error response 404","level":"error","http":{"status":404}}',
    '{"message":"warning response 500","level":"warn","http":{"status":500}}',
    '{"message":"error AND (500)","level":"info","http":{"status":"500"}}',
    '{"message":"ERROR response 500","level":"info"}',
    'not json',
]) + '\n'
for query, expected in [
    ('json | message ~ "error" AND message ~ "500"', 2),
    ('json | level == "error" AND http.status >= 500', 1),
    ('json | level == "error" OR level == "warn" AND http.status >= 500', 3),
    ('json | (level == "error" OR level == "warn") AND http.status >= 500', 2),
    ('json | missing != 1 OR message ~ "warning"', 1),
    ('json | missing != 1 AND message ~ "error"', 0),
    ('json | message ~ "error AND (500)"', 1),
    ('json | message ~ "\\u0065rror" AND message ~ "500"', 2),
    ('json | (message~"error" AND (http.status>=500 OR level==info))', 2),
    ('json | message ~ "error" AND message ~ "5.."', 0),
]:
    result = run(json_records, query)
    assert result.returncode == 0 and len(result.stdout.splitlines()) == expected, (query, result.stdout, result.stderr)

for query in [
    'json | message contains "error"',
    'json | message ~ "error" AND',
    'json | OR message ~ "error"',
    'json | (message ~ "error"',
    'json | message ~ "error")',
    'json | message ~ "error" message ~ "500"',
    'json | message ~ "error" and message ~ "500"',
    'json | message ~ "unfinished',
    'json | message == unquoted value',
    'json | message ~~ "error"',
    'json | ()',
    'json | ' + ' AND '.join(['x==1'] * 33),
    'json | ' + '(' * 33 + 'x==1' + ')' * 33,
]:
    assert run(json_records, query).returncode == 2, query
assert run('{"x":1}\n', 'json | ' + ' AND '.join(['x==1'] * 32)).stdout
assert run('{"x":1}\n', 'json | ' + '(' * 32 + 'x==1' + ')' * 32).stdout
print('Chained JSON conditions, ~ substring matching, precedence, and syntax errors passed.')

# Status totals stay global while filters and source selection restrict matches.
with tempfile.TemporaryDirectory() as directory:
    from pathlib import Path
    import signal
    root = Path(directory)
    a, b = root / 'a.log', root / 'b.log'
    a.write_text('error alpha\ninfo alpha\n')
    b.write_text('error bravo\ninfo bravo\n')
    terminal = Terminal([str(root)])
    width = 110

    def status_screen():
        global width
        width = 111 if width == 110 else 110
        fcntl.ioctl(terminal.master, termios.TIOCSWINSZ, struct.pack('HHHH', 28, width, 0, 0))
        os.kill(terminal.proc.pid, signal.SIGWINCH)
        return terminal.read(.35)

    try:
        screen = terminal.read(.4)
        assert b'Total indexed: 4' in screen
        assert b'Matched:' not in screen
        terminal.key(b'/error\r')
        screen = status_screen()
        assert b'Total indexed: 4' in screen and b'Matched: 2' in screen
        terminal.key(b'\tgj\r\t')
        screen = status_screen()
        assert b'Total indexed: 4' in screen and b'Matched: 1' in screen

        # An unmatched append to an unselected source still increases the total.
        with b.open('a') as file:
            file.write('background bravo\n')
        terminal.read(.3)
        screen = status_screen()
        assert b'Total indexed: 5' in screen and b'Matched: 1' in screen
        terminal.key(b'?')
        screen = status_screen()
        assert b'Total indexed: 5' in screen and b'Matched: 1' in screen
        terminal.key(b'q')
        terminal.key(b'c\ta\t')
        screen = status_screen()
        assert b'Total indexed: 5' in screen and b'Matched:' not in screen
        fcntl.ioctl(terminal.master, termios.TIOCSWINSZ, struct.pack('HHHH', 6, 24, 0, 0))
        os.kill(terminal.proc.pid, signal.SIGWINCH)
        screen = terminal.read(.35)
        assert b'Total: 5' in screen and b'Resize terminal' not in screen
    finally:
        terminal.close()
print('Global indexed totals, filtered matches, source scopes, live updates, and modal status passed.')
