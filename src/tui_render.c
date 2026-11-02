#include "log_viewer/tui.h"
#include "log_viewer/text.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static constexpr unsigned char HELP_TEXT[] = {
#embed "../resources/help.txt" suffix(, 0) if_empty(0)
};

static constexpr unsigned char KEY_HINT_TEXT[] = {
#embed "../resources/key-hints.txt" suffix(, 0) if_empty(0)
};

typedef struct {
  wchar_t value;
  size_t bytes;
  int cells;
} Glyph;

typedef struct {
  bool quoted, escaped;
  TuiColor base;
} Syntax;

static TuiColor severity_color(LogLevel level) {
  switch (level) {
  case LOG_LEVEL_ERROR:
    return TUI_ERROR;

  case LOG_LEVEL_WARN:
    return TUI_WARNING;

  case LOG_LEVEL_DEBUG:
    return TUI_MUTED;

  default:
    return TUI_NORMAL;
  }
}

static Glyph read_glyph(const char *text, mbstate_t *state) {
  Glyph glyph = { 0 };
  glyph.bytes = mbrtowc(&glyph.value, text, MB_CUR_MAX, state);

  if (glyph.bytes == (size_t)-1 || glyph.bytes == (size_t)-2) {
    memset(state, 0, sizeof(*state));
    glyph.value = L'?';
    glyph.bytes = 1;
  }

  glyph.cells = wcwidth(glyph.value);

  if (glyph.cells < 0) {
    glyph.value = L'?';
    glyph.cells = 1;
  }

  return glyph;
}

static TuiColor syntax_color(Syntax *syntax, unsigned char byte) {
  TuiColor color = syntax->base;

  if (byte == '"' && !syntax->escaped) {
    syntax->quoted = !syntax->quoted;
    color = TUI_STRING;
  } else if (syntax->quoted) color = TUI_STRING;
  else if (isdigit(byte) || strchr("{}[]:", byte)) color = TUI_LITERAL;

  syntax->escaped = syntax->quoted && byte == '\\' && !syntax->escaped;

  return color;
}

/* Draw UTF-8 by terminal cells; invalid input is replaced instead of truncating. */
static void paint_text(int y, int x, int width, const char *text, size_t skip, bool active, LogLevel level) {
  if (width <= 0) return;

  size_t column = 0;
  int used = 0;
  mbstate_t state = { 0 };
  Syntax syntax = { .base = severity_color(level) };

  move(y, x);

  for (const char *p = text; *p && *p != '\n' && used < width;) {
    Glyph glyph = read_glyph(p, &state);

    if (!glyph.bytes) break;

    TuiColor color = syntax_color(&syntax, (unsigned char)*p);

    if (column >= skip && used + glyph.cells <= width) {
      attrset(COLOR_PAIR(color) | (active ? A_REVERSE : 0));
      addnwstr(&glyph.value, 1);
      used += glyph.cells;
    }

    column += (size_t)glyph.cells;
    p += glyph.bytes;
  }

  attrset(A_NORMAL);
}

