# log-viewer

A C23 terminal log explorer for browsing and following text logs. Open multiple
files or a directory, search their history, and inspect JSON records in a
scrollable detail view.

- Follow file growth and rotation, or read logs piped from another command.
- Discover files recursively, including new files added while browsing.
- Switch between one file and all sources using the file tree.
- Filter by log level, plain text, regular expressions, or chained JSON conditions.
- Browse the complete log history without a fixed record limit.

## Build manually

Requires:

- A C23 compiler; verified with GCC 16.
- GNU Make.
- ncursesw development headers and libraries (`libncurses-dev` on Debian/Ubuntu).
- POSIX threads and a terminal supporting ncurses.

From the repository root:

```sh
make MODE=release
```

This creates `./log-viewer`. To choose a compiler explicitly:

```sh
make CC=gcc-16 MODE=release
```

For a debug build with symbols, run `make` or `make MODE=debug`. Use `make clean`
to remove build output.

On macOS, install the build dependencies with Homebrew:

```sh
brew install gcc@15 ncurses make
gmake CC=gcc-15 MODE=release
```

The macOS release binaries support macOS 15 or newer, with separate downloads for
Apple Silicon (`macos-arm64`) and Intel (`macos-x86_64`). Extract the `.tar.gz`
archive and run `./log-viewer app.log` from its folder. Homebrew is not required
to run these binaries.

## Run

```sh
./log-viewer app.log
./log-viewer app.log worker.log
./log-viewer ./logs/
my-command 2>&1 | ./log-viewer
./log-viewer --filter 'error AND timeout' app.log
```

**When piping a command, put `2>&1` before `|` to capture both stdout and stderr.**
Without it, warnings and errors can print directly over the interface and disrupt
the display. For example:

```sh
./gradlew bootRun --args='--spring.profiles.active=local' 2>&1 | ./log-viewer
```

Directories include hidden and extensionless regular files. Directory symlinks
and special files are skipped, and overlapping inputs are deduplicated. Use
uncompressed text logs; binary systemd journals and compressed archives are not
decoded. Records appear in ingestion order rather than being merged by timestamp.

The source tree shows loaded files grouped by directory. Press **Tab** to focus
it, move with **j/k** or arrows, and press **Enter** or **Space** to select a file.
Choose **All sources**, or press **a** in the tree, to browse everything. The
current filter stays active when switching sources, and all files keep following
in the background. Press **s** to show or hide the tree.

The status bar shows the total indexed lines across all sources. When filtering
or viewing one source, it also shows the number of matching lines.

To print matches without opening the terminal interface:

```sh
./log-viewer --dump --filter 'json | level == "error"' app.log
printf 'info\nerror\n' | ./log-viewer --dump --filter error
```

`--dump` exits after the current inputs reach EOF. Use `-` to include stdin
alongside file inputs, and `--` before filenames starting with a dash.

## Keys

| Key | Action |
| --- | --- |
| Tab | Switch focus between sources and logs |
| `s` | Show / hide source tree |
| `j` / `k`, arrows | Move selection |
| `gg` / `G` | First / last log; `G` resumes follow |
| `Ctrl-d` / `Ctrl-u`, PageDown / PageUp | Scroll down / up |
| `f` | Toggle live follow |
| `1` / `2` / `3` / `4` | Toggle error / warn / info / debug |
| `5` / `0` | Toggle unclassified / show all levels |
| `h` / `l`, left / right | Pan horizontally |
| `/` | Edit a filter; Enter applies it |
| `t` | Open a new filter tab |
| Shift+Left / Shift+Right | Previous / next filter tab |
| `x` | Close the current tab (keeps at least one) |
| Enter | Select a source / inspect a record / accept a filter |
| Esc | Cancel filter editing / close detail |
| `c` | Clear query; keep level selection |
| `?` | Help |
| `q` | Close detail or ask to quit |

Filter tabs let you keep several queries open over the same inputs. Press `t` to
open a new tab and type its query, then Enter to run it. New tabs inherit the
current source and level selection. Each tab remembers its query, source, levels,
scroll position, and follow setting. Switch with Shift+Left / Shift+Right; the active tab appears
in the tab bar above the filter editor. Tabs last for the current session.

All tabs share the indexed logs. Only the active tab searches, so opening tabs
does not duplicate log data or add background searches. Switching reruns that
tab’s query and may briefly show “Searching…” on large inputs.

Press `y` in the quit dialog to exit; `n`, Esc, or Enter returns to the viewer.

While editing a filter, left/right moves the caret, Home/End jumps to either end,
and Backspace/Delete removes text. Results stay visible until Enter applies the
query; Esc discards your edits.

Enter opens the selected record in a scrollable view and pretty-prints valid
JSON. Use `j/k` to scroll, `h/l` to pan, and `g` to return to the top.
Inside the inspector, `/` searches for literal text (case-insensitive); Enter jumps
to the first match and `n` / `N` cycles forward / backward, wrapping at either end.
Esc cancels search editing without closing the inspector.

Press `y` in the inspector to copy the entire displayed record, including all JSON
fields, even when they are off screen. On macOS this uses `pbcopy`; on Linux it
tries `wl-copy`, `xclip`, then `xsel`. Without those tools it sends an OSC 52
clipboard request to your terminal. Terminal support and clipboard permissions
may limit that fallback; the inspector reports which method was used.

## Log levels

Press **1**, **2**, **3**, or **4** to toggle error, warn, info, or debug lines.
**5** toggles unclassified lines, and **0** restores all levels. All groups are
visible initially. The status bar shows enabled (`+`) and hidden (`-`) groups.
Level selection combines with the current query and selected source; hiding a
level keeps its history available when you enable it again.

Levels are recognized from JSON fields such as `level`, `severity`, `log.level`,
and `severityText`, as well as common plain-text prefixes and `level=info` logs.
Fatal/critical messages belong to error, trace/verbose to debug, and notice to
info. A message mentioning "error" does not override its explicit INFO level.
Lines without a recognized level remain in the unclassified group.

To select levels when starting the viewer or printing matches:

```sh
./log-viewer --levels error,warn app.log
my-command 2>&1 | ./log-viewer --levels info,debug
./log-viewer --dump --levels error --filter 'timeout' ./logs/
```

Use `--levels all` to include everything or `--levels none` to start with every
group hidden. See the [query guide](docs/queries.md#log-level-selection) for
recognized fields and numeric level formats.

## Queries

Press `/` and enter a query, or pass it with `--filter`:

```text
connection refused
error AND timeout
~error|warn
json | http.status >= 500
json | message ~ "error" AND message ~ "500"
json | (level == "error" OR level == "warn") AND http.status >= 500
```

Plain text and regex searches are case-insensitive. JSON comparisons are
case-sensitive; JSON `~` means literal substring matching. Queries apply to the
selected source or **All sources**.

See the [query language guide](docs/queries.md) for operators, precedence,
quoting, and limits.

## License

Application source is licensed under [GNU GPLv3](LICENSE) (`GPL-3.0-only`).
Sheredom’s bundled [JSON header](include/json.h) is under the Unlicense.
Copyright (c) 2026 log-viewer contributors.
