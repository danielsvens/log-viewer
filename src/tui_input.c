#include "log_viewer/tui.h"
#include "log_viewer/json_support.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

static void apply_filter(Tui *tui, Search *search, Query *query, const char *text) {
  query_compile(query, text);

  search_request(search, query->text);

  tui->top = 0;
  tui->restoring_tab = false;
}

static void close_overlay(Tui *tui) {
  free(tui->detail);

  tui->detail = nullptr;
  tui->overlay = OVERLAY_NONE;
  tui->detail_searching = false;
}

extern char **environ;

static bool clipboard_command(FILE *input, const char *const arguments[]) {
  posix_spawn_file_actions_t actions;

  if (posix_spawn_file_actions_init(&actions)) return false;

  bool ready = posix_spawn_file_actions_adddup2(&actions, fileno(input), STDIN_FILENO) == 0 &&
               posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
               posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
  pid_t child;
  rewind(input);
  int error = ready ? posix_spawnp(&child, arguments[0], &actions, nullptr, (char *const *)arguments, environ) : EIO;
  posix_spawn_file_actions_destroy(&actions);

  if (error) return false;

  int status;
  pid_t result;

  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);

  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool copy_detail(Tui *tui) {
  if (!tui->detail) return false;

  FILE *input = tmpfile();
  bool copied = false;

  if (input) {
    (void)fcntl(fileno(input), F_SETFD, FD_CLOEXEC);

    if (fwrite(tui->detail, 1, strlen(tui->detail), input) == strlen(tui->detail) && fflush(input) == 0) {
#ifdef __APPLE__
      const char *const commands[][5] = { { "pbcopy", nullptr } };
#else
      const char *const commands[][5] = { { "wl-copy", nullptr },
                                          { "xclip", "-selection", "clipboard", nullptr },
                                          { "xsel", "--clipboard", "--input", nullptr } };
#endif
      for (size_t i = 0; i < sizeof(commands) / sizeof(*commands) && !copied; i++) copied = clipboard_command(input, commands[i]);
    }

    fclose(input);
  }

  if (copied) {
    snprintf(tui->detail_status, sizeof(tui->detail_status), "Copied entire record to clipboard");
    return true;
  }
  if (!tui->tty) return false;

  static constexpr char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char *text = (const unsigned char *)tui->detail;
  size_t length = strlen(tui->detail);
  bool written = fputs("\033]52;c;", tui->tty) >= 0;

  for (size_t i = 0; written && i < length; i += 3) {
    size_t remaining = length - i;
    unsigned int value = (unsigned int)text[i] << 16;
    if (remaining > 1) value |= (unsigned int)text[i + 1] << 8;
    if (remaining > 2) value |= text[i + 2];

    char encoded[] = {
      BASE64[value >> 18], BASE64[(value >> 12) & 63], remaining > 1 ? BASE64[(value >> 6) & 63] : '=', remaining > 2 ? BASE64[value & 63] : '='
    };
    written = fwrite(encoded, 1, sizeof(encoded), tui->tty) == sizeof(encoded);
  }

  written = written && fputc('\a', tui->tty) != EOF && fflush(tui->tty) == 0;
  snprintf(tui->detail_status, sizeof(tui->detail_status), "%s", written ? "Copy sent to terminal clipboard (OSC 52)" : "Clipboard write failed");

  return written;
}

static void find_detail(Tui *tui, bool backwards, bool first) {
  if (!tui->detail || !*tui->detail_query) {
    tui->detail_match = SIZE_MAX;
    tui->detail_status[0] = 0;
    return;
  }

  size_t found = SIZE_MAX, wrap = SIZE_MAX;
  size_t current = tui->detail_match;

  for (const char *p = tui->detail; (p = strcasestr(p, tui->detail_query)); p++) {
    size_t offset = (size_t)(p - tui->detail);

    if (backwards) {
      wrap = offset;
      if (current == SIZE_MAX || offset < current) found = offset;
    } else {
      if (wrap == SIZE_MAX) wrap = offset;
      if (found == SIZE_MAX && (first || current == SIZE_MAX || offset > current)) found = offset;
    }
  }

  tui->detail_match = found != SIZE_MAX ? found : wrap;

  if (tui->detail_match == SIZE_MAX) {
    snprintf(tui->detail_status, sizeof(tui->detail_status), "No matches");
    return;
  }

  tui->detail_scroll = 0;
  const char *line = tui->detail;

  for (size_t i = 0; i < tui->detail_match; i++) {
    if (tui->detail[i] == '\n') {
      tui->detail_scroll++;
      line = tui->detail + i + 1;
    }
  }

  size_t columns = 0;
  mbstate_t state = { 0 };

  while (line < tui->detail + tui->detail_match) {
    wchar_t value;
    size_t bytes = mbrtowc(&value, line, (size_t)(tui->detail + tui->detail_match - line), &state);

    if (bytes == (size_t)-1 || bytes == (size_t)-2 || !bytes) {
      memset(&state, 0, sizeof(state));
      bytes = 1;
      value = L'?';
    }

    int cells = wcwidth(value);
    columns += cells < 0 ? 1 : (size_t)cells;
    line += bytes;
  }

  tui->detail_horizontal = columns > 8 ? columns - 8 : 0;
  snprintf(tui->detail_status, sizeof(tui->detail_status), "Match: %.120s (n next, N previous)", tui->detail_query);
}

static void edit_filter_text(Tui *tui, int key);

static void handle_detail_search(Tui *tui, int key) {
  if (key == 27) {
    tui->detail_searching = false;
    return;
  }
  if (key == '\n' || key == KEY_ENTER) {
    memcpy(tui->detail_query, tui->input, sizeof(tui->detail_query));
    tui->detail_searching = false;
    find_detail(tui, false, true);
    return;
  }

  edit_filter_text(tui, key);
}

