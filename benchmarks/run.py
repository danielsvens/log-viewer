#!/usr/bin/env python3
"""Warm/OS-managed-cache benchmarks. Never evicts the host's filesystem cache."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
from generate import generate

parser = argparse.ArgumentParser()
parser.add_argument('--lines', type=int, nargs='+', default=[1_000_000, 10_000_000])
parser.add_argument('--directory', type=Path, default=Path('/tmp/log-viewer-benchmark'))
parser.add_argument('--output', type=Path, default=Path('benchmarks/results.json'))
parser.add_argument('--baseline-bin', type=Path)
args = parser.parse_args()
args.directory.mkdir(parents=True, exist_ok=True)
results = {'cache': 'OS-managed; repeated generated-file reads (warm, not forced cold)', 'runs': []}
for lines in args.lines:
    path = args.directory / f'{lines}.log'
    if not path.exists():
        generate(path, lines)
    for query in ['RARE_MARKER', 'payment', 'error AND timeout AND payment', 'json | http.status >= 500']:
        row = json.loads(subprocess.check_output(['build/release/bench', str(path), query], text=True))
        row.update(query=query, dataset=str(path))
        assert row['lines'] == lines
        expected = 3 if query in ['RARE_MARKER', 'error AND timeout AND payment'] else lines if query == 'payment' else 1
        assert row['matches'] == expected, row
        if expected == 3:
            assert row['sample_ids'] == [1, lines // 2 + 1, lines]
        results['runs'].append(row)
        print(json.dumps(row), flush=True)
        args.output.write_text(json.dumps(results, indent=2) + '\n')
    if args.baseline_bin:
        for query in ['RARE_MARKER', 'error AND timeout AND payment']:
            start = time.perf_counter()
            proc = subprocess.Popen([str(args.baseline_bin.resolve()), '--dump', '--filter', query, str(path)], stdout=subprocess.DEVNULL)
            _, status, usage = os.wait4(proc.pid, 0)
            proc.returncode = os.waitstatus_to_exitcode(status)
            assert proc.returncode == 0
            row = dict(dataset=str(path), query=query, seconds=time.perf_counter()-start,
                       peak_rss_kib=usage.ru_maxrss, baseline=True, mode='original --dump (streamed, no retained history)')
            results['runs'].append(row)
            print(json.dumps(row), flush=True)
            args.output.write_text(json.dumps(results, indent=2) + '\n')
