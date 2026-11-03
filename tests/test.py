"""Headless command-line filtering, complete history, and input tests."""
import fcntl
import os
import subprocess
import tempfile

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


print('All headless CLI, input, directory, query, and history tests passed.')

# Level masks recognize metadata instead of message substrings and compose with queries.
import json
level_records = [
    {'id': 1, 'level': 'error', 'message': 'timeout'},
    {'id': 2, 'severity': 'WARNING', 'message': 'timeout'},
    {'id': 3, 'level': 'INFO', 'message': 'error timeout'},
    {'id': 4, 'log': {'level': 'debug'}, 'message': 'timeout'},
    {'id': 5, 'message': 'error timeout'},
    {'id': 6, 'PRIORITY': '3', 'message': 'timeout'},
    {'id': 7, 'severityNumber': 9, 'message': 'timeout'},
    {'id': 8, 'log.level': 'TRACE', 'message': 'timeout'},
]
level_data = ''.join(json.dumps(row) + '\n' for row in level_records)
for levels, expected in [('all', list(range(1, 9))), ('error', [1, 6]), ('warn', [2]),
                         ('info', [3, 7]), ('debug', [4, 8]), ('unknown', [5]),
                         ('error,warn', [1, 2, 6]), ('none', [])]:
    result = subprocess.run([BIN, '--dump', '--levels', levels], input=level_data,
                            text=True, capture_output=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert [json.loads(line)['id'] for line in result.stdout.splitlines()] == expected, (levels, result.stdout)

for query in ['timeout', '~timeout', 'json | message ~ "timeout" AND id >= 3']:
    result = subprocess.run([BIN, '--dump', '--levels', 'info', '--filter', query],
                            input=level_data, text=True, capture_output=True, timeout=5)
    assert result.returncode == 0 and [json.loads(line)['id'] for line in result.stdout.splitlines()] == [3, 7]

for levels in ['', 'error,', 'error,,warn', 'oops']:
    result = subprocess.run([BIN, '--dump', '--levels', levels], input='', text=True, capture_output=True)
    assert result.returncode == 2
assert subprocess.run([BIN, '--levels'], capture_output=True).returncode == 2
result = subprocess.run([BIN, '--dump', '--levels', 'error'], input='\x1b[31mERROR\x1b[0m: failed\nINFO error\n',
                        text=True, capture_output=True)
assert result.returncode == 0 and result.stdout == 'ERROR: failed\n'
print('Structured and plain-text level detection, masks, aliases, and query composition passed.')

# Structured levels beyond the viewport preview are still evaluated in full.
large_level_record = json.dumps({'message': 'x' * (1024 * 1024 + 10) + ' needle', 'severity': 'ERROR'})
result = subprocess.run([BIN, '--dump', '--levels', 'error', '--filter', 'needle'],
                        input=large_level_record, text=True, capture_output=True, timeout=10)
assert result.returncode == 0 and result.stdout == large_level_record + '\n'

# Docker CLI is faked so the default suite needs neither Docker nor a daemon.
with tempfile.TemporaryDirectory() as directory:
    docker = os.path.join(directory, 'docker')
    calls = os.path.join(directory, 'calls')
    with open(docker, 'w') as script:
        script.write('''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
with open(os.environ['DOCKER_CALLS'], 'a') as out:
    out.write(json.dumps(args) + '\\n')
if args[:2] == ['container', 'inspect']:
    if args[-1] in ('removed', 'daemon'):
        print('No such container', file=sys.stderr)
        sys.exit(1)
    if args[-1] == 'warning':
        print('Docker warning', file=sys.stderr, flush=True)
    print('a' * 64)
else:
    assert args[-1] == 'a' * 64
    assert '--follow' not in args
    if os.environ.get('DOCKER_LARGE'):
        sys.stdout.write(('large record ' + 'x' * 100 + '\\n') * 20000)
    print('stdout message', flush=True)
    print('stderr message', file=sys.stderr, flush=True)
    sys.stdout.write('partial')
    sys.exit(int(os.environ.get('DOCKER_FAIL', '0')))
''')
    os.chmod(docker, 0o700)
    env = dict(os.environ, PATH=directory + os.pathsep + os.environ['PATH'], DOCKER_CALLS=calls)
    import json
    for tail in ('1000', '0', '5000', 'all'):
        args = [BIN, '--dump', '--docker', '--option;$(touch injected)']
        if tail != '1000':
            args += ['--docker-tail', tail]
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == ['stdout message', 'stderr message', 'partial']
        with open(calls) as file:
            recorded = [json.loads(line) for line in file]
        assert recorded[-2] == ['container', 'inspect', '--format', '{{.Id}}', '--', '--option;$(touch injected)']
        assert recorded[-1] == ['logs', '--tail', tail, '--', 'a' * 64]

    result = subprocess.run([BIN, '--dump', '--docker', 'warning'], env=env, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([BIN, '--dump', '--docker', 'api'], env=dict(env, DOCKER_LARGE='1', DOCKER_FAIL='9'),
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 1 and result.stdout.count('large record ') == 20000
    assert result.stdout.endswith('partial\n')

    for invalid in ('-1', '+1', '', '1.5', 'ALL', '1x'):
        result = subprocess.run([BIN, '--dump', '--docker', 'api', '--docker-tail', invalid], env=env, capture_output=True)
        assert result.returncode == 2

    file_path = os.path.join(directory, 'application.log')
    with open(file_path, 'w') as file:
        file.write('local message\n')
    result = subprocess.run([BIN, '--dump', '--docker', 'api', '--docker', 'worker', file_path, '-'],
                            input='stdin message\n', text=True, env=env, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert result.stdout.count('stdout message\n') == 2
    assert 'local message\n' in result.stdout and 'stdin message\n' in result.stdout

    for failed_env, container in ((dict(env, DOCKER_FAIL='7'), 'api'), (env, 'removed'), (env, 'daemon'), (dict(env, PATH='/nonexistent'), 'api')):
        result = subprocess.run([BIN, '--dump', '--docker', container, file_path], env=failed_env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 1
        assert 'local message\n' in result.stdout
        if container == 'api' and failed_env.get('DOCKER_FAIL'):
            assert 'partial\n' in result.stdout
print('Docker arguments, tail limits, mixed inputs, output draining, and connector failures passed.')
