# Raven frontend

Build with `make gui`, then run `build/gui/log-viewer-gui [existing CLI input options]`.
Submit the search field with Enter. Activate a row with Enter or double-click to
load details. Follow and viewport pause are independent of ingestion. All core
input kinds, including Docker and Kubernetes, enter through the existing options
parser and search session. The normal executable remains the TUI.

The optional CMake build pins Raven and fetches Vulkan headers when needed. It
requires network access on the first build, CMake, a C23 compiler, and X11 headers
on Linux. Runtime rendering requires a working Vulkan driver. On macOS Raven
uses Cocoa and MoltenVK; application code remains C23. Raven's web feature,
designer, examples and tests are disabled for this build.

The reader thread owns scratch buffers and publishes copied previews through a
mutex-protected cache. Paint callbacks perform no filesystem access. The cache
holds at most 512 previews of 4096 bytes; details are limited to 1 MiB. Older
viewport requests are rejected by generation. Query and history changes detected
during a read discard its batch. Status and visible content refresh at 10 Hz.

This is an initial integration, not a completed production GUI. Remaining work:
- coherent native range snapshots and selected-record identity for details;
- source selection, level controls, configurable columns and preferences;
- precise large-range scrolling (Raven uses floating-point scroll coordinates);
- asynchronous session open/close and comprehensive GUI error reporting;
- UTF-8 normalization, large-record chunk navigation and richer level styling;
- automated GUI interactions, million-row benchmarks, and macOS bundle packaging.

Do not interpret a successful build or short launch smoke test as evidence of
million-row performance. Validate frame times and ingestion/query regressions
using the existing benchmark corpus before adding visual effects. Counts above
INT_MAX currently exceed Raven's table index range and must not be treated as a
supported navigation range.
