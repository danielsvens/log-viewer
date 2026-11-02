#include "log_viewer/tui.h"
#include "log_viewer/json_support.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void apply_filter(Tui *tui, Search *search, Query *query, const char *text) {
  query_compile(query, text);

  search_request(search, query->text);

  tui->top = 0;
}

static void close_overlay(Tui *tui) {
  free(tui->detail);

  tui->detail = nullptr;
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

static size_t previous_character(const char *text, size_t cursor) {
  if (!cursor) return 0;

  cursor--;

  while (cursor && ((unsigned char)text[cursor] & 0xc0u) == 0x80u) cursor--;

  return cursor;
}

static size_t next_character(const char *text, size_t cursor) {
  if (!text[cursor]) return cursor;

  cursor++;

  while (((unsigned char)text[cursor] & 0xc0u) == 0x80u) cursor++;

  return cursor;
}

static void edit_filter_text(Tui *tui, int key) {
  size_t length = strlen(tui->input);
  size_t cursor = tui->input_cursor;
  bool changed = false;

  switch (key) {
  case KEY_LEFT:
    tui->input_cursor = previous_character(tui->input, cursor);
    break;
  case KEY_RIGHT:
    tui->input_cursor = next_character(tui->input, cursor);
    break;
  case KEY_HOME:
  case 1:
    tui->input_cursor = 0;
    break;
  case KEY_END:
  case 5:
    tui->input_cursor = length;
    break;
  case KEY_BACKSPACE:
  case 127:
  case 8:
    if (cursor) {
      size_t previous = previous_character(tui->input, cursor);
      memmove(tui->input + previous, tui->input + cursor, length - cursor + 1);
      tui->input_cursor = previous;
      changed = true;
    }
    break;
  case KEY_DC:
    if (cursor < length) {
      size_t next = next_character(tui->input, cursor);
      memmove(tui->input + cursor, tui->input + next, length - next + 1);
      changed = true;
    }
    break;
  case 21:
    tui->input[0] = 0;
    tui->input_cursor = 0;
    changed = true;
    break;
  case 11:
    tui->input[cursor] = 0;
    changed = true;
    break;
  default:
    if (key >= 32 && key <= 255 && length + 1 < sizeof(tui->input)) {
      memmove(tui->input + cursor + 1, tui->input + cursor, length - cursor + 1);
      tui->input[cursor] = (char)key;
      tui->input_cursor++;
      changed = true;
    }
    break;
  }

  if (changed) tui->input_error[0] = 0;
}

static bool handle_filter_key(Tui *tui, Search *search, Query *query, int key) {
  if (key == 27) {
    tui->editing = false;
    tui->input_error[0] = 0;
    return true;
  }

  if (key == '\n' || key == KEY_ENTER) {
    Query candidate = { 0 };
    bool valid = query_compile(&candidate, tui->input);
    bool allocation_failed = candidate.allocation_failed;

    snprintf(tui->input_error, sizeof(tui->input_error), "%s", candidate.error);
    query_destroy(&candidate);

    if (allocation_failed) {
      errno = ENOMEM;
      return false;
    }
    if (!valid) return true;

    /* Keep the active regex at its original address; compile it only on submission. */
    apply_filter(tui, search, query, tui->input);

    if (query->allocation_failed) {
      errno = ENOMEM;
      return false;
    }

    tui->editing = false;
    return true;
  }

  edit_filter_text(tui, key);

  return true;
}

static void start_filter_edit(Tui *tui, const Query *query) {
  memcpy(tui->input, query->text, sizeof(tui->input));
  tui->input_cursor = strlen(tui->input);
  tui->input_scroll = 0;
  tui->input_error[0] = 0;

  tui->editing = true;
}

static bool open_detail(Tui *tui, Search *search, const LogView *store) {
  LogRecord record;
  int read = search_read_result(search, store->selected, &tui->reader, &tui->line, &record, 0);

  if (read <= 0) return read == 0;

  char *detail = json_format_message(record.text);

  if (!detail) return false;

  tui->detail = detail;
  tui->overlay = OVERLAY_DETAIL;
  tui->detail_scroll = tui->detail_horizontal = 0;

  return true;
}

static bool handle_navigation_key(Tui *tui, Search *search, LogView *store, Query *query, int key) {
  switch (key) {
  case 'q':
    tui->running = false;
    break;

  case 'j':
  case KEY_DOWN:
    log_view_move(search, store, 1);
    break;

  case 'k':
  case KEY_UP:
    log_view_move(search, store, -1);
    break;

  case 4:
  case KEY_NPAGE:
    log_view_move(search, store, LINES / 2);
    break;

  case 21:
  case KEY_PPAGE:
    log_view_move(search, store, -LINES / 2);
    break;

  case 'G':
  case KEY_END:
    log_view_set_follow(search, store, true);
    break;

  case 'g':
    if (tui->pending_g) log_view_first(store);
    break;

  case KEY_HOME:
    log_view_first(store);
    break;

  case 'f':
    log_view_set_follow(search, store, !store->follow);
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
    apply_filter(tui, search, query, "");
    break;

  case '?':
    tui->overlay = OVERLAY_HELP;
    tui->detail_scroll = tui->detail_horizontal = 0;
    break;

  case '\n':
  case KEY_ENTER:
    if (!open_detail(tui, search, store)) return false;
    break;

  default:
    break;
  }

  tui->pending_g = key == 'g' && !tui->pending_g;

  return true;
}

static void handle_tree_key(Tui *tui, Search *search, LogView *view, int key) {
  SourceTree *tree = &tui->tree;
  size_t page = LINES > 6 ? (size_t)(LINES - 6) : 1;

  switch (key) {
  case 'j':
  case KEY_DOWN:
    if (tree->cursor + 1 < tree->count) tree->cursor++;
    break;

  case 'k':
  case KEY_UP:
    if (tree->cursor) tree->cursor--;
    break;

  case 4:
  case KEY_NPAGE:
    tree->cursor = tree->count && page < tree->count - tree->cursor ? tree->cursor + page : (tree->count ? tree->count - 1 : 0);
    break;

  case 21:
  case KEY_PPAGE:
    tree->cursor = tree->cursor > page ? tree->cursor - page : 0;
    break;

  case 'g':
  case KEY_HOME:
    tree->cursor = 0;
    break;

  case 'G':
  case KEY_END:
    tree->cursor = tree->count ? tree->count - 1 : 0;
    break;

  case 'a':
    tree->cursor = 0;
    [[fallthrough]];
  case ' ':
  case '\n':
  case KEY_ENTER:
    if (tree->cursor < tree->count && !tree->rows[tree->cursor].directory) {
      search_select_source(search, tree->rows[tree->cursor].source);
      tui->top = 0;
      view->selected = view->anchor = 0;
      view->follow = true;
      tui->pending_g = false;
    }
    break;

  case 27:
    tui->tree_focus = false;
    break;

  default:
    break;
  }
}

bool tui_handle_key(Tui *tui, Search *search, LogView *store, Query *query, int key) {
  if (key == ERR) return true;

  if (tui->overlay != OVERLAY_NONE) handle_overlay_key(tui, key);
  else if (tui->editing) return handle_filter_key(tui, search, query, key);
  else if (key >= '0' && key <= '5') {
    LogLevelMask levels = key == '0' ? LOG_LEVELS_ALL : search_levels(search) ^ (1u << (unsigned int)(key - '1'));
    search_set_levels(search, levels);
    tui->top = 0;
    tui->pending_g = false;
  } else if (key == '\t') {
    tui->tree_visible = true;
    tui->tree_focus = !tui->tree_focus;
    tui->pending_g = false;
  } else if (key == 's') {
    tui->tree_visible = !tui->tree_visible;
    if (!tui->tree_visible) tui->tree_focus = false;
  } else if (tui->tree_focus && key != 'q' && key != '?' && key != '/' && key != 'c') handle_tree_key(tui, search, store, key);
  else return handle_navigation_key(tui, search, store, query, key);

  return true;
}
