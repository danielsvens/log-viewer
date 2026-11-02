"""Optional pseudo-terminal interaction tests; run with make tui-test."""
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
records = '{"level":"error","http":{"status":503},"message":"timeout"}\n{"level":"info","http":{"status":200}}\nplain warning\n'

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
    assert b'No matching logs' not in t.key(b'/NEVER_MATCH')
    t.key(b'\x1b')
    assert b'No matching logs' in t.key(b'/NEVER_MATCH\r')
    assert b'LIVE_NEW_RECORD' in t.key(b'c')
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

# Digit toggles change level scope; zero restores the complete history.
with tempfile.TemporaryDirectory() as directory:
    from pathlib import Path
    path = Path(directory) / 'levels.log'
    path.write_text('{"level":"error","message":"alpha"}\n'
                    '{"level":"warn","message":"beta"}\n'
                    '{"level":"info","message":"error gamma"}\n'
                    '{"level":"debug","message":"delta"}\n'
                    '{"message":"unknown"}\n')
    terminal = Terminal([str(path)], columns=140)
    columns = 140

    def level_screen():
        global columns
        columns = 141 if columns == 140 else 140
        fcntl.ioctl(terminal.master, termios.TIOCSWINSZ, struct.pack('HHHH', 28, columns, 0, 0))
        os.kill(terminal.proc.pid, signal.SIGWINCH)
        return terminal.read(.3)

    try:
        assert b'Total indexed: 5' in terminal.wait_for(b'Total indexed: 5')
        for key, count in [(b'1', 4), (b'2', 3), (b'3', 2), (b'4', 1), (b'5', 0)]:
            terminal.key(key)
            screen = level_screen()
            assert b'Total indexed: 5' in screen and f'Matched: {count}'.encode() in screen, screen
        terminal.key(b'0')
        screen = level_screen()
        assert b'Total indexed: 5' in screen and b'Matched:' not in screen
        terminal.key(b'3')
        terminal.key(b'/500\r')  # Query digits must not change the mask.
        screen = level_screen()
        assert b'1:error+ 2:warn+ 3:info- 4:debug+ 5:unknown+' in screen
        terminal.key(b'c')
        terminal.key(b'0')
    finally:
        terminal.close()
print('Level toggles, unknown visibility, restoring all levels, and query editing passed.')
