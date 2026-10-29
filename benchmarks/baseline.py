#!/usr/bin/env python3
"""Build the original storage path in a temporary directory, leaving the worktree alone."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('dataset')
parser.add_argument('--revision', default='b53937f')
parser.add_argument('--output', default='benchmarks/baseline-results.json')
args = parser.parse_args()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    archive = root / 'baseline.tar'
    subprocess.run(['git', 'archive', '--output', str(archive), args.revision], check=True)
    subprocess.run(['tar', '-xf', str(archive), '-C', directory], check=True)
    binary = root / 'baseline'
    sources = [root / 'src' / f'{module}.c' for module in ['json_support', 'log_store', 'query', 'source', 'text']]
    subprocess.run(['gcc', '-std=c23', '-O2', '-D_GNU_SOURCE', '-D_XOPEN_SOURCE=700',
                    '-I'+str(root / 'include'), 'benchmarks/baseline.c', *map(str, sources), '-o', str(binary)], check=True)
    result = json.loads(subprocess.check_output([str(binary), args.dataset], text=True))
    result.update(revision=args.revision, dataset=args.dataset,
                  note='Original retained-history path; rare query searches only the retained window; AND is unsupported.')
    Path(args.output).write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))
