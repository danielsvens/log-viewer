# Architecture and ownership

The entry point and ncurses components are retained. Phase 1 replaces string
retention with file-backed line indexing and moves ingestion/search to one worker.

| Module | Responsibility |
| --- | --- |
| `main.c`, `options.c` | Locale, arguments, usage, exit status |
| `app.c` | Start the engine, run the TUI or dump consumer, clean up |
| `source_set.c` | Expand multiple paths, recursively discover files, deduplicate identities, schedule inputs |
| `source.c` | Read bounded batches, index line boundaries, spool streams, follow files |
| `log_store.c` | Own descriptors and paged 16-byte line metadata; buffered random access |
| `source_tree.c` | Build a sorted source tree with borrowed names and stable file IDs |
| `search.c` | Worker, query cancellation, ordered result IDs, snapshots, navigation |
| `query.c` | Prepared AND/text, regex, numeric/JSON comparisons, reusable query scratch |
| `json_support.c` | Parser limits, reusable parser workspace, nested field lookup, pretty printing |
| `text.c` | Reusable normalization and ASCII comparison |
| `log_level.c` | Structured metadata, numeric level mappings, plain prefixes, and level masks |
| `tui.c`, `tui_input.c`, `tui_render.c` | Terminal lifetime, keys, visible rows, modal |

Human-readable help, usage, and key hints live in `resources/*.txt` and are
embedded as NUL-terminated byte arrays with C23 `#embed`. They have no runtime
filesystem dependency. Numeric compile-time limits use typed `constexpr` values;
header guards and platform feature flags remain preprocessor macros.

Interfaces live under `include/log_viewer/`. Only `json_support.c` includes the
third-party parser implementation. Core modules have no ncurses dependency.

## Storage

`LineIndex` is statically asserted to be 16 bytes: two 64-bit values for raw file
offset and length (excluding the newline). Fixed-size metadata pages avoid a
large moving allocation during indexing. There is no per-line string allocation,
fixed record cap, or history eviction. Empty records and a final unterminated
record are indexed. A provisional final record is updated at its stable ID when its source
grows, even when other sources have appended later records; results from that record onward are reevaluated.

A source-generation list owns descriptors and counts its records. A separate
run table maps monotonically appended record ranges to generation IDs, allowing
interleaved sources while retaining 16-byte line entries. Runs use two `size_t`
values each and coalesce consecutive appends from the same generation. Regular files are
read directly; rotation retains the old descriptor and creates a new segment.
Non-regular input is appended to an unlinked `mkstemp` file under `TMPDIR` (or
`/tmp`), using the same line index. Disk/full filesystem and IO failures are
reported rather than discarding history.

`mmap` was considered. A live external truncate can invalidate mapped pages
between a size check and access; unmapping on the next poll does not prevent
`SIGBUS`. Phase 1 therefore uses buffered `pread`, which also gives safe random
access and avoids copying whole files into application-owned memory. There are
no mapping lifetimes to synchronize. The measured throughput is sufficient for
the milestone without introducing snapshots, a cache manager, or a signal trap.

A detected truncate invalidates that generation, retains its record IDs as
unavailable, and starts a new segment. Unfiltered browsing shows placeholders
and an affected-record count; filtered searches skip unavailable generations.
Externally removed bytes cannot be reconstructed. Append/rename are the expected
writer patterns; in-place rewrites, or a truncate/regrow entirely between polls,
are not transactional snapshots. `pread` remains memory-safe even in those races.

## Search and synchronization

`Search` owns a worker thread, the source set, metadata, and result pages.
`SourceSet` owns copied paths and independent `LogSource` states. A shared read
buffer is lent to one source for a poll and then detached. Initial regular-file
history is drained in argument/sorted directory order; subsequent polling rotates
across inputs. Recursive rescans discover additions while a device/inode history
prevents repeated import of rotated files and hard links. Empty directories are
valid inputs. Directory symlinks and nonregular children are skipped. Dump mode
expands directories once. Discovery and opens occur under the index mutex; very
large trees or slow filesystems may delay the UI during a rescan. The worker
alternates 1 MiB ingestion batches with scans of up to 8,192 records. Queries
are prepared once per revision. Text uses bounded case-folded substring matching
with precomputed skip tables; AND conditions short-circuit. Large text records
are processed in chunks with normalized carry bytes so terms and ANSI sequences
can span boundaries. JSON/regex work is not done for text-only queries.

JSON uses the bundled parser's allocation callback to reuse one workspace.
Scalar field values are borrowed from that workspace, then compared before the
next parse. JSON expressions compile to a bounded tree of conditions with AND/OR nodes,
AND precedence, and explicit parentheses. Each record is parsed once into the
reusable workspace; field lookups share that document and boolean evaluation
short-circuits. Numeric query operands are converted once. Object/array field
serialization can still allocate, as can a workspace growth operation. Invalid
JSON and allocation failures are distinguished in the scan path.

