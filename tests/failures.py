"""Deterministic allocation failures in the linker-wrapped application build."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

failed = 0
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'input.log'
    path.write_text('{"http":{"status":503}}\n' * 1000)
    for allocation in range(1, 19):
        result = subprocess.run([sys.argv[1], '--dump', '--filter', 'json | http.status >= 500', str(path)],
                                env={**os.environ, 'LOG_VIEWER_FAIL_ALLOCATION': str(allocation)},
                                capture_output=True, timeout=5)
        assert result.returncode in (0, 1, 2), (allocation, result.stderr)
        assert b'Unreleased application allocations' not in result.stderr, (allocation, result.stderr)
        failed += result.returncode != 0
assert failed >= 8, 'Allocation injection did not exercise enough failure paths'
print(f'Allocation failure cleanup verified ({failed} failures injected).')

# Exercise failures throughout recursive discovery, owned path copies, and file setup.
failed = 0
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'nested').mkdir()
    for index in range(4):
        (root / 'nested' / f'{index}.log').write_text('{"http":{"status":503}}\n')
    for allocation in range(1, 81):
        result = subprocess.run([sys.argv[1], '--dump', '--filter', 'json | http.status >= 500', str(root)],
                                env={**os.environ, 'LOG_VIEWER_FAIL_ALLOCATION': str(allocation)},
                                capture_output=True, timeout=5)
        assert result.returncode in (0, 1, 2), (allocation, result.stderr)
        assert b'Unreleased application allocations' not in result.stderr, (allocation, result.stderr)
        failed += result.returncode != 0
assert failed >= 20, 'Directory allocation injection did not exercise enough failure paths'
print(f'Directory allocation failure cleanup verified ({failed} failures injected).')

# Restricted levels exercise the normalization/parser workspace and composed JSON query.
failed = 0
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'levels.log'
    path.write_text('{"level":"error","http":{"status":503}}\n' * 100)
    for allocation in range(1, 36):
        result = subprocess.run([sys.argv[1], '--dump', '--levels', 'error', '--filter', 'json | http.status >= 500', str(path)],
                                env={**os.environ, 'LOG_VIEWER_FAIL_ALLOCATION': str(allocation)},
                                capture_output=True, timeout=5)
        assert result.returncode in (0, 1, 2), (allocation, result.stderr)
        assert b'Unreleased application allocations' not in result.stderr, (allocation, result.stderr)
        failed += result.returncode != 0
assert failed >= 10, 'Level filtering allocation injection did not exercise enough failure paths'
print(f'Level filtering allocation failure cleanup verified ({failed} failures injected).')

# Docker setup, spools, indexing, and mixed-source cleanup under allocation failures.
failed = 0
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    docker = root / 'docker'
    docker.write_text('#!/bin/sh\nif [ "$1" = container ]; then\n'
                      "printf '%064d\\n' 0\nelse\nprintf '{\"message\":\"docker\"}\\n'\nfi\n")
    docker.chmod(0o700)
    path = root / 'local.log'
    path.write_text('{"message":"local"}\n')
    for allocation in range(1, 45):
        result = subprocess.run([sys.argv[1], '--dump', '--docker', 'api', '--docker', 'worker', str(path)],
                                env={**os.environ, 'PATH': directory + os.pathsep + os.environ['PATH'],
                                     'LOG_VIEWER_FAIL_ALLOCATION': str(allocation)},
                                capture_output=True, timeout=5)
        assert result.returncode in (0, 1, 2), (allocation, result.stderr)
        assert b'Unreleased application allocations' not in result.stderr, (allocation, result.stderr)
        failed += result.returncode != 0
assert failed >= 10, 'Docker allocation injection did not exercise enough failure paths'
print(f'Docker allocation failure cleanup verified ({failed} failures injected).')
