/* Exercise editor keys without a terminal or screen timing assumptions. */
#include "log_viewer/tui.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void key(Tui *tui, Search *search, LogView *view, Query *query, int value) { assert(tui_handle_key(tui, search, view, query, value)); }

static void type_text(Tui *tui, Search *search, LogView *view, Query *query, const char *text) {
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) key(tui, search, view, query, *p);
}

static void wait_matches(Search *search, size_t expected) {
  for (size_t i = 0; i < 5000; i++) {
    SearchStatus status = search_status(search);

    assert(!status.error);
    if (status.eof && !status.searching && status.matches == expected) return;

    usleep(1000);
  }

  assert(!"editor search timed out");
}

int main(void) {
  char path[] = "/tmp/log-viewer-editor-XXXXXX";
  int fd = mkstemp(path);

  assert(fd >= 0);

  FILE *file = fdopen(fd, "w");

  assert(file);
  assert(fputs("{\"status\":503}\n{\"status\":201}\n", file) >= 0 && fclose(file) == 0);

  Search *search = nullptr;
  Query query = { 0 };
  Tui tui = { .running = true, .top = 7 };
  LogView view = { .follow = true };

  assert(query_compile(&query, "json | status >= 500"));
  assert(search_open(&search, path, query.text, false) == SOURCE_OPENED);
  wait_matches(search, 1);

  uint64_t revision = search_status(search).revision;
  key(&tui, search, &view, &query, '/');
  assert(tui.editing && tui.input_cursor == strlen(tui.input));
  key(&tui, search, &view, &query, KEY_LEFT);
  key(&tui, search, &view, &query, KEY_LEFT);
  key(&tui, search, &view, &query, KEY_LEFT);
  key(&tui, search, &view, &query, KEY_DC);
  key(&tui, search, &view, &query, '2');
  assert(!strcmp(tui.input, "json | status >= 200"));

  key(&tui, search, &view, &query, KEY_END);
  key(&tui, search, &view, &query, KEY_BACKSPACE);
  key(&tui, search, &view, &query, '0');
  assert(!strcmp(tui.input, "json | status >= 200"));
  assert(!strcmp(query.text, "json | status >= 500"));
  assert(search_status(search).revision == revision && search_status(search).matches == 1);
  assert(search_levels(search) == LOG_LEVELS_ALL && tui.top == 7);

  key(&tui, search, &view, &query, '\n');
  assert(!tui.editing && !strcmp(query.text, "json | status >= 200") && tui.top == 0);
  wait_matches(search, 2);
  revision = search_status(search).revision;

  key(&tui, search, &view, &query, '/');
  key(&tui, search, &view, &query, 21);
  type_text(&tui, search, &view, &query, "json | status >=");
  key(&tui, search, &view, &query, '\n');
  assert(tui.editing && *tui.input_error);
  assert(!strcmp(query.text, "json | status >= 200"));
  assert(search_status(search).revision == revision && search_status(search).matches == 2);
  key(&tui, search, &view, &query, ' ');
  assert(!*tui.input_error);
  key(&tui, search, &view, &query, 27);
  assert(!tui.editing && search_status(search).revision == revision);

  key(&tui, search, &view, &query, '/');
  key(&tui, search, &view, &query, 21);
  type_text(&tui, search, &view, &query, "abé中z");
  key(&tui, search, &view, &query, KEY_LEFT);
  key(&tui, search, &view, &query, KEY_BACKSPACE);
  assert(!strcmp(tui.input, "abéz"));
  key(&tui, search, &view, &query, KEY_LEFT);
  key(&tui, search, &view, &query, KEY_DC);
  assert(!strcmp(tui.input, "abz"));
  type_text(&tui, search, &view, &query, "ö");
  assert(!strcmp(tui.input, "aböz"));
  key(&tui, search, &view, &query, KEY_HOME);
  key(&tui, search, &view, &query, KEY_BACKSPACE);
  assert(!strcmp(tui.input, "aböz") && tui.input_cursor == 0);
  key(&tui, search, &view, &query, KEY_END);
  key(&tui, search, &view, &query, KEY_DC);
  key(&tui, search, &view, &query, KEY_LEFT);
  key(&tui, search, &view, &query, 11);
  assert(!strcmp(tui.input, "abö"));
  key(&tui, search, &view, &query, 27);

  key(&tui, search, &view, &query, '/');
  key(&tui, search, &view, &query, 21);

  for (size_t i = 0; i < QUERY_CAPACITY - 1; i++) key(&tui, search, &view, &query, 'x');

  key(&tui, search, &view, &query, 'y');
  assert(strlen(tui.input) == QUERY_CAPACITY - 1);
  key(&tui, search, &view, &query, KEY_HOME);
  key(&tui, search, &view, &query, 'z');
  assert(tui.input[0] == 'x');
  key(&tui, search, &view, &query, KEY_DC);
  key(&tui, search, &view, &query, 'z');
  assert(tui.input[0] == 'z' && strlen(tui.input) == QUERY_CAPACITY - 1);
  key(&tui, search, &view, &query, 27);
  assert(search_status(search).revision == revision);

  key(&tui, search, &view, &query, '/');
  key(&tui, search, &view, &query, 21);
  type_text(&tui, search, &view, &query, "~[");
  key(&tui, search, &view, &query, '\n');
  assert(tui.editing && *tui.input_error && !query.compiled);
  key(&tui, search, &view, &query, 21);
  type_text(&tui, search, &view, &query, "~503");
  key(&tui, search, &view, &query, '\n');
  assert(!tui.editing && query.compiled);
  wait_matches(search, 1);

  query_destroy(&query);
  search_close(search);
  unlink(path);
  puts("Deferred filter submission, caret editing, UTF-8, invalid drafts, and input bounds passed.");

  return 0;
}
