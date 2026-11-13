#include "log_viewer/core/source_set.h"
#include "log_viewer/core/text.h"
#include "log_viewer/core/kube.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *grow_array(void *array, size_t *capacity, size_t count, size_t element_size) {
  if (count < *capacity) return array;

  if (*capacity > SIZE_MAX / 2) {
    errno = ENOMEM;
    return nullptr;
  }

  size_t next = *capacity ? *capacity * 2 : 8;

  if (next > SIZE_MAX / element_size) {
    errno = ENOMEM;
    return nullptr;
  }

  void *allocation = realloc(array, next * element_size);

  if (!allocation) {
    errno = ENOMEM;
    return nullptr;
  }

  *capacity = next;

  return allocation;
}

static bool remember_identity(SourceSet *set, const struct stat *identity) {
  for (size_t i = 0; i < set->seen_count; i++)
    if (set->seen[i].device == identity->st_dev && set->seen[i].inode == identity->st_ino) return true;

  SourceIdentity *seen = grow_array(set->seen, &set->seen_capacity, set->seen_count, sizeof(*set->seen));

  if (!seen) return false;

  set->seen = seen;
  set->seen[set->seen_count++] = (SourceIdentity){ identity->st_dev, identity->st_ino };

  return true;
}

static SourceOpenResult add_source(SourceSet *set, const char *path, const struct stat *identity) {
  for (size_t i = 0; i < set->count; i++) {
    SourceEntry *entry = &set->entries[i];

    if ((!path && !entry->path) || (path && entry->path && !strcmp(path, entry->path))) return SOURCE_OPENED;
  }

  if (identity) {
    for (size_t i = 0; i < set->seen_count; i++)
      if (set->seen[i].device == identity->st_dev && set->seen[i].inode == identity->st_ino) return SOURCE_OPENED;
  }

  SourceEntry *entries = grow_array(set->entries, &set->capacity, set->count, sizeof(*set->entries));

  if (!entries) return SOURCE_OPEN_ERROR;

  set->entries = entries;

  char *owned_path = path ? text_copy(path) : nullptr;

  if (path && !owned_path) {
    errno = ENOMEM;
    return SOURCE_OPEN_ERROR;
  }

  LogSource source;
  SourceOpenResult result = source_open(&source, owned_path);

  if (result != SOURCE_OPENED) {
    free(owned_path);
    return result;
  }

  if (source.regular && !remember_identity(set, &source.identity)) {
    source_close(&source);
    free(owned_path);
    return SOURCE_OPEN_ERROR;
  }

  source.source_id = set->count;
  set->entries[set->count++] = (SourceEntry){ source, owned_path };

  return SOURCE_OPENED;
}

static int compare_names(const void *left, const void *right) { return strcmp(*(const char *const *)left, *(const char *const *)right); }

static char **directory_names(const char *path, size_t *count) {
  *count = 0;

  DIR *directory = opendir(path);

  if (!directory) return nullptr;

  size_t capacity = 8;
  char **names = malloc(capacity * sizeof(*names));
  bool success = names != nullptr;

  if (!success) errno = ENOMEM;

  while (success) {
    errno = 0;

    struct dirent *entry = readdir(directory);

    if (!entry) {
      success = errno == 0;
      break;
    }

    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

    char **grown = grow_array(names, &capacity, *count, sizeof(*names));

    if (!grown) {
      success = false;
      break;
    }

    names = grown;
    names[*count] = text_copy(entry->d_name);

    if (!names[*count]) {
      errno = ENOMEM;
      success = false;
      break;
    }

    (*count)++;
  }

  int error = errno;

  if (closedir(directory) < 0 && success) {
    success = false;
    error = errno;
  }

  if (!success) {
    for (size_t i = 0; i < *count; i++) free(names[i]);

    free(names);
    errno = error;
    return nullptr;
  }

  qsort(names, *count, sizeof(*names), compare_names);

  return names;
}

static bool scan_directory(SourceSet *set, const char *path, bool remember) {
  if (remember) {
    for (size_t i = 0; i < set->directory_count; i++)
      if (!strcmp(set->directories[i], path)) return true;

    char **directories = grow_array(set->directories, &set->directory_capacity, set->directory_count, sizeof(*set->directories));

    if (!directories) return false;

    set->directories = directories;

    char *copy = text_copy(path);

    if (!copy) {
      errno = ENOMEM;
      return false;
    }

    set->directories[set->directory_count++] = copy;
  }

  size_t count;
  char **entries = directory_names(path, &count);

  if (!entries) return false;

  bool success = true;

  for (size_t i = 0; i < count; i++) {
    const char *name = entries[i];

    if (!success) {
      free(entries[i]);
      continue;
    }

    size_t length = strlen(path) + strlen(name) + 2;
    char *child = malloc(length);

    if (!child) {
      errno = ENOMEM;
      success = false;
    } else {
      snprintf(child, length, "%s/%s", path, name);

      struct stat identity;

      if (lstat(child, &identity) < 0) success = errno == ENOENT;
      else if (S_ISDIR(identity.st_mode)) success = scan_directory(set, child, false);
      else if (S_ISREG(identity.st_mode)) success = add_source(set, child, &identity) == SOURCE_OPENED;

      free(child);
    }

    free(entries[i]);
  }

  free(entries);

  return success;
}

