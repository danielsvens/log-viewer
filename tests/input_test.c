/* Exercise editor keys without a terminal or screen timing assumptions. */
#include "log_viewer/tui.h"
#include "log_viewer/text.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

  tui.overlay = OVERLAY_DETAIL;
  tui.detail = text_copy("{\n  \"first\": \"Error\",\n  \"second\": \"error\"\n}");
  assert(tui.detail);
  tui.detail_match = SIZE_MAX;
  key(&tui, search, &view, &query, '/');
  type_text(&tui, search, &view, &query, "error");
  key(&tui, search, &view, &query, '\n');
  assert(tui.overlay == OVERLAY_DETAIL && !tui.detail_searching && tui.detail_scroll == 1);
  size_t first_match = tui.detail_match;
  key(&tui, search, &view, &query, 'n');
  assert(tui.detail_scroll == 2 && tui.detail_match > first_match);
  key(&tui, search, &view, &query, 'n');
  assert(tui.detail_match == first_match);
  key(&tui, search, &view, &query, 'N');
  assert(tui.detail_scroll == 2);

  key(&tui, search, &view, &query, '/');
  key(&tui, search, &view, &query, 21);
  type_text(&tui, search, &view, &query, "missing");
  key(&tui, search, &view, &query, 27);
  assert(tui.overlay == OVERLAY_DETAIL && !strcmp(tui.detail_query, "error"));
  key(&tui, search, &view, &query, '/');
  key(&tui, search, &view, &query, 21);
  type_text(&tui, search, &view, &query, "missing");
  key(&tui, search, &view, &query, '\n');
  assert(tui.detail_match == SIZE_MAX && !strcmp(tui.detail_status, "No matches"));
  assert(!strcmp(query.text, "~503"));
  key(&tui, search, &view, &query, 'q');

  tui.overlay = OVERLAY_DETAIL;
  tui.detail = text_copy("{\"x\":1}");
  assert(tui.detail);
  tui.tty = tmpfile();
  assert(tui.tty);
  const char *original_path = getenv("PATH");
  char *saved_path = original_path ? text_copy(original_path) : nullptr;
  assert(!original_path || saved_path);
  char directory[] = "/tmp/log-viewer-clipboard-XXXXXX";
  assert(mkdtemp(directory));
  char command_path[256], copied_path[256];
#ifdef __APPLE__
  snprintf(command_path, sizeof(command_path), "%s/pbcopy", directory);
#else
  snprintf(command_path, sizeof(command_path), "%s/wl-copy", directory);
#endif
  snprintf(copied_path, sizeof(copied_path), "%s/copied", directory);
  FILE *command = fopen(command_path, "w");
  assert(command);
  assert(fprintf(command, "#!/bin/sh\n/bin/cat > '%s'\n", copied_path) > 0);
  assert(fclose(command) == 0 && chmod(command_path, 0700) == 0);
  assert(setenv("PATH", directory, 1) == 0);
  key(&tui, search, &view, &query, 'y');
  assert(!strcmp(tui.detail_status, "Copied entire record to clipboard"));
  FILE *copied = fopen(copied_path, "r");
  assert(copied);
  char native_clipboard[64] = { 0 };
  assert(fread(native_clipboard, 1, sizeof(native_clipboard) - 1, copied) == strlen(tui.detail));
  assert(!strcmp(native_clipboard, tui.detail));
  assert(fclose(copied) == 0);
  assert(unlink(command_path) == 0 && unlink(copied_path) == 0 && rmdir(directory) == 0);

  assert(setenv("PATH", "/nonexistent", 1) == 0);
  key(&tui, search, &view, &query, 'y');
  assert(strstr(tui.detail_status, "OSC 52"));
  rewind(tui.tty);
  char clipboard[64] = { 0 };
  assert(fread(clipboard, 1, sizeof(clipboard) - 1, tui.tty) == 20);
  assert(!strcmp(clipboard, "\033]52;c;eyJ4IjoxfQ==\a"));
  fclose(tui.tty);
  tui.tty = nullptr;
  assert((saved_path ? setenv("PATH", saved_path, 1) : unsetenv("PATH")) == 0);
  free(saved_path);
  key(&tui, search, &view, &query, 'q');
  assert(tui.running && tui.overlay == OVERLAY_NONE);
  key(&tui, search, &view, &query, 'q');
  assert(tui.running && tui.overlay == OVERLAY_QUIT);
  key(&tui, search, &view, &query, 'q');
  assert(tui.running && tui.overlay == OVERLAY_NONE);

  const int cancel_keys[] = { 27, 'n', 'N', '\n', KEY_ENTER };

  for (size_t i = 0; i < sizeof(cancel_keys) / sizeof(*cancel_keys); i++) {
    key(&tui, search, &view, &query, 'q');
    key(&tui, search, &view, &query, cancel_keys[i]);
    assert(tui.running && tui.overlay == OVERLAY_NONE);
  }

  tui.tree_focus = true;
  key(&tui, search, &view, &query, 'q');
  assert(tui.running && tui.overlay == OVERLAY_QUIT);
  key(&tui, search, &view, &query, 'y');
  assert(!tui.running && tui.overlay == OVERLAY_NONE);

  query_destroy(&query);
  search_close(search);
  unlink(path);
  puts("Deferred filter submission, caret editing, UTF-8, invalid drafts, input bounds, inspector search, clipboard, and quit confirmation passed.");

  return 0;
}
