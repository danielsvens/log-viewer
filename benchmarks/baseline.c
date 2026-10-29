/* Compiled only against the original revision by baseline.py. */
#include "log_viewer/log_store.h"
#include "log_viewer/source.h"
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

typedef struct {
  LogStore store;
  Query query;
  size_t received;
} Baseline;

static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static bool consume(void *context, char *record) {
  Baseline *state = context;
  state->received++;
  log_store_append(&state->store, &state->query, record);
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  Baseline state = { 0 };
  LogSource source;
  if (!log_store_init(&state.store) || source_open(&source, argv[1]) != SOURCE_OPENED) return 1;
  double start = seconds();
  while (!source.eof)
    if (!source_poll(&source, false, consume, &state)) return 1;
  double indexing = seconds() - start;
  query_compile(&state.query, "RARE_MARKER");
  start = seconds();
  log_store_refilter(&state.store, &state.query);
  double query = seconds() - start;
  size_t rare = state.store.matches;
  query_compile(&state.query, "error AND timeout AND payment");
  log_store_refilter(&state.store, &state.query);
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  printf(
      "{\"received\":%zu,\"retained\":%zu,\"index_seconds\":%.6f,\"rare_query_seconds\":%.6f,\"rare_matches\":%zu,\"AND_matches\":%zu,\"peak_rss_kib\":%ld}\n",
      state.received,
      state.store.count,
      indexing,
      query,
      rare,
      state.store.matches,
      usage.ru_maxrss
  );
  source_close(&source);
  log_store_destroy(&state.store);
  query_destroy(&state.query);
  return 0;
}
