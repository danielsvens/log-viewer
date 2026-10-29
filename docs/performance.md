# Phase 1 measurements and acceptance evidence

Measured on AMD Ryzen 7 7800X3D, Linux x86-64, GCC 16.2.1 with `-O2`.
Inputs were generated on `/tmp` (tmpfs); these are warm/OS-managed-cache CPU and
memory-path measurements, **not cold disk throughput claims**. We did not evict
host caches or change machine settings. On slower storage, indexing/search also
pay filesystem IO costs. Times are individual reproducible samples, not percentiles.

## Workload and method

The deterministic generator emits 1,000,000 and 10,000,000 variable-length records,
50% JSON and 50% plain text. Sizes are 89,344,410 and 898,444,422 bytes. The rare
marker and three AND terms occur at the beginning, middle, and end. `payment`
is in every record; the numeric JSON condition matches only the final JSON record.

Each benchmark process indexes with the empty filter, waits for completion, then
requests one query over the complete retained index. It measures full completion,
first published result, `getrusage` peak RSS, and 1,000 direct result jumps/readouts.
Results are validated against expected counts and sample IDs. Startup/inherited
RSS contributes a floor to small-dataset peak readings. `first_result_seconds`
is publication observed by a polling consumer, not a promise about terminal frame
latency. Index times include starting the background worker and polling completion.

## Results

| Lines | Query | Index (s) | Lines/s | MB/s | Full scan (s) | First result (ms) | Matches | Peak RSS (MiB) | Jump/read (µs) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000,000 | `RARE_MARKER` | 0.020 | 50,173,913 | 4,483 | 0.086 | 1.04 | 3 | 20.1 | 28.81 |
| 1,000,000 | `payment` | 0.018 | 55,856,042 | 4,990 | 0.101 | 1.21 | 1,000,000 | 27.8 | 35.18 |
| 1,000,000 | `error AND timeout AND payment` | 0.018 | 56,968,219 | 5,090 | 0.115 | 1.39 | 3 | 20.1 | 30.58 |
| 1,000,000 | `json | http.status >= 500` | 0.018 | 55,901,665 | 4,994 | 0.288 | 288.32 | 1 | 19.1 | 0.10 |
| 10,000,000 | `RARE_MARKER` | 0.159 | 63,070,599 | 5,666 | 0.804 | 1.08 | 3 | 157.9 | 31.60 |
| 10,000,000 | `payment` | 0.168 | 59,615,223 | 5,356 | 0.986 | 1.38 | 10,000,000 | 234.0 | 28.98 |
| 10,000,000 | `error AND timeout AND payment` | 0.166 | 60,325,970 | 5,420 | 1.133 | 1.39 | 3 | 157.3 | 31.25 |
| 10,000,000 | `json | http.status >= 500` | 0.159 | 62,826,589 | 5,645 | 2.961 | 2960.62 | 1 | 157.0 | 0.10 |

The 10M rare/AND results are IDs **1, 5,000,001, 10,000,000**, proving that the
whole dataset remains searchable. JSON's first match is near EOF, so first-result
time correctly equals almost the entire scan, rather than a first-match shortcut.

For 10M lines, the logical index is exactly 160,000,000 bytes (152.6 MiB), plus
page rounding, pointers, descriptors, buffers, and runtime overhead. The unfiltered
view uses implicit IDs. A common nonempty query adds about 80 MB of result IDs;
there is no second copy of matching text. Result page capacity is reused across
queries and retained until close. Plain giant records use chunked search; JSON,
regex, modal formatting, and full dump output can need memory proportional to
one record. The original raw file is never loaded wholesale into a heap buffer.

## Original implementation baseline

`benchmarks/baseline.py` compiles the original retained-history path from Git
revision `b53937f`, with the same release optimization and dataset. It bypasses
TUI frame waits and uses the original source/store/query functions directly:

- Received 10,000,000 records in **1.391 s**, retaining only **100,000**.
- Its rare-marker search returned **1** match (the last one), rather than 3.
- Its AND expression returned **0** matches; AND was not implemented.
- Peak RSS was **18.4 MiB**, reflecting its deliberately smaller retained window.
- The 0.0043 s rare-query time scans only that 100k window, so it is not comparable
  to a complete 10M query. New indexing is about 8× faster while retaining all IDs.

We also measured the unmodified original `--dump` binary: 1M rare scans took 7.02 s
and 10M took 70.69 s. These include its fixed polling sleeps and do not retain
history; they should not be read as pure matching-engine speedups. Raw measurements
are included separately in the JSON results. Baseline export/build does not alter
the working tree or switch branches.

## Terminal responsiveness

