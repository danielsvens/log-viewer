#ifndef LOG_VIEWER_SOURCE_SET_H
#define LOG_VIEWER_SOURCE_SET_H

#include "log_viewer/source.h"
#include <time.h>

typedef struct {
  LogSource source;
  char *path;
} SourceEntry;

typedef struct {
  dev_t device;
  ino_t inode;
} SourceIdentity;

typedef struct {
  SourceEntry *entries;
  size_t count, capacity, cursor;
  char **directories;
  size_t directory_count, directory_capacity;
  SourceIdentity *seen;
  size_t seen_count, seen_capacity;
  char *buffer;
  struct timespec refreshed;
  bool eof, loading, initial, cycle_complete;
} SourceSet;

/* Initialize once; close even when opening fails, to release any earlier inputs. */
SourceOpenResult source_set_open(SourceSet *set, const char *const *paths, size_t count);
SourceOpenResult source_set_open_inputs(SourceSet *set, const InputSpec *inputs, size_t count, const char *tail);
void source_set_close(SourceSet *set);
bool source_set_poll(SourceSet *set, LogStore *store, bool follow, size_t *changed);

#endif
