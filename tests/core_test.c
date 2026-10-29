#include "log_viewer/json_support.h"
#include "log_viewer/log_store.h"
#include "log_viewer/query.h"
#include "log_viewer/source.h"
#include "log_viewer/search.h"
#include "log_viewer/text.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void test_independent_queries(void) {
  Query first = { 0 }, second = { 0 };
  assert(query_compile(&first, "~error|warn"));
  assert(query_compile(&second, "json | http.status >= 500"));
  assert(query_matches(&first, "WARNING"));
  assert(!query_matches(&first, "info"));
  assert(query_matches(&second, "{\"http\":{\"status\":503}}"));
  assert(!query_matches(&second, "{\"http\":{\"status\":\"503\"}}"));
  assert(query_compile(&first, first.text));
  assert(query_matches(&first, "error"));
  assert(!query_compile(&first, "~["));
  assert(!query_matches(&first, "error"));
  assert(query_matches(&second, "{\"http\":{\"status\":500}}"));
  assert(query_compile(&first, ""));
  assert(query_matches(&first, "anything"));
  query_destroy(&first);
  query_destroy(&second);
  query_destroy(&second);
}

static void test_record_transforms(void) {
  const char raw[] = "\033[31merror\033[0m\tmessage\r\001";
  char *clean = text_sanitize(raw, sizeof(raw) - 1);
  assert(clean && !strcmp(clean, "error message"));
  free(clean);

  JsonField field;
  assert(json_read_field("{\"context\":{\"message\":\"\\u0065rror\"}}", "context.message", &field));
  assert(!field.number && !strcmp(field.text, "error"));
  free(field.text);
  assert(!json_read_field("{\"context\":null}", "context.message", &field));
  assert(field.text == NULL);

  char *formatted = json_format_message("{\"status\":503,\"nested\":{\"ok\":false}}");
  assert(formatted && strstr(formatted, "\n  \"status\" : 503"));
  assert(strstr(formatted, "\n    \"ok\" : false"));
  free(formatted);
  formatted = json_format_message("plain message");
  assert(formatted && !strcmp(formatted, "plain message"));
  free(formatted);
}

static void test_file_index_and_partial_record(void) {
  char path[] = "/tmp/log-viewer-index-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  const char raw[] = "first\n\npartial";
  assert(write(fd, raw, sizeof(raw) - 1) == (ssize_t)(sizeof(raw) - 1));
  close(fd);
  LogStore store;
  log_store_init(&store);
  LogSource source;
  assert(source_open(&source, path) == SOURCE_OPENED);
  size_t changed;
  assert(source_poll(&source, &store, true, &changed));
  assert(source_poll(&source, &store, true, &changed));
  assert(source.eof && store.count == 3);
  RecordReader reader = { 0 };
  RecordLocation location;
  const char *text;
  assert(log_store_location(&store, 2, &location));
  assert(record_reader_get(&reader, location, &text) == 1 && location.length == 7 && !memcmp(text, "partial", 7));
  fd = open(path, O_WRONLY | O_APPEND);
  assert(fd >= 0);
  assert(write(fd, "-continued\nlast", 15) == 15);
  close(fd);
  assert(source_poll(&source, &store, true, &changed));
  assert(changed == 2);
  assert(source_poll(&source, &store, true, &changed));
  assert(store.count == 4);
  assert(log_store_location(&store, 2, &location));
  assert(record_reader_get(&reader, location, &text) == 1 && location.length == 17 && !memcmp(text, "partial-continued", 17));
  /* External truncation must never dereference a mapping or return stale text as a new line. */
  fd = open(path, O_WRONLY | O_TRUNC);
  assert(fd >= 0);
  assert(write(fd, "new\n", 4) == 4);
  close(fd);
  assert(source_poll(&source, &store, true, &changed));
  assert(changed == 0 && store.unavailable == 4);
  assert(log_store_location(&store, 2, &location) && !location.available);
  assert(record_reader_get(&reader, location, &text) == 0);
  assert(log_store_location(&store, 4, &location));
  assert(record_reader_get(&reader, location, &text) == 1 && !memcmp(text, "new", 3));
  record_reader_destroy(&reader);
  source_close(&source);
  source_close(&source);
  log_store_destroy(&store);
  log_store_destroy(&store);
  unlink(path);
}

static SearchStatus completed(Search *search) {
  for (int i = 0; i < 5000; i++) {
    SearchStatus status = search_status(search);
    assert(!status.error);
    if (status.eof && !status.searching) return status;
    usleep(1000);
  }
  assert(!"search timed out");
  return (SearchStatus){ 0 };
}

