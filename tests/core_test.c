#include "log_viewer/json_support.h"
#include "log_viewer/log_store.h"
#include "log_viewer/query.h"
#include "log_viewer/log_level.h"
#include "log_viewer/source.h"
#include "log_viewer/search.h"
#include "log_viewer/source_tree.h"
#include "log_viewer/text.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void test_log_levels(void) {
  static const struct {
    const char *text;
    LogLevel level;
  } cases[] = { { "{\"level\":\"INFO\",\"message\":\"fatal error warning\"}", LOG_LEVEL_INFO },
                { "{\"severity\":\"warning\",\"message\":\"fine\"}", LOG_LEVEL_WARN },
                { "{\"Level\":\"Fatal\"}", LOG_LEVEL_ERROR },
                { "{\"level\":\"info\",\"severity\":\"error\"}", LOG_LEVEL_INFO },
                { "{\"log\":{\"level\":\"DEBUG\"}}", LOG_LEVEL_DEBUG },
                { "{\"log.level\":\"ERROR\"}", LOG_LEVEL_ERROR },
                { "{\"log_level\":\"trace\"}", LOG_LEVEL_DEBUG },
                { "{\"severityText\":\"Information\"}", LOG_LEVEL_INFO },
                { "{\"levelname\":\"CRITICAL\"}", LOG_LEVEL_ERROR },
                { "{\"@l\":\"Warning\"}", LOG_LEVEL_WARN },
                { "{\"PRIORITY\":\"3\"}", LOG_LEVEL_ERROR },
                { "{\"PRIORITY\":4}", LOG_LEVEL_WARN },
                { "{\"PRIORITY\":6}", LOG_LEVEL_INFO },
                { "{\"PRIORITY\":7}", LOG_LEVEL_DEBUG },
                { "{\"PRIORITY\":8}", LOG_LEVEL_UNKNOWN },
                { "{\"level\":30}", LOG_LEVEL_INFO },
                { "{\"level\":40}", LOG_LEVEL_WARN },
                { "{\"level\":60}", LOG_LEVEL_ERROR },
                { "{\"levelno\":20}", LOG_LEVEL_INFO },
                { "{\"levelno\":30}", LOG_LEVEL_WARN },
                { "{\"level_value\":40000}", LOG_LEVEL_ERROR },
                { "{\"severity_number\":9}", LOG_LEVEL_INFO },
                { "{\"severityNumber\":17}", LOG_LEVEL_ERROR },
                { "{\"severityNumber\":0}", LOG_LEVEL_UNKNOWN },
                { "{\"level\":\"custom\",\"message\":\"ERROR\"}", LOG_LEVEL_UNKNOWN },
                { "{\"message\":\"fatal error\"}", LOG_LEVEL_UNKNOWN },
                { "{\"level\":null}", LOG_LEVEL_UNKNOWN },
                { "[\"ERROR\"]", LOG_LEVEL_UNKNOWN },
                { "{bad json ERROR", LOG_LEVEL_UNKNOWN },
                { "[WARN] careful", LOG_LEVEL_WARN },
                { "debug: detail", LOG_LEVEL_DEBUG },
                { "2026-11-02 10:12:34.567 INFO ready", LOG_LEVEL_INFO },
                { "[2026-11-02 10:12:34] ERROR failed", LOG_LEVEL_ERROR },
                { "ts=2026-11-02 level=info msg=error", LOG_LEVEL_INFO },
                { "severity: WARN careful", LOG_LEVEL_WARN },
                { "message=\"level=error\"", LOG_LEVEL_UNKNOWN },
                { "plain message mentions error and warning", LOG_LEVEL_UNKNOWN },
                { "500 requests mention ERROR", LOG_LEVEL_UNKNOWN },
                { "[logger] request mentions ERROR", LOG_LEVEL_UNKNOWN },
                { "2026-11-02 10:12:34 request mentions ERROR", LOG_LEVEL_UNKNOWN },
                { "2026-11-02T10:12:34Z [main] INFO ready", LOG_LEVEL_INFO },
                { "errorish word", LOG_LEVEL_UNKNOWN },
                { "   ", LOG_LEVEL_UNKNOWN },
                { "", LOG_LEVEL_UNKNOWN } };

  JsonWorkspace workspace = { 0 };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    LogLevel level;

    assert(log_level_detect(cases[i].text, strlen(cases[i].text), &workspace, &level));
    assert(level == cases[i].level);
  }

  LogLevelMask mask;
  assert(log_levels_parse("error,warn", &mask) && mask == ((1u << LOG_LEVEL_ERROR) | (1u << LOG_LEVEL_WARN)));
  assert(log_levels_parse("info, debug ,unknown", &mask));
  assert(log_levels_parse("all", &mask) && mask == LOG_LEVELS_ALL);
  assert(log_levels_parse("none", &mask) && mask == 0);
  assert(!log_levels_parse("", &mask));
  assert(!log_levels_parse("error,", &mask));
  assert(!log_levels_parse("error,,warn", &mask));
  assert(!log_levels_parse("made-up", &mask));

  Query query = { 0 };
  QueryScratch scratch = { 0 };
  const char *record = "{\"level\":\"info\",\"message\":\"error 500\"}";

  assert(query_compile(&query, "json | message ~ \"error\" AND message ~ \"500\""));
  assert(query_match_filtered_span(&query, record, strlen(record), &scratch, 1u << LOG_LEVEL_INFO) == QUERY_MATCH);
  assert(query_match_filtered_span(&query, record, strlen(record), &scratch, 1u << LOG_LEVEL_ERROR) == QUERY_NO_MATCH);

  query_destroy(&query);
  query_scratch_destroy(&scratch);
  json_workspace_destroy(&workspace);
}

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

