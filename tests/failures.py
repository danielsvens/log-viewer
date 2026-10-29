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
