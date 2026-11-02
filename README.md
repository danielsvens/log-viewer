# log-viewer

A C23 terminal log explorer for browsing and following text logs. Open multiple
files or a directory, search their history, and inspect JSON records in a
scrollable detail view.

- Follow file growth and rotation, or read logs piped from another command.
- Discover files recursively, including new files added while browsing.
- Switch between one file and all sources using the file tree.
- Filter with plain text, regular expressions, or chained JSON conditions.
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
| `h` / `l`, left / right | Pan horizontally |
| `/` | Edit a live filter |
| Enter | Select a source / inspect a record / accept a filter |
| Esc | Cancel filter editing / close detail |
| `c` | Clear filter |
| `?` | Help |
| `q` | Close detail or quit |

Enter opens the selected record in a scrollable view and pretty-prints valid
JSON. Use `j/k` to scroll, `h/l` to pan, and `g` to return to the top.

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
