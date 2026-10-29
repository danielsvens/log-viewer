#include "log_viewer/tui.h"
#include "log_viewer/json_support.h"

#include <stdlib.h>
#include <string.h>

static void apply_filter(Tui *tui, LogStore *store, Query *query, const char *text) {
  query_compile(query, text);
  log_store_refilter(store, query);
  tui->top = 0;
}

static void close_overlay(Tui *tui) {
  free(tui->detail);
  tui->detail = NULL;
  tui->overlay = OVERLAY_NONE;
}

static void handle_overlay_key(Tui *tui, int key) {
  if (key == 27 || key == 'q' || key == '\n' || key == KEY_ENTER) {
    close_overlay(tui);
    return;
  }
  size_t page = (size_t)(LINES > 6 ? LINES - 6 : 1);
  switch (key) {
  case 'j':
  case KEY_DOWN:
    tui->detail_scroll++;
    break;
  case 'k':
  case KEY_UP:
    if (tui->detail_scroll) tui->detail_scroll--;
    break;
  case 'g':
    tui->detail_scroll = 0;
    break;
  case 'h':
  case KEY_LEFT:
    tui->detail_horizontal = tui->detail_horizontal >= 8 ? tui->detail_horizontal - 8 : 0;
    break;
  case 'l':
  case KEY_RIGHT:
    tui->detail_horizontal += 8;
    break;
  case 4:
  case KEY_NPAGE:
    tui->detail_scroll += page;
    break;
  case 21:
  case KEY_PPAGE:
    tui->detail_scroll = tui->detail_scroll > page ? tui->detail_scroll - page : 0;
    break;
  default:
    break;
  }
}

static void edit_filter_text(Tui *tui, int key) {
  size_t length = strlen(tui->input);
  if (key == KEY_BACKSPACE || key == 127 || key == 8) {
    if (!length) return;
    do {
      length--;
    } while (length && ((unsigned char)tui->input[length] & 0xc0u) == 0x80u);
    tui->input[length] = 0;
  } else if (key == 21) tui->input[0] = 0;
  else if (key >= 32 && key <= 255 && length + 1 < sizeof(tui->input)) {
    tui->input[length] = (char)key;
    tui->input[length + 1] = 0;
  }
}

static void handle_filter_key(Tui *tui, LogStore *store, Query *query, int key) {
  if (key == 27) {
    tui->editing = false;
    apply_filter(tui, store, query, tui->saved_filter);
    return;
  }
  if (key == '\n' || key == KEY_ENTER) {
    tui->editing = false;
    return;
  }
  edit_filter_text(tui, key);
  apply_filter(tui, store, query, tui->input);
}

static void start_filter_edit(Tui *tui, const Query *query) {
  memcpy(tui->saved_filter, query->text, sizeof(tui->saved_filter));
  memcpy(tui->input, query->text, sizeof(tui->input));
  tui->editing = true;
}

static bool open_detail(Tui *tui, const LogStore *store) {
  const LogRecord *record = log_store_selected(store);
  if (!record) return true;
  char *detail = json_format_message(record->text);
  if (!detail) return false;

  tui->detail = detail;
  tui->overlay = OVERLAY_DETAIL;
  tui->detail_scroll = tui->detail_horizontal = 0;
  return true;
}

static bool handle_navigation_key(Tui *tui, LogStore *store, Query *query, int key) {
  switch (key) {
  case 'q':
    tui->running = false;
    break;
  case 'j':
  case KEY_DOWN:
    log_store_move(store, 1);
    break;
  case 'k':
  case KEY_UP:
    log_store_move(store, -1);
    break;
  case 4:
  case KEY_NPAGE:
    log_store_move(store, LINES / 2);
    break;
  case 21:
  case KEY_PPAGE:
    log_store_move(store, -LINES / 2);
    break;
  case 'G':
  case KEY_END:
    log_store_set_follow(store, true);
    break;
  case 'g':
    if (tui->pending_g) log_store_first(store);
    break;
  case KEY_HOME:
    log_store_first(store);
    break;
  case 'f':
    log_store_set_follow(store, !store->follow);
    break;
  case 'h':
  case KEY_LEFT:
    tui->horizontal = tui->horizontal >= 8 ? tui->horizontal - 8 : 0;
    break;
  case 'l':
  case KEY_RIGHT:
    tui->horizontal += 8;
    break;
  case '/':
    start_filter_edit(tui, query);
    break;
  case 'c':
    apply_filter(tui, store, query, "");
    break;
  case '?':
    tui->overlay = OVERLAY_HELP;
    tui->detail_scroll = tui->detail_horizontal = 0;
    break;
  case '\n':
  case KEY_ENTER:
    if (!open_detail(tui, store)) return false;
    break;
  default:
    break;
  }
  tui->pending_g = key == 'g' && !tui->pending_g;
  return true;
}

bool tui_handle_key(Tui *tui, LogStore *store, Query *query, int key) {
  if (key == ERR) return true;
  if (tui->overlay != OVERLAY_NONE) handle_overlay_key(tui, key);
  else if (tui->editing) handle_filter_key(tui, store, query, key);
  else return handle_navigation_key(tui, store, query, key);
  return true;
}