static void handle_overlay_key(Tui *tui, int key) {
  if (tui->detail_searching) {
    handle_detail_search(tui, key);
    return;
  }

  if (tui->overlay == OVERLAY_DETAIL && key == '/') {
    memcpy(tui->input, tui->detail_query, sizeof(tui->input));
    tui->input_cursor = strlen(tui->input);
    tui->input_scroll = 0;
    tui->detail_searching = true;
    return;
  }
  if (tui->overlay == OVERLAY_DETAIL && (key == 'n' || key == 'N')) {
    find_detail(tui, key == 'N', false);
    return;
  }
  if (tui->overlay == OVERLAY_DETAIL && key == 'y') {
    if (!copy_detail(tui)) snprintf(tui->detail_status, sizeof(tui->detail_status), "Clipboard unavailable");
    return;
  }

  if (tui->overlay == OVERLAY_QUIT) {
    if (key == 'y' || key == 'Y') {
      tui->running = false;
      close_overlay(tui);
    } else if (key == 27 || key == 'n' || key == 'N' || key == 'q' || key == '\n' || key == KEY_ENTER) close_overlay(tui);

    return;
  }

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

static void save_filter_tab(Tui *tui, Search *search, const LogView *view, const Query *query) {
  FilterTab *tab = &tui->tabs[tui->active_tab];
  memcpy(tab->query, query->text, sizeof(tab->query));
  tab->levels = search_levels(search);
  tab->source = search_selected_source(search);
  tab->view = *view;

  tab->top = tui->restoring_tab ? tui->restore_top : tui->top;
  tab->horizontal = tui->horizontal;
  tab->tree_cursor = tui->tree.cursor;
  tab->tree_top = tui->tree.top;
}

static bool activate_filter_tab(Tui *tui, Search *search, LogView *view, Query *query, size_t index) {
  FilterTab *tab = &tui->tabs[index];

  if (!query_compile(query, tab->query) && query->allocation_failed) {
    errno = ENOMEM;
    return false;
  }

  search_configure(search, query->text, tab->source, tab->levels);
  *view = tab->view;
  tui->active_tab = index;
  tui->top = tui->restore_top = tab->top;
  tui->horizontal = tab->horizontal;
  tui->tree.cursor = tab->tree_cursor;
  tui->tree.top = tab->tree_top;

  tui->restoring_tab = true;
  tui->pending_g = false;
  tui->selected_source = nullptr;

  return true;
}

static bool handle_tab_key(Tui *tui, Search *search, LogView *view, Query *query, int key) {
  if (key == 't') {
    size_t count = tui->tab_count ? tui->tab_count : 1;

    if (count >= SIZE_MAX / sizeof(*tui->tabs)) {
      errno = ENOMEM;
      return false;
    }

    FilterTab *tabs = realloc(tui->tabs, (count + 1) * sizeof(*tabs));

    if (!tabs) return false;

    tui->tabs = tabs;

    if (!tui->tab_count) {
      tui->tabs[0] = (FilterTab){ .id = 1 };
      tui->tab_count = 1;
      tui->next_tab_id = 2;
    }

    save_filter_tab(tui, search, view, query);
    tabs[count] =
        (FilterTab){ .levels = search_levels(search), .source = search_selected_source(search), .view = { .follow = true }, .id = tui->next_tab_id++ };
    tui->tab_count = count + 1;

    if (!activate_filter_tab(tui, search, view, query, count)) return false;

    start_filter_edit(tui, query);
    tui->tree_focus = false;

    return true;
  }
  if (tui->tab_count < 2) return true;

  save_filter_tab(tui, search, view, query);
  size_t index = tui->active_tab;

  if (key == 'x') {
    memmove(tui->tabs + index, tui->tabs + index + 1, (tui->tab_count - index - 1) * sizeof(*tui->tabs));
    tui->tab_count--;
    if (index == tui->tab_count) index--;
  } else if (key == KEY_SRIGHT) index = (index + 1) % tui->tab_count;
  else index = index ? index - 1 : tui->tab_count - 1;

  return activate_filter_tab(tui, search, view, query, index);
}

static bool open_detail(Tui *tui, Search *search, const LogView *store) {
  LogRecord record;
  int read = search_read_result(search, store->selected, &tui->reader, &tui->line, &record, 0);

  if (read <= 0) return read == 0;

  char *detail = json_format_message(record.text);

  if (!detail) return false;

  tui->detail = detail;
  tui->detail_searching = false;
  tui->detail_query[0] = tui->detail_status[0] = 0;
  tui->detail_match = SIZE_MAX;
  tui->overlay = OVERLAY_DETAIL;
  tui->detail_scroll = tui->detail_horizontal = 0;

  return true;
}

static bool handle_navigation_key(Tui *tui, Search *search, LogView *store, Query *query, int key) {
  if (key == 'j' || key == 'k' || key == 'g' || key == 'G' || key == 'f' || key == KEY_UP || key == KEY_DOWN || key == KEY_HOME || key == KEY_END || key == 4 ||
      key == 21 || key == KEY_NPAGE || key == KEY_PPAGE)
    tui->restoring_tab = false;

  switch (key) {
  case 'q':
    tui->overlay = OVERLAY_QUIT;
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
      tui->restoring_tab = false;
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
  else if (key == 't' || key == KEY_SLEFT || key == KEY_SRIGHT || key == 'x') return handle_tab_key(tui, search, store, query, key);
  else if (key >= '0' && key <= '5') {
    LogLevelMask levels = key == '0' ? LOG_LEVELS_ALL : search_levels(search) ^ (1u << (unsigned int)(key - '1'));
    search_set_levels(search, levels);
    tui->top = 0;
    tui->restoring_tab = false;
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
