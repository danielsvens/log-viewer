#ifndef LOG_VIEWER_SOURCE_SET_H
#define LOG_VIEWER_SOURCE_SET_H

#include "log_viewer/source.h"
#include <time.h>

typedef struct {
  char *name, *uid;
  size_t parent;
  size_t *members, count;
  const char *status;
} SourceGroup;

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
  struct KubeTarget *targets;
  size_t target_count;
  SourceGroup *groups;
  size_t group_count, revision;
  bool kube_pending, kube_failed;
} SourceSet;

/* Initialize once; close even when opening fails, to release any earlier inputs. */
SourceOpenResult source_set_open(SourceSet *set, const char *const *paths, size_t count);
SourceOpenResult source_set_open_inputs(SourceSet *set, const InputSpec *inputs, size_t count, const char *tail);
bool source_set_contains(const SourceSet *set, size_t scope, size_t source);
void source_set_close(SourceSet *set);
bool source_set_poll(SourceSet *set, LogStore *store, bool follow, size_t *changed);

#endif
