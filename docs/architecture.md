# Code structure and function flow

`main.c` handles locale setup, command-line results, and the application exit
code. It delegates resource lifetime and the event loop to `app.c`.

| Module | Responsibility |
| --- | --- |
| `options.c` | Parse arguments and provide usage text |
| `app.c` | Initialize modules, connect incoming records to their consumer, run the loop, clean up |
| `source.c` | Read bounded batches, assemble records, follow file rotation/truncation, restore stdin flags |
| `log_store.c` | Own retained records, build visible indices, preserve selection, evict old records |
| `query.c` | Compile text/regex/JSON filters and evaluate record matches |
| `json_support.c` | Bound parsing, decode strings, extract nested fields, pretty-print messages |
| `text.c` | Copy/trim/sanitize text and apply the existing severity heuristic |
| `tui.c` | Initialize and close the terminal |
| `tui_input.c` | Dispatch navigation, filter editing, and modal keys |
| `tui_render.c` | Draw the viewport, highlighted text, status, and modal |

Public interfaces live under `include/log_viewer/`. The upstream `include/json.h`
implementation is included only by `json_support.c`; other modules use the
small application interface. The core modules do not depend on ncurses.

## Function guideline

Use the flow **check → gather → transform → aggregate → return** where those
stages apply:

1. **Check:** Reject invalid input, failed operations, or unmet preconditions
   early. Internal initialized-state contracts use assertions.
2. **Gather:** Read the inputs needed for the operation into local variables.
3. **Transform:** Parse, format, compare, or compute using focused helpers.
4. **Aggregate:** Commit the result to the owning state or collect results.
5. **Return:** Release temporary resources and return the result or status.

Do not add empty stages or five repetitive comments to small functions. Split
unrelated work into named helpers, and keep dispatch functions concerned with
routing actions. Resource acquisition necessarily checks each operation before
using its result.

For example, `query_matches()` checks query validity, gathers a JSON field,
compares it, releases the temporary field, and returns the match result.
`log_store_append()` checks its initialized-state contract, gathers the next
record and its match result, then commits record storage, visible indices,
and follow selection together.

## State and ownership

- `Application` owns the query, source, store, and TUI. Cleanup handles partially
  initialized state. The only mutable module-global state is the
  `sig_atomic_t` interruption flag used by signal handlers.
- `LogSource` owns its partial-line buffer and file descriptor, borrowing the
  command-line path. It borrows stdin and restores its flags rather than closing
  it. Call `source_open()` before `source_close()`; close is safe after a failed
  open and can be repeated.
- A `RecordSink` takes ownership of every emitted string, even on failure.
  The app transfers it to the store or frees it after dump output.
- `LogStore` owns retained strings and the record/index allocations. Selected
  record pointers are borrowed and expire on eviction or destruction.
- `Query` is initially zeroed and owns a compiled regex when present. Compilation
  replaces its previous expression, including when the replacement is invalid.
- JSON helpers return owned strings, while the third-party parse tree stays
  private to the JSON module and is released before each helper returns.
- `Tui` owns the terminal and modal text. Navigation, editing, and rendering all
  receive explicit state rather than accessing hidden application globals.

## Verification

`make test` runs a core test binary without ncurses, followed by pseudo-terminal
integration tests. Core tests exercise independent query state, decoded JSON,
record ownership, selection through filtering/eviction, and partial-line
flushing. Integration tests cover live file append, rotation, truncation, modal
rendering, filter cancellation, and pipeline keyboard routing.

To reuse the integration suite with another build, set `LOG_VIEWER_BIN` to its
path. `make format-check`, `make tidy`, and `make MODE=release test` provide the
formatting, static-analysis, and release checks.
