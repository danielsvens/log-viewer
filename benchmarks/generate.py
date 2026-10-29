#!/usr/bin/env python3
"""Deterministic mixed JSON/text workload; writes in bounded batches."""
import argparse
import json
from pathlib import Path


def generate(path, lines):
    special = {0, lines // 2, lines - 1}
    with open(path, 'w') as output:
        batch = []
        for i in range(lines):
            rare = i in special
            message = ('error timeout payment RARE_MARKER' if rare else
                       ('error payment retry' if i % 10 == 0 else 'request completed payment'))
            message += ' x' * (i % 17)
            if i % 2:
                line = json.dumps({'id': i, 'level': 'error' if rare else 'info',
                                   'http': {'status': 503 if rare else 200}, 'message': message}, separators=(',', ':'))
            else:
                line = f'{i:010d} INFO service=checkout {message}'
            batch.append(line + '\n')
            if len(batch) == 8192:
                output.writelines(batch)
                batch.clear()
        output.writelines(batch)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('path', type=Path)
    parser.add_argument('--lines', type=int, default=1_000_000)
    args = parser.parse_args()
    generate(args.path, args.lines)
