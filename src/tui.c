#include "log_viewer/tui.h"

#include <errno.h>
#include <stdlib.h>

static void initialize_colors(void) {
  if (!has_colors()) return;
  start_color();
  use_default_colors();
  init_pair(TUI_HEADER, COLOR_BLACK, COLOR_CYAN);
  init_pair(TUI_ERROR, COLOR_RED, -1);
  init_pair(TUI_WARNING, COLOR_YELLOW, -1);
  init_pair(TUI_NORMAL, COLOR_GREEN, -1);
  init_pair(TUI_MUTED, COLOR_CYAN, -1);
  init_pair(TUI_STRING, COLOR_YELLOW, -1);
  init_pair(TUI_LITERAL, COLOR_MAGENTA, -1);
}

bool tui_open(Tui *tui) {
  *tui = (Tui){ 0 };
  tui->tty = fopen("/dev/tty", "r+");
  if (!tui->tty) return false;
  tui->screen = newterm(NULL, tui->tty, tui->tty);
  if (!tui->screen) {
    fclose(tui->tty);
    tui->tty = NULL;
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
  return true;
}

void tui_close(Tui *tui) {
  if (tui->screen) {
    endwin();
    delscreen(tui->screen);
  }
  if (tui->tty) fclose(tui->tty);
  free(tui->detail);
  record_reader_destroy(&tui->reader);
  text_buffer_destroy(&tui->line);
  *tui = (Tui){ 0 };
}
