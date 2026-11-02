"""Reject macOS binaries that depend on non-system dynamic libraries."""

import argparse
import subprocess


def non_system_libraries(output):
    libraries = [line.strip().split(' (', 1)[0] for line in output.splitlines()[1:] if line.strip()]
    return [library for library in libraries if not library.startswith(('/usr/lib/', '/System/Library/'))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    args = parser.parse_args()
    output = subprocess.check_output(['otool', '-L', args.binary], text=True)
    print(output, end='')
    unexpected = non_system_libraries(output)
    if unexpected:
        parser.exit(1, 'Non-system runtime dependencies: ' + ', '.join(unexpected) + '\n')


if __name__ == '__main__':
    main()