The real pseudo-terminal harness starts a JSON search/indexing session over 10M
lines, verifies a search is active, and opens/closes help five times. The measured
key-to-help responses are in `benchmarks/tui-results.json`; the maximum was
**0.28 ms** in this run. Superseding the search, clearing the filter, and jumping
to the first record took **164.3 ms**. A 250 ms response ceiling is asserted for
help. These are local observations, not a hard real-time guarantee, and the probe
covers active initial indexing/search together. Engine jump/read latency for
sparse matches is measured separately in the table. A separate PTY launch searching
`RARE_MARKER` measured the first actual rendered record at **52.9 ms**, including
process/terminal startup and the 50 ms frame/input wait. It waits for the full
message text, not the query text in the footer.

Only the viewport is normalized/rendered, with a 64 KiB row preview and a reusable
read-ahead buffer. A full selected message is loaded/formatted only on Enter.
Scans do not hold the UI mutex while evaluating queries. Bounded ingestion does
hold it during its read/index batch; cold or slow storage can increase that latency.

## Safety and behavioral verification

- Debug and release core tests, plus pseudo-terminal integration tests.
- File and disk-spooled pipe history beyond 100k; first/middle/last matches.
- Text/regex/JSON semantics, Unicode escapes, AND conditions, missing fields,
  malformed JSON, and empty input.
- Empty lines, EOF partial records and subsequent extension, multi-megabyte lines,
  search terms and ANSI sequences across read boundaries, and offsets beyond 4 GiB.
- Live append, rename rotation, truncate invalidation, input flag restoration,
  redirected stdin position, JSON modal, and filtering while input arrives.
- Query supersession, stable sparse selection, follow pause, and shutdown while
  scanning, using separate readers/workspaces and revision-checked publication.
- ASan/UBSan and ThreadSanitizer runs pass the tested paths. LeakSanitizer cannot
  run in this tracing environment, so its detection was disabled explicitly.
  The separate linker-wrapper check reports balanced application-owned allocations
  on normal/core/TUI paths and on **12 injected allocation failures**. Shared-library
  internal allocations are outside that allocation-balance check; their normal
  destructor APIs are called. No known leak or invalid access remains in these paths.

Truncation is not historical snapshotting: if a writer removes bytes, the affected
file generation is marked unavailable, visibly reported, and skipped by filters.
Rename rotation retains the old descriptor and readable history. `pread` avoids
invalid mapping access in both cases. In-place rewrites and truncate/regrow races
between polls are not a transactional snapshot. Stream history uses temporary
filesystem space, including tmpfs RAM when that is the selected filesystem; use
`TMPDIR` for an actual disk location when desired.

## Reproduce

```sh
make MODE=release test
make benchmark
python3 benchmarks/tui.py /tmp/log-viewer-benchmark/10000000.log
python3 benchmarks/baseline.py /tmp/log-viewer-benchmark/10000000.log --revision b53937f
make sanitize                          # includes LeakSanitizer where supported
make sanitize ASAN_OPTIONS=detect_leaks=0  # tracing environments
make threadcheck
make memcheck
```

Use `BENCH_ARGS='--lines 1000000 --directory /path/to/datasets --output /tmp/results.json'`
to change the dataset/cache location or size. The generator may also be invoked
directly. `benchmarks/run.py --baseline-bin /path/to/original/log-viewer` adds the
old CLI comparison. Do not use concurrent writers to the same results file.
Generated datasets are outside the repository and are not removed automatically.

## Acceptance audit

| Requirement | Evidence |
| --- | --- |
| 10M lines without per-line ingestion strings | 16-byte `LineIndex` static assertion; paged append source; benchmark count 10M |
| No 100k eviction; all original records searchable | Rare/AND IDs 1, 5,000,001, 10,000,000; common query 10M results |
| AND correctness, original order, compact results | Prepared terms; ascending record-ID pages; count and sample assertions |
| Responsive TUI during search | Active-search PTY probe; cancellation/core tests; navigation measurements |
| Sparse scrolling/jumps | 1,000 randomized result jumps; direct index lookup, no scanning between matches |
| JSON, modal, file/stdin/stream/dump modes | Existing and expanded core/PTY suites |
| Measured memory independent of raw text retention | 16 bytes/line plus result IDs; RSS measurements; raw bytes file-backed |
| Resource cleanup and tested memory/concurrency safety | ASan/UBSan, TSan, allocation balance and failure injection |
| Reproducibility | Generator, engine/PTY/baseline harnesses, raw JSON measurements |

No SQLite, inverted/trigram index, search database, arena, or custom runtime
allocator was introduced. Future indexes can build on stable record IDs and
length-bounded query evaluation without changing the TUI's result lookup.
