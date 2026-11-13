#include "log_viewer/tui/tui.h"
#include "tui_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

typedef struct {
  short id, foreground, background;
} ColorPair;

static void initialize_colors(void) {
  if (!has_colors()) return;

  start_color();
  use_default_colors();

  static constexpr ColorPair TUI_COLOR_PAIRS[] = {
    { TUI_HEADER, COLOR_BLACK, COLOR_CYAN }, { TUI_ERROR, COLOR_RED, -1 },     { TUI_WARNING, COLOR_YELLOW, -1 },  { TUI_NORMAL, COLOR_GREEN, -1 },
    { TUI_MUTED, COLOR_CYAN, -1 },           { TUI_STRING, COLOR_YELLOW, -1 }, { TUI_LITERAL, COLOR_MAGENTA, -1 },
  };

  for (size_t i = 0; i < sizeof(TUI_COLOR_PAIRS) / sizeof(*TUI_COLOR_PAIRS); i++) {
    init_pair(TUI_COLOR_PAIRS[i].id, TUI_COLOR_PAIRS[i].foreground, TUI_COLOR_PAIRS[i].background);
  }
}

bool tui_open(Tui *tui) {
  *tui = (Tui){ 0 };

  tui->tty = fopen("/dev/tty", "r+");

  if (!tui->tty) return false;
  if (fcntl(fileno(tui->tty), F_SETFD, FD_CLOEXEC) < 0) {
    int error = errno;
    fclose(tui->tty);
    tui->tty = nullptr;
    errno = error;
    return false;
  }

  tui->screen = newterm(nullptr, tui->tty, tui->tty);

  if (!tui->screen) {
    fclose(tui->tty);
    tui->tty = nullptr;
    errno = ENOTSUP;
    return false;
  }

  set_term(tui->screen);
  cbreak();
  noecho();

  keypad(stdscr, TRUE);
  define_key("\033[1;2D", KEY_SLEFT);
  define_key("\033[1;2C", KEY_SRIGHT);
  timeout(50);
  set_escdelay(25);

  initialize_colors();

  tui->running = true;
  tui->tree_visible = true;

  return true;
}

void tui_close(Tui *tui) {
  if (tui->screen) {
    endwin();
    delscreen(tui->screen);
  }

  if (tui->tty) fclose(tui->tty);

  free(tui->detail);
  free(tui->tabs);
  source_tree_destroy(&tui->tree);
  record_reader_destroy(&tui->reader);
  text_buffer_destroy(&tui->line);
  json_workspace_destroy(&tui->level_json);

  *tui = (Tui){ 0 };
}

static int run_loop(Search *search, Query *query, Tui *tui, const volatile sig_atomic_t *interrupted) {
  LogView view = { .follow = true };

  while (!*interrupted && tui->running) {
    SearchStatus status = search_status(search);

    if (status.error) {
      errno = status.error;
      return 1;
    }

    search_sync_view(search, &view);
    tui_render(tui, search, &view, query, status.eof);

    if (tui->io_error) {
      errno = tui->io_error;
      return 1;
    }

    if (!tui_handle_key(tui, search, &view, query, getch())) return 1;
  }

  return *interrupted ? 130 : 0;
}

int tui_run(Search *search, Query *query, const volatile sig_atomic_t *interrupted) {
  Tui tui = { 0 };
  int result = tui_open(&tui) ? run_loop(search, query, &tui, interrupted) : 1;

  int error = errno;
  tui_close(&tui);
  errno = error;

  return result;
}
