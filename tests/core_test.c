#include "log_viewer/json_support.h"
#include "log_viewer/log_store.h"
#include "log_viewer/query.h"
#include "log_viewer/source.h"
#include "log_viewer/text.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void test_store_selection_and_eviction(void) {
  LogStore store;
  Query query = { 0 };
  assert(log_store_init(&store));
  assert(query_compile(&query, "keep"));
  assert(log_store_selected(&store) == NULL);
  for (size_t i = 0; i < LOG_STORE_CAPACITY; i++) {
    char text[64];
    snprintf(text, sizeof(text), "%s %zu", i % 2 ? "keep" : "skip", i);
    char *owned = text_copy(text);
    assert(owned);
    log_store_append(&store, &query, owned);
  }
  assert(store.matches == LOG_STORE_CAPACITY / 2);
  log_store_move(&store, -5000);
  unsigned long selected_id = log_store_selected(&store)->id;
  log_store_append(&store, &query, text_copy("keep appended"));
  assert(store.count == LOG_STORE_CAPACITY - LOG_STORE_CAPACITY / 10 + 1);
  assert(!store.follow && log_store_selected(&store)->id == selected_id);

  assert(query_compile(&query, "keep 99999"));
  log_store_refilter(&store, &query);
  assert(store.matches == 1 && log_store_selected(&store)->id == LOG_STORE_CAPACITY);
  assert(query_compile(&query, ""));
  log_store_refilter(&store, &query);
  assert(log_store_selected(&store)->id == LOG_STORE_CAPACITY);

  log_store_first(&store);
  for (size_t i = 0; i < LOG_STORE_CAPACITY / 10; i++) log_store_append(&store, &query, text_copy("new"));
  assert(log_store_selected(&store)->id == LOG_STORE_CAPACITY / 5 + 1);
  log_store_set_follow(&store, true);
  assert(log_store_selected(&store)->id == store.sequence);
  log_store_destroy(&store);
  log_store_destroy(&store);
  query_destroy(&query);
}

typedef struct {
  char *records[4];
  size_t count;
} Capture;

static bool capture_record(void *context, char *record) {
  Capture *capture = context;
  assert(capture->count < 4);
  capture->records[capture->count++] = record;
  return true;
}

static void test_source_partial_record(void) {
  char path[] = "/tmp/log-viewer-source-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  const char text[] = "first\npartial";
  assert(write(fd, text, sizeof(text) - 1) == (ssize_t)(sizeof(text) - 1));
  close(fd);

  LogSource source;
  Capture capture = { 0 };
  assert(source_open(&source, path) == SOURCE_OPENED);
  assert(source_poll(&source, true, capture_record, &capture));
  assert(capture.count == 1 && !strcmp(capture.records[0], "first"));
  assert(source.pending_length == 7);
  assert(source_poll(&source, false, capture_record, &capture));
  assert(capture.count == 2 && !strcmp(capture.records[1], "partial"));
  assert(source.pending_length == 0 && source.eof);
  source_close(&source);
  source_close(&source);
  for (size_t i = 0; i < capture.count; i++) free(capture.records[i]);
  unlink(path);
}

int main(void) {
  test_independent_queries();
  test_record_transforms();
  test_store_selection_and_eviction();
  test_source_partial_record();
  puts("Core module, ownership, selection, eviction, and partial-record tests passed.");
  return 0;
}