static void test_json_chains(void) {
  Query query = { 0 };
  QueryScratch scratch = { 0 };
  const char *expression = "json | message ~ \"error\" AND message ~ \"500\"";

  assert(query_compile(&query, expression));
  assert(query_matches(&query, "{\"message\":\"error 500\"}"));
  assert(!query_matches(&query, "{\"message\":\"error 404\"}"));
  assert(!query_matches(&query, "{\"message\":\"ERROR 500\"}"));

  assert(query_compile(&query, "json | (a==1 OR b==2) AND c==3"));
  assert(query_matches(&query, "{\"a\":1,\"c\":3}"));
  assert(!query_matches(&query, "{\"a\":1,\"c\":0}"));
  assert(query_compile(&query, "json | a==1 OR b==2 AND c==3"));
  assert(query_matches(&query, "{\"a\":1,\"c\":0}"));

  assert(query_compile(&query, "json | context ~ \"error\" AND context.message ~ \"500\""));
  const char *record = "{\"context\":{\"message\":\"error 500\"}}";

  for (size_t i = 0; i < 100; i++) {
    assert(query_match_span(&query, record, strlen(record), &scratch) == QUERY_MATCH);
    assert(query_match_span(&query, "invalid", 7, &scratch) == QUERY_NO_MATCH);
  }

  assert(query_compile(&query, query.text));
  assert(query_matches(&query, record));
  assert(!query_compile(&query, "json | context contains error"));
  assert(!query_matches(&query, record));

  query_scratch_destroy(&scratch);
  query_destroy(&query);
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
  assert(field.text == nullptr);

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

  Search *search = nullptr;

  assert(search_open(&search, nullptr, "error AND timeout AND payment", false) == SOURCE_OPENED);
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

  constexpr size_t lines = 200001;

  for (size_t i = 0; i < lines; i++) {
    bool rare = i == 0 || i == lines / 2 || i == lines - 1;

    fprintf(file, "{\"id\":%zu,\"http\":{\"status\":%d},\"message\":\"%s\"}\n", i, rare ? 503 : 200, rare ? "error timeout payment" : "normal payment");
  }

  fclose(file);

  Search *search = nullptr;

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

static void assert_record(LogStore *store, size_t id, const char *expected) {
  RecordLocation location;
  RecordReader reader = { 0 };
  const char *text;

  assert(log_store_location(store, id, &location));
  assert(location.length == strlen(expected));
  assert(record_reader_get(&reader, location, &text) == 1);
  assert(!memcmp(text, expected, strlen(expected)));

  record_reader_destroy(&reader);
}

static void test_interleaved_sources(void) {
  char first[] = "/tmp/log-viewer-first-XXXXXX";
  char second[] = "/tmp/log-viewer-second-XXXXXX";
  char empty[] = "/tmp/log-viewer-empty-XXXXXX";
  int a = mkstemp(first), b = mkstemp(second), c = mkstemp(empty);

  assert(a >= 0 && b >= 0 && c >= 0);
  assert(write(a, "alpha", 5) == 5);
  assert(write(b, "bravo\n", 6) == 6);

  LogSource sources[3];
  assert(source_open(&sources[0], first) == SOURCE_OPENED);
  assert(source_open(&sources[1], second) == SOURCE_OPENED);
  assert(source_open(&sources[2], empty) == SOURCE_OPENED);

  LogStore store;
  log_store_init(&store);

  size_t changed;

  for (size_t i = 0; i < 3; i++) {
    assert(source_poll(&sources[i], &store, true, &changed));
    assert(source_poll(&sources[i], &store, true, &changed));
  }

  assert(store.count == 2);
  assert_record(&store, 0, "alpha");
  assert_record(&store, 1, "bravo");

  assert(write(a, "-next\nA2", 8) == 8);
  assert(source_poll(&sources[0], &store, true, &changed) && changed == 0);
  assert(source_poll(&sources[0], &store, true, &changed));
  assert(write(b, "B2\n", 3) == 3);
  assert(source_poll(&sources[1], &store, true, &changed));
  assert(store.count == 4);

  assert_record(&store, 0, "alpha-next");
  assert_record(&store, 1, "bravo");
  assert_record(&store, 2, "A2");
  assert_record(&store, 3, "B2");

  assert(ftruncate(a, 0) == 0 && pwrite(a, "new\n", 4, 0) == 4);
  assert(source_poll(&sources[0], &store, true, &changed) && changed == 0);
  assert(store.unavailable == 2 && store.count == 5);

  RecordLocation location;
  assert(log_store_location(&store, 0, &location) && !location.available);
  assert(log_store_location(&store, 2, &location) && !location.available);
  assert_record(&store, 1, "bravo");
  assert_record(&store, 3, "B2");
  assert_record(&store, 4, "new");
  assert(log_store_location(&store, 4, &location) && !strcmp(location.name, first));

  for (size_t i = 0; i < 3; i++) source_close(&sources[i]);

  log_store_destroy(&store);
  close(a);
  close(b);
  close(c);
  unlink(first);
  unlink(second);
  unlink(empty);
}

static SearchStatus wait_for_counts(Search *search, size_t count, size_t matches) {
  for (size_t i = 0; i < 5000; i++) {
    SearchStatus status = search_status(search);

    assert(!status.error);
    if (status.eof && !status.searching && status.count == count && status.matches == matches) return status;

    usleep(1000);
  }

  assert(!"multiple-source follow timed out");

  return (SearchStatus){ 0 };
}

static void test_multiple_source_search(void) {
  char first[] = "/tmp/log-viewer-follow-a-XXXXXX";
  char second[] = "/tmp/log-viewer-follow-b-XXXXXX";
  int a = mkstemp(first), b = mkstemp(second);

  assert(a >= 0 && b >= 0);
  assert(write(a, "alpha\n", 6) == 6 && write(b, "bravo\n", 6) == 6);

  const char *paths[] = { first, second };
  Search *search = nullptr;

  assert(search_open_paths(&search, paths, 2, "", true) == SOURCE_OPENED);
  wait_for_counts(search, 2, 2);

  assert(search_source_count(search) == 2);
  assert(!strcmp(search_source_name(search, 0), first));
  assert(search_source_name(search, 2) == nullptr);
  assert(search_selected_source(search) == SEARCH_ALL_SOURCES);

  SourceTree tree = { 0 };
  assert(source_tree_sync(&tree, search));
  assert(tree.count == 3 && tree.rows[0].source == SEARCH_ALL_SOURCES);
  assert(tree.rows[1].source == 0 || tree.rows[2].source == 0);
  source_tree_destroy(&tree);

  search_select_source(search, 0);
  wait_for_counts(search, 2, 1);
  assert(search_selected_source(search) == 0);

  search_request(search, "bravo");
  wait_for_counts(search, 2, 0);
  search_select_source(search, 1);
  wait_for_counts(search, 2, 1);
  search_select_source(search, 999);
  assert(search_selected_source(search) == 1);

  for (size_t i = 0; i < 40; i++) search_select_source(search, i % 2);

  search_select_source(search, SEARCH_ALL_SOURCES);
  search_request(search, "");
  wait_for_counts(search, 2, 2);

  assert(write(a, "tail", 4) == 4);
  wait_for_counts(search, 3, 3);
  assert(write(b, "beta\n", 5) == 5);
  wait_for_counts(search, 4, 4);

  search_request(search, "tail-done");
  wait_for_counts(search, 4, 0);
  assert(write(a, "-done\n", 6) == 6);
  wait_for_counts(search, 4, 1);

  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  LogRecord record;

  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1);
  assert(record.id == 3 && !strcmp(record.text, "tail-done") && !strcmp(record.source, first));

  search_request(search, "~bravo|beta");
  wait_for_counts(search, 4, 2);
  assert(ftruncate(a, 0) == 0 && pwrite(a, "new\n", 4, 0) == 4);
  wait_for_counts(search, 5, 2);
  assert(search_status(search).unavailable == 2);

  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1);
  assert(record.id == 2 && !strcmp(record.text, "bravo") && !strcmp(record.source, second));

  search_select_source(search, 0);
  search_request(search, "new");
  wait_for_counts(search, 5, 1);
  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1);
  assert(!strcmp(record.text, "new"));

  search_close(search);
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);
  close(a);
  close(b);
  unlink(first);
  unlink(second);
}

