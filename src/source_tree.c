#include "log_viewer/source_tree.h"
#include "log_viewer/source_set.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *path;
  size_t source;
} TreeFile;

static int compare_files(const void *left, const void *right) {
  const TreeFile *a = left, *b = right;

  return strcmp(a->path, b->path);
}

static bool append_row(SourceTree *tree, SourceTreeRow row) {
  if (tree->count == tree->capacity) {
    size_t capacity = tree->capacity ? tree->capacity * 2 : 16;

    if (capacity > SIZE_MAX / sizeof(*tree->rows)) {
      errno = ENOMEM;
      return false;
    }

    SourceTreeRow *rows = realloc(tree->rows, capacity * sizeof(*rows));

    if (!rows) return false;

    tree->rows = rows;
    tree->capacity = capacity;
  }

  tree->rows[tree->count++] = row;

  return true;
}

static bool append_group(SourceTree *tree, Search *search, size_t group, size_t depth, size_t count, size_t groups) {
  size_t parent;
  const char *name = search_group_name(search, group, &parent);
  if (!append_row(tree, (SourceTreeRow){ name, strlen(name), depth, SOURCE_GROUP_BASE + group, false })) return false;

  bool children = false;
  for (size_t i = 0; i < groups; i++) {
    size_t p;
    search_group_name(search, i, &p);
    if (p != group) continue;
    children = true;
    if (!append_group(tree, search, i, depth + 1, count, groups)) return false;
  }
  if (!children) {
    for (size_t i = 0; i < count; i++) {
      if (!search_scope_contains(search, SOURCE_GROUP_BASE + group, i)) continue;
      const char *path = search_source_name(search, i);
      const char *leaf = strrchr(path, '/');
      leaf = leaf ? leaf + 1 : path;
      if (!append_row(tree, (SourceTreeRow){ leaf, strlen(leaf), depth + 1, i, false })) return false;
    }
  }
  return true;
}

bool source_tree_sync(SourceTree *tree, Search *search) {
  size_t count = search_source_count(search);

  size_t revision = search_registry_revision(search);
  if (tree->count && tree->sources == revision) return true;
  if (count > SIZE_MAX / sizeof(TreeFile) - 1) {
    errno = ENOMEM;
    return false;
  }

  TreeFile *files = malloc((count + 1) * sizeof(*files));

  if (!files) return false;

  size_t total = count;
  count = 0;
  for (size_t i = 0; i < total; i++) {
    const char *name = search_source_name(search, i);
    bool grouped = false;
    for (size_t g = 0; !grouped && g < search_group_count(search); g++) grouped = search_scope_contains(search, SOURCE_GROUP_BASE + g, i);
    if (grouped) continue;
    files[count++] = (TreeFile){ name, i };
  }

  qsort(files, count, sizeof(*files), compare_files);

  size_t prefix = count ? strlen(files[0].path) : 0;

  for (size_t i = 1; i < count; i++) {
    size_t common = 0;

    while (common < prefix && files[0].path[common] == files[i].path[common]) common++;

    prefix = common;
  }

  while (prefix && files[0].path[prefix - 1] != '/') prefix--;

  size_t cursor_source = tree->cursor < tree->count ? tree->rows[tree->cursor].source : SEARCH_ALL_SOURCES;
  tree->count = tree->cursor = 0;

  bool success = append_row(tree, (SourceTreeRow){ "All sources", 11, 0, SEARCH_ALL_SOURCES, false });

  for (size_t i = 0; success && i < count; i++) {
    const char *path = files[i].path;
    const char *part = path + prefix;
    size_t depth = 0;

    for (const char *slash = strchr(part, '/'); slash; slash = strchr(part, '/')) {
      size_t end = (size_t)(slash - path) + 1;
      bool shared = i > 0 && !strncmp(path, files[i - 1].path, end);

      if (!shared) success = append_row(tree, (SourceTreeRow){ part, (size_t)(slash - part) + 1, depth, SEARCH_ALL_SOURCES, true });
      if (!success) break;

      part = slash + 1;
      depth++;
    }

    if (!success) break;

    success = append_row(tree, (SourceTreeRow){ part, strlen(part), depth, files[i].source, false });

    if (files[i].source == cursor_source) tree->cursor = tree->count - 1;
  }

  size_t groups = search_group_count(search);
  for (size_t i = 0; success && i < groups; i++) {
    size_t parent;
    search_group_name(search, i, &parent);
    if (parent == SIZE_MAX) success = append_group(tree, search, i, 0, total, groups);
  }
  for (size_t i = 0; i < tree->count; i++)
    if (!tree->rows[i].directory && tree->rows[i].source == cursor_source) tree->cursor = i;
  free(files);

  if (success) tree->sources = revision;

  return success;
}

void source_tree_destroy(SourceTree *tree) {
  free(tree->rows);

  *tree = (SourceTree){ 0 };
}