bool source_set_contains(const SourceSet *set, size_t scope, size_t source) {
  if (scope == SIZE_MAX || scope == source) return true;
  if (scope < SOURCE_GROUP_BASE || scope - SOURCE_GROUP_BASE >= set->group_count) return false;

  const SourceGroup *group = &set->groups[scope - SOURCE_GROUP_BASE];
  for (size_t i = 0; i < group->count; i++)
    if (group->members[i] == source) return true;
  return false;
}

void source_set_close(SourceSet *set) {
  kube_close(set);
  for (size_t i = 0; i < set->group_count; i++) {
    free(set->groups[i].name);
    free(set->groups[i].uid);
    free(set->groups[i].members);
  }
  free(set->groups);
  for (size_t i = 0; i < set->count; i++) {
    source_close(&set->entries[i].source);
    free(set->entries[i].path);
  }

  for (size_t i = 0; i < set->directory_count; i++) free(set->directories[i]);

  free(set->entries);
  free(set->directories);
  free(set->buffer);
  free(set->seen);

  *set = (SourceSet){ 0 };
}

static SourceOpenResult add_path(SourceSet *set, const char *path) {
  if (!path || !strcmp(path, "-")) return add_source(set, nullptr, nullptr);

  struct stat identity;

  if (stat(path, &identity) < 0) return SOURCE_OPEN_ERROR;
  if (!S_ISDIR(identity.st_mode)) return add_source(set, path, &identity);

  return scan_directory(set, path, true) ? SOURCE_OPENED : SOURCE_OPEN_ERROR;
}

static SourceOpenResult add_docker(SourceSet *set, const char *container, const char *tail) {
  SourceEntry *entries = grow_array(set->entries, &set->capacity, set->count, sizeof(*entries));

  if (!entries) return SOURCE_OPEN_ERROR;

  set->entries = entries;

  size_t length = strlen(container) + 8;
  char *name = malloc(length);

  if (!name) return SOURCE_OPEN_ERROR;

  snprintf(name, length, "docker:%s", container);

  LogSource source;

  if (source_open_docker(&source, name, container, tail) != SOURCE_OPENED) {
    free(name);
    return SOURCE_OPEN_ERROR;
  }

  source.source_id = set->count;
  set->entries[set->count++] = (SourceEntry){ source, name };

  return SOURCE_OPENED;
}

static SourceOpenResult prepare_polling(SourceSet *set) {
  set->buffer = malloc(SOURCE_READ_BYTES);

  if (!set->buffer) {
    errno = ENOMEM;
    return SOURCE_OPEN_ERROR;
  }

  clock_gettime(CLOCK_MONOTONIC, &set->refreshed);
  set->initial = set->loading = set->count > 0;
  set->eof = !set->count && !set->target_count;

  return SOURCE_OPENED;
}

SourceOpenResult source_set_open(SourceSet *set, const char *const *paths, size_t count) {
  *set = (SourceSet){ 0 };

  for (size_t i = 0; i < (count ? count : 1); i++) {
    SourceOpenResult result = add_path(set, count ? paths[i] : nullptr);

    if (result != SOURCE_OPENED) return result;
  }

  return prepare_polling(set);
}

static SourceOpenResult add_input(SourceSet *set, const InputSpec *input, const char *tail) {
  switch (input->kind) {
  case INPUT_PATH:
    return add_path(set, input->value);
  case INPUT_DOCKER:
    return add_docker(set, input->value, tail);
  case INPUT_KUBE_DEPLOYMENT:
  case INPUT_KUBE_POD:
    return kube_add_target(set, input) ? SOURCE_OPENED : SOURCE_OPEN_ERROR;
  }

  return SOURCE_OPEN_ERROR;
}

SourceOpenResult source_set_open_inputs(SourceSet *set, const InputSpec *inputs, size_t count, const char *tail) {
  if (!count) return source_set_open(set, nullptr, 0);

  *set = (SourceSet){ 0 };

  for (size_t i = 0; i < count; i++) {
    SourceOpenResult result = add_input(set, &inputs[i], tail);

    if (result != SOURCE_OPENED) return result;
  }

  return prepare_polling(set);
}

bool source_set_poll(SourceSet *set, LogStore *store, bool follow, size_t *changed) {
  *changed = SIZE_MAX;
  if (!kube_poll(set, follow)) return false;
  set->cycle_complete = !set->count;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  if (follow && (now.tv_sec - set->refreshed.tv_sec > 0 || now.tv_nsec - set->refreshed.tv_nsec >= 250000000)) {
    for (size_t i = 0; i < set->directory_count; i++) {
      if (!scan_directory(set, set->directories[i], false) && errno != ENOENT) return false;
    }

    set->refreshed = now;
  }

  if (set->count) {
    set->cursor %= set->count;

    LogSource *source = &set->entries[set->cursor].source;
    source->buffer = set->buffer;

    bool success = source_poll(source, store, follow, changed);
    source->buffer = nullptr;

    if (!success) return false;
    if (source->regular && !remember_identity(set, &source->identity)) return false;

    /* Finish historical regular files in argument/path order, then follow fairly. */
    if (!set->initial || source->eof || !source->regular) {
      set->cursor++;

      if (set->cursor == set->count) {
        set->cursor = 0;
        set->initial = false;
        set->cycle_complete = true;
      }
    }
  }

  set->eof = !set->kube_pending;
  bool loading = false;

  for (size_t i = 0; i < set->count; i++) {
    LogSource *source = &set->entries[i].source;
    set->eof = set->eof && source->eof;
    loading = loading || (source->regular && !source->eof);
  }

  set->loading = loading;

  return true;
}