static void write_log_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");

  assert(file);

  assert(fputs(text, file) >= 0);
  assert(fclose(file) == 0);
}

static void test_directory_discovery_and_rotation(void) {
  char directory[] = "/tmp/log-viewer-discovery-XXXXXX";

  assert(mkdtemp(directory));

  Search *search = nullptr;

  assert(search_open(&search, directory, "error", true) == SOURCE_OPENED);
  wait_for_counts(search, 0, 0);

  char nested[256], active[256], rotated[256], second[256];
  snprintf(nested, sizeof(nested), "%s/nested", directory);
  snprintf(active, sizeof(active), "%s/nested/app.log", directory);
  snprintf(rotated, sizeof(rotated), "%s/nested/app.log.1", directory);
  snprintf(second, sizeof(second), "%s/second.log", directory);

  assert(mkdir(nested, 0700) == 0);
  write_log_file(active, "error partial");
  wait_for_counts(search, 1, 1);

  search_request(search, "partial-done");
  wait_for_counts(search, 1, 0);

  FILE *file = fopen(active, "a");

  assert(file);
  assert(fputs("-done\n", file) >= 0 && fclose(file) == 0);
  wait_for_counts(search, 1, 1);

  search_request(search, "error");
  wait_for_counts(search, 1, 1);
  assert(rename(active, rotated) == 0);
  write_log_file(active, "info next\nerror second\n");
  wait_for_counts(search, 3, 2);

  /* Finding another file requires a rescan that must not reimport the rotated inode. */
  write_log_file(second, "error third\n");
  wait_for_counts(search, 4, 3);
  assert(search_source_count(search) == 2);

  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  LogRecord record;

  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1);
  assert(!strcmp(record.text, "error partial-done"));
  assert(search_read_result(search, 1, &reader, &buffer, &record, 0) == 1);
  assert(!strcmp(record.text, "error second"));

  search_select_source(search, 1);
  wait_for_counts(search, 4, 1);
  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1);
  assert(!strcmp(record.text, "error third"));

  search_close(search);
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);

  unlink(active);
  unlink(rotated);
  unlink(second);
  rmdir(nested);
  rmdir(directory);
}

