#ifndef LOG_VIEWER_TUI_H
#define LOG_VIEWER_TUI_H

#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "log_viewer/search.h"
#include "log_viewer/query.h"

typedef enum { TUI_HEADER = 1, TUI_ERROR, TUI_WARNING, TUI_NORMAL, TUI_MUTED, TUI_STRING, TUI_LITERAL } TuiColor;

typedef enum { OVERLAY_NONE, OVERLAY_DETAIL, OVERLAY_HELP } Overlay;

typedef struct {
  FILE *tty;
  SCREEN *screen;
  bool running, editing, pending_g;
  Overlay overlay;
  size_t top, horizontal, detail_scroll, detail_horizontal;
  char input[QUERY_CAPACITY], saved_filter[QUERY_CAPACITY];
  char *detail;
  RecordReader reader;
  TextBuffer line;
  int io_error;
} Tui;

bool tui_open(Tui *tui);
void tui_close(Tui *tui);
void tui_render(Tui *tui, Search *search, const LogView *store, const Query *query, bool eof);
/* False signals a failure to allocate modal text. ERR keys are ignored. */
bool tui_handle_key(Tui *tui, Search *search, LogView *store, Query *query, int key);

#endif