static void render_header(const LogView *store, bool eof, int width) {
  attrset(COLOR_PAIR(TUI_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', width);
  mvprintw(
      0,
      1,
      "LOG VIEWER  %s  %zu/%zu  %s",
      store->follow ? "FOLLOW" : "PAUSED",
      store->matches,
      store->count,
      store->searching ? "SEARCHING / INDEXING" : (eof ? "waiting / EOF" : "LIVE")
  );

  if (store->unavailable) mvprintw(0, width > 55 ? width - 40 : 0, "%zu records lost to source truncation", store->unavailable);

  attrset(A_NORMAL);
}

static void render_record(const LogRecord *record, int row, int left, int width, size_t horizontal, bool selected, LogLevel level) {
  if (selected) {
    attrset(A_REVERSE);
    mvhline(row, left, ' ', width);
  }

  attrset(COLOR_PAIR(TUI_MUTED) | (selected ? A_REVERSE : 0));
  mvprintw(row, left, "%7" PRIu64 " %c ", record->id, selected ? '>' : ' ');
  paint_text(row, left + 10, width - 10, record->text, horizontal, selected, level);
}

static void render_records(Tui *tui, Search *search, const LogView *store, const Query *query, int height, int left, int width) {
  size_t rows = (size_t)(height - 5);
  tui->selected_source = nullptr;

  if (tui->restoring_tab) {
    if (store->searching) {
      mvaddstr(2, left + 2, "Searching...");
      return;
    }

    tui->top = tui->restore_top;
    tui->restoring_tab = false;
  }

  if (store->selected < tui->top) tui->top = store->selected;
  if (store->selected >= tui->top + rows) tui->top = store->selected - rows + 1;

  for (size_t row = 0; row < rows && tui->top + row < store->matches; row++) {
    size_t index = tui->top + row;
    LogRecord record;
    int read = search_read_result(search, index, &tui->reader, &tui->line, &record, 65536);

    if (read < 0) {
      tui->io_error = errno;
      break;
    }
    if (!read) continue;

    if (index == store->selected) tui->selected_source = record.source;

    LogLevel level;

    if (!log_level_detect(record.text, tui->line.length, &tui->level_json, &level)) {
      tui->io_error = errno ? errno : ENOMEM;
      break;
    }

    render_record(&record, (int)row + 1, left, width, tui->horizontal, index == store->selected, level);
  }

  if (!store->matches) mvaddstr(2, left + 2, *query->error ? "Invalid query" : (store->searching ? "Searching..." : "No matching logs"));
}

static void render_tree(Tui *tui, Search *search, int height, int width) {
  attrset(COLOR_PAIR(TUI_MUTED) | (tui->tree_focus ? A_BOLD : 0));
  mvaddnstr(1, 1, tui->tree_focus ? "Sources [Tab: logs]" : "Sources [Tab: focus]", width - 2);
  mvvline(1, width - 1, ACS_VLINE, height - 5);

  size_t rows = height > 6 ? (size_t)(height - 6) : 0;

  if (!rows) return;

  SourceTree *tree = &tui->tree;

  if (tree->cursor < tree->top) tree->top = tree->cursor;
  if (tree->cursor >= tree->top + rows) tree->top = tree->cursor - rows + 1;

  size_t selected = search_selected_source(search);

  for (size_t i = 0; i < rows && tree->top + i < tree->count; i++) {
    size_t index = tree->top + i;
    SourceTreeRow row = tree->rows[index];
    bool focused = tui->tree_focus && index == tree->cursor;
    bool active = !row.directory && row.source == selected;

    attrset(COLOR_PAIR(TUI_MUTED) | (focused ? A_REVERSE : 0));
    mvhline((int)i + 2, 0, ' ', width - 1);
    mvaddch((int)i + 2, 0, active ? '*' : ' ');

    size_t indent = row.depth * 2 + 2;
    if (indent > (size_t)width / 2) indent = (size_t)width / 2;

    char label[512];
    size_t length = row.length < sizeof(label) - 1 ? row.length : sizeof(label) - 1;
    memcpy(label, row.name, length);
    label[length] = 0;

    paint_text((int)i + 2, (int)indent, width - (int)indent - 2, label, 0, focused, LOG_LEVEL_UNKNOWN);
  }

  attrset(A_NORMAL);
}

static size_t filter_columns(const char *text, size_t start, size_t end) {
  size_t columns = 0;
  mbstate_t state = { 0 };

  while (start < end) {
    Glyph glyph = read_glyph(text + start, &state);

    if (!glyph.bytes || glyph.bytes > end - start) break;

    columns += (size_t)glyph.cells;
    start += glyph.bytes;
  }

  return columns;
}

static void scroll_filter_input(Tui *tui, int width) {
  size_t available = (size_t)(width - 3);
  size_t columns = filter_columns(tui->input, 0, tui->input_cursor);
  mbstate_t state = { 0 };
  tui->input_scroll = 0;

  while (columns >= available && tui->input_scroll < tui->input_cursor) {
    Glyph glyph = read_glyph(tui->input + tui->input_scroll, &state);
    columns -= (size_t)glyph.cells;
    tui->input_scroll += glyph.bytes;
  }
}

static void render_filter_tabs(const Tui *tui, const Query *query, int row, int width) {
  size_t count = tui->tab_count ? tui->tab_count : 1;
  size_t visible = width > 36 ? (size_t)(width - 12) / 24 : 1;
  if (visible > count) visible = count;

  size_t first = tui->active_tab / visible * visible;
  int slot = (width - 12) / (int)visible;
  attrset(COLOR_PAIR(TUI_MUTED));
  mvprintw(row, 0, "Tabs %zu/%zu", tui->active_tab + 1, count);

  for (size_t i = first; i < count && i < first + visible; i++) {
    bool active = i == tui->active_tab;
    const char *expression = active ? query->text : tui->tabs[i].query;
    size_t id = tui->tab_count ? tui->tabs[i].id : 1;
    char label[QUERY_CAPACITY + 32];
    snprintf(label, sizeof(label), "%zu: %s", id, *expression ? expression : "All logs");
    int left = 12 + (int)(i - first) * slot;

    attrset(COLOR_PAIR(TUI_MUTED) | (active ? A_REVERSE : 0));
    mvhline(row, left, ' ', slot - 1);
    paint_text(row, left, slot - 1, label, 0, active, LOG_LEVEL_UNKNOWN);
  }

  attrset(A_NORMAL);
}

static void render_footer(Tui *tui, const Query *query, int height, int width) {
  attrset(COLOR_PAIR(TUI_MUTED));
  mvhline(height - 4, 0, ACS_HLINE, width);

  render_filter_tabs(tui, query, height - 4, width);
  mvaddnstr(height - 3, 1, (const char *)KEY_HINT_TEXT, width - 2);

  const char *error = tui->editing ? tui->input_error : query->error;

  if (tui->editing) scroll_filter_input(tui, width);

  attrset(COLOR_PAIR(*error ? TUI_ERROR : TUI_NORMAL));
  mvprintw(height - 2, 0, "%c ", tui->editing ? '/' : '>');
  paint_text(height - 2, 2, width - 3, tui->editing ? tui->input + tui->input_scroll : query->text, 0, false, LOG_LEVEL_UNKNOWN);

  if (*error) {
    attrset(COLOR_PAIR(TUI_ERROR));
    mvaddnstr(height - 4, 1, error, width - 2);
  }
}

static void render_status(Search *search, const LogView *view, const Query *query, int height, int width) {
  char text[256];
  int length = snprintf(text, sizeof(text), width < 44 ? "Total: %zu" : "Total indexed: %zu", view->count);

  bool scoped = search_selected_source(search) != SEARCH_ALL_SOURCES;

  LogLevelMask levels = search_levels(search);

  if (*query->text || scoped || levels != LOG_LEVELS_ALL) {
    snprintf(text + length, sizeof(text) - (size_t)length, width < 44 ? " | Matched: %zu" : "  |  Matched: %zu", view->matches);
  }

  size_t used = strlen(text);

  if (view->unavailable) {
    snprintf(text + used, sizeof(text) - used, "  |  Unavailable: %zu", view->unavailable);
  }

  if (width >= 110) {
    used = strlen(text);
    snprintf(text + used, sizeof(text) - used, "  |  Levels:");

    for (unsigned int level = 0; level < LOG_LEVEL_COUNT; level++) {
      used = strlen(text);
      snprintf(text + used, sizeof(text) - used, " %u:%s%c", level + 1, log_level_name((LogLevel)level), levels & (1u << level) ? '+' : '-');
    }
  } else if (width >= 68) {
    used = strlen(text);
    snprintf(
        text + used,
        sizeof(text) - used,
        "  |  Levels: E%c W%c I%c D%c ?%c",
        levels & (1u << (unsigned int)LOG_LEVEL_ERROR) ? '+' : '-',
        levels & (1u << (unsigned int)LOG_LEVEL_WARN) ? '+' : '-',
        levels & (1u << (unsigned int)LOG_LEVEL_INFO) ? '+' : '-',
        levels & (1u << (unsigned int)LOG_LEVEL_DEBUG) ? '+' : '-',
        levels & (1u << (unsigned int)LOG_LEVEL_UNKNOWN) ? '+' : '-'
    );
  }

  attrset(COLOR_PAIR(TUI_HEADER));
  mvhline(height - 1, 0, ' ', width);
  mvaddnstr(height - 1, 0, text, width - 1);
  attrset(A_NORMAL);
}

static void render_overlay_frame(const Tui *tui, int height, int width) {
  int box_height = height - 2, box_width = width - 4;

  attrset(A_NORMAL);
  for (int row = 1; row < height - 1; row++) mvhline(row, 2, ' ', box_width);

  mvhline(1, 2, ACS_HLINE, box_width);
  mvhline(height - 2, 2, ACS_HLINE, box_width);
  mvvline(1, 2, ACS_VLINE, box_height);
  mvvline(1, width - 3, ACS_VLINE, box_height);

  const char *title = tui->overlay == OVERLAY_HELP ? " Keys & queries " : " Log detail — / search, n/N next/prev, y copy, q close ";

  mvaddnstr(1, 4, title, box_width - 4);
}

static void render_quit_confirmation(int height, int width) {
  int box_width = width > 54 ? 50 : width - 4;
  int left = (width - box_width) / 2;
  int top = (height - 6) / 2;

  attrset(COLOR_PAIR(TUI_NORMAL));
  for (int row = 0; row < 6; row++) mvhline(top + row, left, ' ', box_width);

  mvhline(top, left, ACS_HLINE, box_width);
  mvhline(top + 5, left, ACS_HLINE, box_width);
  mvvline(top, left, ACS_VLINE, 6);
  mvvline(top, left + box_width - 1, ACS_VLINE, 6);

  mvaddnstr(top, left + 2, " Quit? ", box_width - 4);
  paint_text(top + 2, left + 2, box_width - 4, "Quit log-viewer?", 0, false, LOG_LEVEL_UNKNOWN);
  paint_text(top + 3, left + 2, box_width - 4, "y: quit", 0, false, LOG_LEVEL_UNKNOWN);
  paint_text(top + 4, left + 2, box_width - 4, "n / Esc / Enter: stay", 0, false, LOG_LEVEL_UNKNOWN);
  attrset(A_NORMAL);
}

static void render_overlay(Tui *tui, int height, int width) {
  if (tui->overlay == OVERLAY_NONE) return;
  if (tui->overlay == OVERLAY_QUIT) {
    render_quit_confirmation(height, width);
    return;
  }

  const char *text = tui->overlay == OVERLAY_HELP ? (const char *)HELP_TEXT : tui->detail;
  size_t line = 0;
  int row = 3;

  render_overlay_frame(tui, height, width);

  int bottom = tui->overlay == OVERLAY_DETAIL ? height - 3 : height - 2;

  while (text && *text && row < bottom) {
    const char *end = strchr(text, '\n');

    if (line++ >= tui->detail_scroll) {
      bool matched = tui->overlay == OVERLAY_DETAIL && tui->detail_match != SIZE_MAX && tui->detail_match >= (size_t)(text - tui->detail) &&
                     (!end || tui->detail_match < (size_t)(end - tui->detail));
      paint_text(row++, 4, width - 8, text, tui->detail_horizontal, matched, LOG_LEVEL_UNKNOWN);
    }

    if (!end) break;

    text = end + 1;
  }

  if (tui->overlay == OVERLAY_DETAIL) {
    if (tui->detail_searching) {
      scroll_filter_input(tui, width - 7);
      mvaddstr(height - 3, 4, "/ ");
      paint_text(height - 3, 6, width - 10, tui->input + tui->input_scroll, 0, false, LOG_LEVEL_UNKNOWN);
    } else paint_text(height - 3, 4, width - 8, tui->detail_status, 0, false, LOG_LEVEL_UNKNOWN);
  }
}

static void render_cursor(const Tui *tui, int height, int width) {
  curs_set(tui->editing || tui->detail_searching ? 1 : 0);

  if (tui->detail_searching) {
    size_t column = filter_columns(tui->input, tui->input_scroll, tui->input_cursor) + 6;
    move(height - 3, column < (size_t)(width - 4) ? (int)column : width - 5);
    return;
  }
  if (!tui->editing) return;

  size_t column = filter_columns(tui->input, tui->input_scroll, tui->input_cursor) + 2;

  move(height - 2, column < (size_t)width ? (int)column : width - 1);
}

void tui_render(Tui *tui, Search *search, const LogView *store, const Query *query, bool eof) {
  int height = LINES, width = COLS;

  erase();

  if (height < 6 || width < 24) {
    mvaddstr(0, 0, "Resize terminal (24x6)");
    refresh();
    return;
  }

  if (!source_tree_sync(&tui->tree, search)) {
    tui->io_error = errno ? errno : ENOMEM;
    return;
  }

  int sidebar = tui->tree_visible && width >= 68 ? width / 3 : 0;
  if (sidebar > 36) sidebar = 36;

  bool tree_only = tui->tree_visible && tui->tree_focus && !sidebar;

  render_header(store, eof, width);

  if (sidebar || tree_only) render_tree(tui, search, height, sidebar ? sidebar : width);
  if (!tree_only) render_records(tui, search, store, query, height, sidebar, width - sidebar);
  render_footer(tui, query, height, width);
  render_overlay(tui, height, width);
  render_status(search, store, query, height, width);

  render_cursor(tui, height, width);
  refresh();
}