static void test_level_filter_history_and_sources(void) {
  char first[] = "/tmp/log-viewer-level-a-XXXXXX";
  char second[] = "/tmp/log-viewer-level-b-XXXXXX";
  int a = mkstemp(first), b = mkstemp(second);

  assert(a >= 0 && b >= 0);
  close(a);
  close(b);

  write_log_file(
      first,
      "{\"level\":\"error\",\"message\":\"needle alpha\"}\n"
      "{\"level\":\"info\",\"message\":\"error needle alpha\"}\n"
  );
  write_log_file(
      second,
      "{\"severity\":\"warn\",\"message\":\"needle beta\"}\n"
      "{\"level\":\"debug\",\"message\":\"other beta\"}\n"
      "{\"message\":\"unknown needle beta\"}\n"
  );

  const char *paths[] = { first, second };
  Search *search = nullptr;

  assert(search_open_paths(&search, paths, 2, "", true) == SOURCE_OPENED);
  wait_for_counts(search, 5, 5);
  assert(search_levels(search) == LOG_LEVELS_ALL);

  search_set_levels(search, 1u << LOG_LEVEL_ERROR);
  wait_for_counts(search, 5, 1);
  search_request(search, "needle");
  wait_for_counts(search, 5, 1);
  search_select_source(search, 1);
  wait_for_counts(search, 5, 0);
  search_set_levels(search, 1u << LOG_LEVEL_WARN);
  wait_for_counts(search, 5, 1);

  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  LogRecord record;

  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1 && record.id == 3);

  FILE *file = fopen(second, "a");

  assert(file);
  assert(fputs("{\"level\":\"error\",\"message\":\"needle appended\"}\n", file) >= 0 && fclose(file) == 0);
  wait_for_counts(search, 6, 1);
  search_set_levels(search, 1u << LOG_LEVEL_ERROR);
  wait_for_counts(search, 6, 1);
  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1 && record.id == 6);

  search_set_levels(search, LOG_LEVELS_ALL);
  wait_for_counts(search, 6, 3);
  search_select_source(search, SEARCH_ALL_SOURCES);
  wait_for_counts(search, 6, 5);
  search_request(search, "");
  wait_for_counts(search, 6, 6);

  for (size_t i = 0; i < 40; i++) search_set_levels(search, 1u << (i % LOG_LEVEL_COUNT));

  search_set_levels(search, 1u << LOG_LEVEL_ERROR);
  wait_for_counts(search, 6, 2);
  search_set_levels(search, 0);
  wait_for_counts(search, 6, 0);

  file = fopen(second, "a");

  assert(file);
  assert(fputs("ERROR new while hidden\n", file) >= 0 && fclose(file) == 0);
  wait_for_counts(search, 7, 0);
  search_set_levels(search, LOG_LEVELS_ALL);
  wait_for_counts(search, 7, 7);
  search_set_levels(search, 1u << LOG_LEVEL_UNKNOWN);
  wait_for_counts(search, 7, 1);
  assert(search_read_result(search, 0, &reader, &buffer, &record, 0) == 1 && record.id == 5);

  search_close(search);
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);
  unlink(first);
  unlink(second);
}

int main(void) {
  test_level_filter_history_and_sources();
  test_log_levels();
  test_directory_discovery_and_rotation();
  test_multiple_source_search();
  test_interleaved_sources();
  test_64_bit_offsets();
  test_independent_queries();
  test_json_chains();
  test_record_transforms();
  test_file_index_and_partial_record();
  test_history_and_superseding_search();

  puts("Core indexing, complete history, AND/JSON search, cancellation, and truncation tests passed.");

  return 0;
}
