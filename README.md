# log-viewer

A C23 terminal log explorer with file-backed history, background search, syntax
coloring, and a scrollable JSON detail modal. Inspired by LazyTail.

## Build and run

Requires a C23 compiler, make, POSIX threads, and ncursesw development files.

```sh
make MODE=release
./log-viewer app.log
my-command 2>&1 | ./log-viewer
./log-viewer --filter 'error AND timeout AND payment' app.log
```

Regular files are indexed using compact offsets and read on demand. Pipes and
other non-regular inputs are spooled to an unlinked temporary file. History is
not evicted at 100,000 lines; all indexed records remain accessible. File growth
and rename rotation are followed automatically. Pipes remain browsable after EOF.
Keyboard input comes from `/dev/tty`, independently of piped log input.

Unterminated final records are indexed at EOF. If more bytes arrive, the trailing
record is extended and its filter result reevaluated. Lines can exceed 1 MiB;
there is no silent line truncation in storage or search.

## Keys

| Key | Action |
| --- | --- |
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
json | message contains "timeout"
```

JSON filters accept one field condition. Operators: `==`, `!=`, `>`, `>=`, `<`,
`<=`, `contains`. Dot-separated paths select nested object fields. Strings are
decoded before comparison. Field comparisons are case-sensitive; ordering
requires JSON numbers. Missing fields and invalid JSON do not match. Boolean
and null values may be unquoted. Compound JSON conditions and array indexing
are not supported. JSON parsing is skipped for text and regex queries.

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
application source is MIT licensed.

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
