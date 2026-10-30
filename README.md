# log-viewer

A C23 terminal log explorer with file-backed history, background search, syntax
coloring, and a scrollable JSON detail modal

## Downloads and automated releases

Download binaries from [GitHub Releases](https://github.com/danielsvens/log-viewer/releases).
Every push to `main`, including a merged pull request, runs debug and release
core/integration tests plus allocation-cleanup and failure-path checks. A release
is published only after they pass. Pull requests targeting `main` run the same
checks without publishing a release.

Releases are tagged `build-<workflow run number>` and target the exact tested
commit. Each contains a Linux x86-64 binary, a `.tar.gz` archive with documentation
and licenses, and `SHA256SUMS`. The archive preserves executable permissions;
when downloading the standalone binary, run `chmod +x log-viewer-linux-x86_64`.
Use `sha256sum --check SHA256SUMS` with both downloaded assets to verify them.

These are dynamically linked builds made with GCC 16 on Debian 13 (trixie).
They require a compatible Linux x86-64 system with glibc 2.41 or newer and
ncursesw/tinfo 6 runtime libraries (Debian: `libncursesw6 libtinfo6`). Build from
source on older distributions or other platforms. The workflow uses the built-in
GitHub token; no release secret needs to be configured.

## Build and run

Requires a compiler implementing C23 `constexpr`, `nullptr`, and `#embed`,
make, POSIX threads, and ncursesw development files. Verified with GCC 16.

```sh
make MODE=release
./log-viewer app.log
./log-viewer app.log worker.log
./log-viewer /var/log/my-service/
my-command 2>&1 | ./log-viewer
./log-viewer --filter 'error AND timeout AND payment' app.log
```

Regular files are indexed using compact offsets and read on demand. Pipes and
other non-regular inputs are spooled to an unlinked temporary file. History is
not evicted at 100,000 lines; all indexed records remain accessible. File growth
and rename rotation are followed automatically. Pipes remain browsable after EOF.
Keyboard input comes from `/dev/tty`, independently of piped log input.

Pass any number of files and directories. Directories are traversed recursively,
including extensionless and hidden regular files. Symlinks and special files
inside directories are skipped; overlapping paths and hard links are deduplicated.
Regular-file history is read in argument order, with sorted depth-first traversal
within each directory. Streams interleave as input becomes available. Records from different files remain separate even without final
newlines. This is ingestion order, rather than timestamp merging.

In the TUI, all inputs share one searchable history. The selected record's source
path appears above the key hints. Each file follows appends, rename rotation, and
truncation independently. Directories are rescanned approximately every 250 ms to
discover new files and subdirectories. A rotated inode already read is not imported
a second time. Use directories containing uncompressed text logs; binary systemd
journal files and compressed archives are not decoded.

`--dump` reads the currently discovered inputs to EOF and exits; it does not watch
for later directory additions. `-` includes stdin alongside file inputs, and `--`
ends option parsing for names starting with a dash. Unreadable inputs are errors.
The process needs descriptors for all open files and retained rotations; the OS
file-descriptor limit still applies.

Unterminated final records are indexed at EOF. If more bytes arrive, the trailing
record is extended and its filter result reevaluated. Lines can exceed 1 MiB;
there is no silent line truncation in storage or search.

## Sources

The sidebar shows the loaded files grouped by directory, with **All sources** at
its root. Press **Tab** to focus the tree, move with **j/k** or arrows, and press
**Enter** or **Space** to read a file. Select **All sources** (or press **a** while
in the tree) to return to the combined history. An asterisk marks the active scope.
**Tab** returns to the log list; **s** shows or hides the sidebar. On narrow
terminals the focused tree fills the available width.

The current text/regex/JSON filter applies within the selected source. Switching
sources preserves the filter and resumes follow. All sources keep ingesting in
the background, so switching back retains their history. Rotations stay grouped
under the original input path, and newly discovered files appear automatically.
Directory rows group files; select a file or the All sources row to change scope.

## Keys

| Key | Action |
| --- | --- |
| Tab | Switch source tree / log focus |
| `s` | Show / hide source tree |
| `j` / `k`, arrows | Move selection and pause follow |
| `gg` / `G` | First record / last record and resume follow |
| `Ctrl-d` / `Ctrl-u`, PageDown / PageUp | Move half a page |
| `f` | Toggle follow |
| `h` / `l`, left / right | Pan horizontally |
| `/` | Edit a live filter, superseding the previous search |
| Enter | Accept filter / open selected record |
| Esc | Restore previous filter / close modal |
| Backspace / `Ctrl-u` in filter | Delete character / clear input |
| `c` | Clear filter |
| `?` | Help |
| `q` | Close modal or quit |

The detail modal pretty-prints JSON within the parser limits, otherwise displays
the original message. `j`/`k`, PageDown/PageUp, and `Ctrl-d`/`Ctrl-u` scroll;
`g` returns to the beginning; `h`/`l` pan. Search and ingestion continue while
it is open. Regular rows preview the first 64 KiB; Enter reads the full record.

## Filters

See the [query language guide](docs/queries.md) for syntax, operators, examples,
quoting rules, and current limitations.

Text searches are ASCII case-insensitive. An uppercase ` AND ` separator requires
all terms to occur in the same record, in any order. Without it, text is a single
substring (so `connection refused` remains a phrase). Up to 32 AND terms and
511 query bytes are supported. Prefix a POSIX extended regular expression with
`~`. Invalid expressions display an error rather than stale results.

```text
connection refused
error AND timeout AND payment
~error|warn
json | level == "error"
json | http.status >= 500
json | message ~ "timeout"
json | message ~ "error" AND message ~ "500"
```

JSON conditions can be chained with uppercase `AND` and `OR`; `AND` binds more
tightly, and parentheses override the order. Operators: `==`, `!=`, `>`, `>=`, `<`,
`<=`, `~`. Dot-separated paths select nested object fields. Strings are decoded
before comparison. Field comparisons are case-sensitive; ordering requires JSON
numbers. Missing fields make their condition false, and invalid JSON never
matches. Boolean and null values may be unquoted. Array indexing is not supported.
JSON `~` is a literal substring operator, replacing the previous `contains`
spelling. JSON parsing is skipped for text and regex queries.

Search scans the entire index in original order, publishing matching record IDs
in batches. A background worker handles indexing and searching; the TUI renders
only the viewport. Changed filters cancel/supersede earlier work.

For scripts:

```sh
./log-viewer --dump --filter 'json | level == "error"' app.log
printf 'info\nerror\n' | ./log-viewer --dump --filter error
```

`--dump` prints matches in order and exits after input EOF and search completion.

## Storage and memory

- Each line uses 16 bytes of metadata, plus up to 8 bytes per matching result on
  64-bit systems. An empty filter uses implicit IDs and no result array.
- A run table maps contiguous batches of record IDs to source generations,
  adding two machine words per run. Interleaved live writes can produce one run
  per line; normal initial indexing needs one run per nonempty file. All inputs
  share a single 1 MiB ingestion buffer.
- Raw regular-file text stays in its file. Buffered `pread` avoids mapping
  lifetime hazards when external processes truncate files.
- Rotated file descriptors are retained so old records remain readable. If a
  source is truncated, its old generation is marked unavailable, the TUI reports
  the number affected, and new records are indexed as a new generation. Bytes
  removed externally cannot be recovered; the app does not claim snapshot isolation.
- Stream history consumes temporary filesystem space, not a retained heap string
  per line. Set `TMPDIR` to choose the filesystem; `/tmp` may be tmpfs.
- Text/AND search uses reusable bounded buffers, including for giant records.
  Regex/JSON matching and full modal/dump reads may use memory proportional to
  one record. JSON limits remain 128 nesting levels and 16,384 structural items.
- IO/allocation failures stop the operation with cleanup and an error. Temp files
  are unlinked immediately; their storage is released when descriptors close.
- ANSI CSI and control bytes are stripped on access. UTF-8 display uses cell widths.

See [architecture](docs/architecture.md) for ownership/concurrency and
[measured results](docs/performance.md) for the 1M/10M benchmark and tradeoffs.
Sheredom’s bundled [JSON header](include/json.h) is under the Unlicense;
application source is licensed under [GNU GPLv3](LICENSE) (`GPL-3.0-only`).
Copyright (c) 2026 log-viewer contributors.

## Embedded text and code style

Help, CLI usage, and the key hint live in `resources/*.txt`. C23 `#embed` places
these bytes in the executable with a terminating NUL; no runtime resource files
or generated C files are needed. GCC records embedded inputs in the normal
header dependency files, so editing a text file rebuilds its consumer.

First-party C uses typed `constexpr` constants, `nullptr`, and semantic blank-line
grouping following check → gather → transform → aggregate → return where applicable.
The formatter handles layout; logical grouping is reviewed manually. The bundled
JSON header retains its upstream style.

## Development and verification

```sh
make test
make MODE=release test
make format
make format-check
make bear
make tidy
make sanitize
make threadcheck
make memcheck
make benchmark
make help
```

Tests require Python 3. Formatting and analysis require clang-format/clang-tidy;
Bear generation uses its `parse-sh` subcommand. The upstream JSON header is
excluded from formatting. Core tests link without ncurses.

If LeakSanitizer cannot run under a tracing/sandbox environment, use
`make sanitize ASAN_OPTIONS=detect_leaks=0`; `make memcheck` separately checks
application allocation balance and injected allocation failures. Set
`LOG_VIEWER_BIN` to reuse the integration suite with another binary.
