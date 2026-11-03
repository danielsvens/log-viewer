#include "log_viewer/search.h"
#include "log_viewer/source_set.h"
#include "log_viewer/source_tree.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void wait_matches(Search *search, size_t expected) {
  for (int i = 0; i < 1500; i++) {
    SearchStatus status = search_status(search);
    assert(!status.error);
    if (!status.searching && status.matches == expected) return;
    usleep(10000);
  }
  fprintf(stderr, "Expected %zu matches; found %zu\n", expected, search_status(search).matches);
  abort();
}

int main(void) {
  const InputSpec input = { .kind = INPUT_KUBE_DEPLOYMENT, .value = "api", .kube_tail = "1000" };
  Search *search = nullptr;
  assert(search_open_inputs(&search, &input, 1, "1000", "", true) == SOURCE_OPENED);
  wait_matches(search, 2);

  SourceTree tree = { 0 };
  assert(source_tree_sync(&tree, search));
  assert(tree.count == 5);
  assert(tree.rows[1].source == SOURCE_GROUP_BASE);
  assert(tree.rows[2].source == SOURCE_GROUP_BASE + 1);
  assert(!tree.rows[1].directory && !tree.rows[2].directory);
  search_select_source(search, SOURCE_GROUP_BASE);
  wait_matches(search, 2);

  search_configure(search, "history", SOURCE_GROUP_BASE + 1, LOG_LEVELS_ALL);
  wait_matches(search, 2);
  search_configure(search, "absent", SOURCE_GROUP_BASE, LOG_LEVELS_ALL);
  wait_matches(search, 0);
  search_configure(search, "history", SOURCE_GROUP_BASE, LOG_LEVELS_ALL);
  wait_matches(search, 2);

  char next[1024], state[1024];
  snprintf(next, sizeof(next), "%s/scaled", getenv("KUBE_FIXTURE"));
  snprintf(state, sizeof(state), "%s/state", getenv("KUBE_FIXTURE"));
  assert(rename(next, state) == 0);
  wait_matches(search, 4);
  assert(source_tree_sync(&tree, search));
  assert(tree.count == 8);

  search_configure(search, "history", SOURCE_GROUP_BASE + 1, LOG_LEVELS_ALL);
  wait_matches(search, 2);
  search_configure(search, "history", SOURCE_GROUP_BASE, LOG_LEVELS_ALL);
  wait_matches(search, 4);
  search_select_source(search, 0);
  wait_matches(search, 1);
  search_select_source(search, SIZE_MAX);
  wait_matches(search, 4);

  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  char *original[4];
  LogRecord record;
  for (size_t i = 0; i < 4; i++) {
    assert(search_read_result(search, i, &reader, &buffer, &record, 0) == 1);
    assert(record.id == i + 1);
    original[i] = text_copy(record.text);
    assert(original[i]);
  }

  search_configure(search, "", SOURCE_GROUP_BASE, LOG_LEVELS_ALL);
  snprintf(next, sizeof(next), "%s/restarted", getenv("KUBE_FIXTURE"));
  assert(rename(next, state) == 0);
  wait_matches(search, 8);
  for (size_t i = 0; i < 4; i++) {
    assert(search_read_result(search, i, &reader, &buffer, &record, 0) == 1);
    assert(record.id == i + 1 && !strcmp(record.text, original[i]));
    free(original[i]);
  }
  search_request(search, "partial");
  wait_matches(search, 2);
  for (size_t i = 0; i < 2; i++) {
    assert(search_read_result(search, i, &reader, &buffer, &record, 0) == 1);
    assert(!strcmp(record.text, "partial"));
  }
  search_request(search, "");
  wait_matches(search, 8);
  snprintf(next, sizeof(next), "%s/removed", getenv("KUBE_FIXTURE"));
  assert(rename(next, state) == 0);
  search_reconnect(search);
  usleep(500000);
  wait_matches(search, 8);
  assert(source_tree_sync(&tree, search) && tree.count == 8);
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);

  source_tree_destroy(&tree);
  search_close(search);
  puts("Kubernetes group selection, tree updates, scope restoration, and search cache passed.");
}
