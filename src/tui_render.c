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

static TuiColor severity_color(const char *text) {
  switch (text_log_tone(text)) {
  case LOG_ERROR:
    return TUI_ERROR;

  case LOG_WARNING:
    return TUI_WARNING;

  case LOG_DEBUG:
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
static void paint_text(int y, int x, int width, const char *text, size_t skip, bool active) {
  if (width <= 0) return;

  size_t column = 0;
  int used = 0;
  mbstate_t state = { 0 };
  Syntax syntax = { .base = severity_color(text) };

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

static void render_record(const LogRecord *record, int row, int left, int width, size_t horizontal, bool selected) {
  if (selected) {
    attrset(A_REVERSE);
    mvhline(row, left, ' ', width);
  }

  attrset(COLOR_PAIR(TUI_MUTED) | (selected ? A_REVERSE : 0));
  mvprintw(row, left, "%7" PRIu64 " %c ", record->id, selected ? '>' : ' ');
  paint_text(row, left + 10, width - 10, record->text, horizontal, selected);
}

static void render_records(Tui *tui, Search *search, const LogView *store, const Query *query, int height, int left, int width) {
  size_t rows = (size_t)(height - 4);
  tui->selected_source = nullptr;

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

    render_record(&record, (int)row + 1, left, width, tui->horizontal, index == store->selected);
  }

  if (!store->matches) mvaddstr(2, left + 2, *query->error ? "Invalid query" : (store->searching ? "Searching..." : "No matching logs"));
}

static void render_tree(Tui *tui, Search *search, int height, int width) {
  attrset(COLOR_PAIR(TUI_MUTED) | (tui->tree_focus ? A_BOLD : 0));
  mvaddnstr(1, 1, tui->tree_focus ? "Sources [Tab: logs]" : "Sources [Tab: focus]", width - 2);
  mvvline(1, width - 1, ACS_VLINE, height - 4);

  size_t rows = height > 5 ? (size_t)(height - 5) : 0;

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

    paint_text((int)i + 2, (int)indent, width - (int)indent - 2, label, 0, focused);
  }

  attrset(A_NORMAL);
}

static void render_footer(const Tui *tui, const Query *query, int height, int width) {
  attrset(COLOR_PAIR(TUI_MUTED));
  mvhline(height - 3, 0, ACS_HLINE, width);

  if (tui->selected_source) paint_text(height - 3, 1, width - 2, tui->selected_source, 0, false);
  mvaddnstr(height - 2, 1, (const char *)KEY_HINT_TEXT, width - 2);

  attrset(COLOR_PAIR(*query->error ? TUI_ERROR : TUI_NORMAL));
  mvprintw(height - 1, 0, "%c ", tui->editing ? '/' : '>');
  addnstr(tui->editing ? tui->input : query->text, width - 3);

  if (*query->error) {
    attrset(COLOR_PAIR(TUI_ERROR));
    mvaddnstr(height - 3, 1, query->error, width - 2);
  }
}

static void render_overlay_frame(const Tui *tui, int height, int width) {
  int box_height = height - 2, box_width = width - 4;

  attrset(A_NORMAL);
  for (int row = 1; row < height - 1; row++) mvhline(row, 2, ' ', box_width);

  mvhline(1, 2, ACS_HLINE, box_width);
  mvhline(height - 2, 2, ACS_HLINE, box_width);
  mvvline(1, 2, ACS_VLINE, box_height);
  mvvline(1, width - 3, ACS_VLINE, box_height);

  const char *title = tui->overlay == OVERLAY_HELP ? " Keys & queries " : " Log detail — j/k scroll, h/l pan, q close ";

  mvaddnstr(1, 4, title, box_width - 4);
}

static void render_overlay(const Tui *tui, int height, int width) {
  if (tui->overlay == OVERLAY_NONE) return;

  const char *text = tui->overlay == OVERLAY_HELP ? (const char *)HELP_TEXT : tui->detail;
  size_t line = 0;
  int row = 3;

  render_overlay_frame(tui, height, width);

  while (text && *text && row < height - 2) {
    const char *end = strchr(text, '\n');

    if (line++ >= tui->detail_scroll) {
      paint_text(row++, 4, width - 8, text, tui->detail_horizontal, false);
    }

    if (!end) break;

    text = end + 1;
  }
}

static void render_cursor(const Tui *tui, int height, int width) {
  curs_set(tui->editing ? 1 : 0);

  if (!tui->editing) return;

  size_t column = strlen(tui->input) + 2;

  move(height - 1, column < (size_t)width ? (int)column : width - 1);
}

void tui_render(Tui *tui, Search *search, const LogView *store, const Query *query, bool eof) {
  int height = LINES, width = COLS;

  erase();

  if (height < 5 || width < 24) {
    mvaddstr(0, 0, "Resize terminal (24x5)");
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

  render_cursor(tui, height, width);
  refresh();
}