A single mutex protects source/index mutations, result publication, request text,
and snapshot/location lookup. Expensive filter evaluation happens outside that
mutex. Only the worker mutates metadata and result pages. The TUI copies record
locations while locked, then releases the lock before its own IO/formatting.
Descriptors live until shutdown, so copied locations cannot refer to a closed
file. Separate readers and scratch buffers prevent shared mutable text buffers.
Ingestion holds the lock for a bounded batch, including its read; slow storage
can increase that lock latency. There is no hard real-time latency guarantee.

Atomic query revisions and a stop flag are checked between records and between
chunks of a giant text record. New requests hide old results immediately. Only a
batch from the current revision may be published. Results are ascending compact
record IDs; publication reuses pages instead of holding duplicate full result
sets. The empty filter uses implicit IDs. Completed-query result capacity is
retained for subsequent searches and freed on shutdown.

`LogView` belongs to the UI thread and contains counts, selection/follow state,
and a stable record-ID anchor. Selection uses direct result lookup and binary
search, including across widely separated matches. Regex/JSON library calls
within one record are not forcibly interrupted, but do not hold the UI mutex.

## Resource lifetimes

- `app_run()` owns the UI query, `Search`, and `Tui`; it tears down the terminal
  before printing runtime errors.
- `Search` stops/signals and joins its worker before freeing pages, buffers,
  queries, workspaces, or descriptors. Main code must stop reading before close.
- `SourceSet` owns input paths, directory roots, identity history, and the shared
  ingestion buffer. Record source names borrow its paths until search shutdown.
- `LogSource` owns its input descriptor when opened from a path, borrows stdin,
  and restores stdin flags. It owns a stream spool descriptor and, when used on its own, a reusable read
  buffer. Source-set polls borrow the set’s shared buffer instead. `LogStore` owns separate descriptor references for history.
- `RecordReader` owns its reusable byte buffer. Returned spans are borrowed
  until its next refill/destruction. Location lookup never returns index pointers.
- `QueryScratch` owns normalization and parser buffers; it is local to one
  consumer. Query compilation owns any compiled regex and replaces it safely.
- `Tui` owns its reader, normalized row buffer, terminal, and formatted modal
  string. Rows preview at most 64 KiB; a modal reads only the selected full record.
- Error paths clean up partially initialized state. Application-owned allocation
  balance and deterministic failure paths are verified by test-only linker wrappers.

No runtime arena, allocator, database, inverted index, or custom text cache was
introduced. The mutable process-global state is only the signal interruption flag.

## Function guideline

Continue **check → gather → transform → aggregate → return**, using only applicable
stages. Check failures early; gather metadata/inputs; transform with small helpers;
commit state/results together; release temporary ownership and return a status.
Use one blank line between changes of purpose, including within branches and
loops. Separate allocation/lookup, its guard, successful work, and the final
return. Keep related state updates together and declarations close to their use.
Do not add empty stages or repetitive five-label comments to small functions.
Resource acquisition checks each operation before using its result.

For example, `search_read_result()` checks a result index, gathers a location under
the mutex, reads and normalizes outside the lock, then returns a borrowed display
record. `query_match_span()` validates query state, normalizes only when required,
evaluates prepared conditions, and returns match/no-match/error explicitly.

## Verification

`make test` exercises >100k file and pipe history, AND/JSON/regex semantics,
supersession, shutdown during work, stable selection, partial-line extension,
truncation, >4 GiB offsets, and existing terminal workflows. `make sanitize`,
`make threadcheck`, and `make memcheck` cover memory safety, concurrency, and
allocation balance/failure cleanup. See [performance](performance.md) for the
separate reproducible 1M/10M measurements and acceptance evidence.

## Source selection

Each input has a stable source ID, copied into all its file generations. The
search request contains both the expression and a source ID (or all sources).
Changing either increments the same cancellation revision. Scoped searches skip
other sources using metadata before reading text; an empty expression also avoids
raw reads. The all-sources empty-filter fast path retains implicit result IDs.
A scoped result set uses the existing paged IDs, including unavailable records
when no expression is applied. Ingestion continues for all sources independently.

The UI owns a source tree whose rows borrow immutable source paths. It rebuilds
when discovery increases the source count, preserving the cursor by source ID.
Common directory prefixes are hidden and remaining directories are grouped with
indented children. Tab changes pane focus; Enter selects scope. Narrow terminals
show the focused source tree in place of the log pane. The tree is destroyed
before Search releases its paths.

## Log levels

Level masks participate in the same cancellation revision as source and query
selection. The all-levels empty-query path uses implicit IDs without reading or
parsing records. Restricted masks classify candidate records in the worker;
JSON level detection and JSON query conditions share one parsed document and
reusable workspace. Unknown is a separate group, visible initially. Hiding all
levels still indexes incoming records without evaluating them.

The UI classifies visible row previews with its own JSON workspace for coloring,
independent of the worker. Truncated previews that are not complete JSON cannot
supply structured colors; filtering still examines the complete record. No level
strings or parsed documents are retained per indexed line. Level filtering of a
giant record can grow reader and scratch buffers to that record's size.
