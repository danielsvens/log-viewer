#include "log_viewer/tui.h"

#include <errno.h>
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
  source_tree_destroy(&tui->tree);
  record_reader_destroy(&tui->reader);
  text_buffer_destroy(&tui->line);

  *tui = (Tui){ 0 };
}
