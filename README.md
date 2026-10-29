# log-viewer

A C23 terminal log viewer with live ingestion, syntax coloring, filters, and a
scrollable JSON detail modal. Inspired by LazyTail.

## Build and run

Requires a C23 compiler, make, and ncursesw development headers/library.

```sh
make
./log-viewer /var/log/my-app.log
my-command 2>&1 | ./log-viewer
./log-viewer --filter 'json | http.status >= 500' app.log
```

Files are followed automatically, including rename rotation and truncation.
Pipes remain browsable after the producer exits. Keyboard input comes from
`/dev/tty`, so it works independently of piped log input. Newline-delimited
records are expected; a final unterminated pipe record is shown at EOF.
File records become visible once their terminating newline arrives.

## Keys

| Key | Action |
| --- | --- |
| `j` / `k`, arrows | Move selection; pause automatic scrolling |
| `gg` / `G` | First record / last record and resume follow |
| `Ctrl-d` / `Ctrl-u`, PageDown / PageUp | Move half a page |
| `f` | Toggle follow |
| `h` / `l`, left / right | Pan horizontally |
| `/` | Edit filter, applied while typing |
| Enter | Accept filter / open selected record |
| Esc | Cancel filter edit and restore previous filter / close modal |
| Backspace / `Ctrl-u` in filter | Delete character / clear input |
| `c` | Clear filter |
| `?` | Help |
| `q` | Close modal or quit |

The detail modal pretty-prints valid JSON, otherwise shows the original message.
Use `j` / `k`, PageDown / PageUp, or `Ctrl-d` / `Ctrl-u` to scroll; `g` returns
to its beginning. Use `h` / `l` to pan long fields horizontally. Ingestion continues while the modal is open.

## Filters

Plain text performs a case-insensitive substring search. Prefix an extended
POSIX regular expression with `~`. Invalid expressions show an error in the TUI.

```text
connection refused
~error|warn
json | level == "error"
json | http.status >= 500
json | message contains "timeout"
```

Structured filters start with `json |` and accept one field condition. Supported
operators: `==`, `!=`, `>`, `>=`, `<`, `<=`, `contains`. Dot-separated paths select
nested object fields. Comparisons and `contains` are case-sensitive; ordering
operators require JSON numbers. Missing fields and invalid JSON never match.
JSON strings and query string literals are decoded before comparison.
Boolean and null values can be compared without quotes. Arrays are displayed
and formatted, but array indexing and compound query expressions are not supported.

For scripts or environments without a terminal:

```sh
./log-viewer --dump --filter 'json | level == "error"' app.log
printf 'info\nerror\n' | ./log-viewer --dump --filter error
```

`--dump` prints matches and exits at EOF instead of following the source.

## Limits and implementation

- Keeps the latest 100,000 lines, evicting 10,000 at a time when full.
- Each line is capped at 1 MiB; excess bytes are discarded until its newline.
- JSON parsing limits structural items to 16,384 and nesting to 128 levels per record.
- ANSI control sequences are stripped; colors are derived from severity and JSON
  syntax. UTF-8 text uses terminal cell widths.
- The bundled parser is Sheredom’s [include/json.h](include/json.h), provided
  under the Unlicense; no download is needed. Application source is MIT licensed.
- Filters rescan retained records synchronously; very large records or expensive
  regular expressions can affect responsiveness.

## Code organization

The entry point delegates to modules for application lifecycle, input, storage,
queries, JSON, text, and the TUI. See [architecture and function guidelines](docs/architecture.md)
for interfaces, ownership, and the check → gather → transform → aggregate → return flow.

## Development tools

```sh
make MODE=release       # optimized build; default is debug
make run ARGS=app.log
make bear               # regenerate compile_commands.json
make format             # format project sources
make format-check       # verify formatting
make tidy               # regenerate database and run clang-tidy
make help
```

Tool targets require clang-format, clang-tidy, and Bear with `parse-sh` support.
The bundled third-party JSON header is excluded from formatting. Build objects
and header dependencies live under `build/debug` or `build/release`.

## Verification

```sh
make test
```

Runs core module tests and requires Python 3 for integration tests. Tests exercise
record ownership, selection across eviction, text/regex/JSON filters, invalid JSON, terminal
navigation, JSON details, live file append, rotation, filter cancellation, and
separate keyboard input for pipelines.
