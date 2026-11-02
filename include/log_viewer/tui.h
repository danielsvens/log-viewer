#ifndef LOG_VIEWER_TUI_H
#define LOG_VIEWER_TUI_H

#include <ncurses.h>
#include <stddef.h>
#include <stdio.h>
#include "log_viewer/search.h"
#include "log_viewer/query.h"
#include "log_viewer/source_tree.h"

typedef enum { TUI_HEADER = 1, TUI_ERROR, TUI_WARNING, TUI_NORMAL, TUI_MUTED, TUI_STRING, TUI_LITERAL } TuiColor;

typedef enum { OVERLAY_NONE, OVERLAY_DETAIL, OVERLAY_HELP, OVERLAY_QUIT } Overlay;

typedef struct {
  FILE *tty;
  SCREEN *screen;
  bool running, editing, pending_g, tree_visible, tree_focus;
  SourceTree tree;
  Overlay overlay;
  size_t top, horizontal, detail_scroll, detail_horizontal;
  char input[QUERY_CAPACITY], input_error[160];
  size_t input_cursor, input_scroll;
  char *detail;
  bool detail_searching;
  char detail_query[QUERY_CAPACITY], detail_status[160];
  size_t detail_match;
  const char *selected_source;
  RecordReader reader;
  TextBuffer line;
  JsonWorkspace level_json;
  int io_error;
} Tui;

bool tui_open(Tui *tui);
void tui_close(Tui *tui);
void tui_render(Tui *tui, Search *search, const LogView *store, const Query *query, bool eof);
/* False signals a failure to allocate modal text. ERR keys are ignored. */
bool tui_handle_key(Tui *tui, Search *search, LogView *store, Query *query, int key);

#endif