static void test_stream_history(const char *path, size_t lines) {
  int input = open(path, O_RDONLY), descriptors[2], saved_stdin = dup(STDIN_FILENO);
  assert(input >= 0 && saved_stdin >= 0 && pipe(descriptors) == 0);
  pid_t writer = fork();
  assert(writer >= 0);
  if (writer == 0) {
    close(descriptors[0]);
    close(saved_stdin);
    char bytes[65536];
    ssize_t length;
    while ((length = read(input, bytes, sizeof(bytes))) > 0) {
      ssize_t written = 0;
      while (written < length) {
        ssize_t n = write(descriptors[1], bytes + written, (size_t)(length - written));
        if (n <= 0) _exit(1);
        written += n;
      }
    }
    close(input);
    close(descriptors[1]);
    _exit(length < 0 ? 1 : 0);
  }
  close(input);
  close(descriptors[1]);
  assert(dup2(descriptors[0], STDIN_FILENO) >= 0);
  close(descriptors[0]);
  Search *search = NULL;
  assert(search_open(&search, NULL, "error AND timeout AND payment", false) == SOURCE_OPENED);
  SearchStatus status = completed(search);
  assert(status.count == lines && status.matches == 3);
  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  LogRecord record;
  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1 && record.id == 1);
  assert(search_read_result(search, 2, &reader, &buffer, &record, 0) == 1 && record.id == lines);
  search_close(search);
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);
  int child_status;
  assert(waitpid(writer, &child_status, 0) == writer && child_status == 0);
  assert(dup2(saved_stdin, STDIN_FILENO) >= 0);
  close(saved_stdin);
}

static void test_history_and_superseding_search(void) {
  char path[] = "/tmp/log-viewer-search-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  FILE *file = fdopen(fd, "w");
  assert(file);
  const size_t lines = 200001;
  for (size_t i = 0; i < lines; i++) {
    bool rare = i == 0 || i == lines / 2 || i == lines - 1;
    fprintf(file, "{\"id\":%zu,\"http\":{\"status\":%d},\"message\":\"%s\"}\n", i, rare ? 503 : 200, rare ? "error timeout payment" : "normal payment");
  }
  fclose(file);
  Search *search = NULL;
  assert(search_open(&search, path, "error AND timeout AND payment", false) == SOURCE_OPENED);
  SearchStatus status = completed(search);
  assert(status.count == lines && status.matches == 3);
  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  LogRecord record;
  for (size_t i = 0; i < 3; i++) {
    assert(search_read_result(search, i, &reader, &buffer, &record, 0) == 1);
    assert(record.id == (i == 0 ? 1 : i == 1 ? lines / 2 + 1 : lines));
  }
  for (int i = 0; i < 50; i++) search_request(search, i % 2 ? "~payment" : "json | http.status >= 500");
  search_request(search, "error AND timeout AND payment");
  status = completed(search);
  assert(status.matches == 3 && status.count == lines);
  LogView view = { .follow = true };
  search_sync_view(search, &view);
  assert(view.selected == 2);
  log_view_set_follow(search, &view, false);
  search_sync_view(search, &view);
  assert(view.selected == 2);
  log_view_move(search, &view, -1);
  search_sync_view(search, &view);
  assert(view.selected == 1);
  assert(search_read_result(search, view.selected, &reader, &buffer, &record, 0) == 1 && record.id == lines / 2 + 1);
  search_request(search, "json | http.status >= 500");
  status = completed(search);
  assert(status.matches == 3);
  search_request(search, "");
  status = completed(search);
  assert(status.matches == lines);
  assert(search_read_result(search, lines - 1, &reader, &buffer, &record, 0) == 1 && record.id == lines);
  search_request(search, "~payment"); /* Exercise shutdown with work in flight. */
  search_close(search);
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);
  test_stream_history(path, lines);
  unlink(path);
}

static void test_64_bit_offsets(void) {
  char path[] = "/tmp/log-viewer-offset-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  uint64_t offset = (uint64_t)UINT32_MAX + 8192;
  assert(pwrite(fd, "far", 3, (off_t)offset) == 3);
  LogStore store;
  log_store_init(&store);
  size_t segment;
  assert(log_store_add_file(&store, fd, &segment));
  assert(log_store_append(&store, offset, 3));
  RecordLocation location;
  RecordReader reader = { 0 };
  const char *text;
  assert(log_store_location(&store, 0, &location));
  assert(record_reader_get(&reader, location, &text) == 1 && !memcmp(text, "far", 3));
  close(fd);
  unlink(path);
  record_reader_destroy(&reader);
  log_store_destroy(&store);
}

int main(void) {
  test_64_bit_offsets();
  test_independent_queries();
  test_record_transforms();
  test_file_index_and_partial_record();
  test_history_and_superseding_search();
  puts("Core indexing, complete history, AND/JSON search, cancellation, and truncation tests passed.");
  return 0;
}
