#ifndef LOG_VIEWER_SOURCE_TREE_H
#define LOG_VIEWER_SOURCE_TREE_H

#include "log_viewer/search.h"

typedef struct {
  const char *name;
  size_t length, depth, source;
  bool directory;
} SourceTreeRow;

typedef struct {
  SourceTreeRow *rows;
  size_t count, capacity, sources, cursor, top;
} SourceTree;

/* Rows borrow immutable source paths from Search. */
bool source_tree_sync(SourceTree *tree, Search *search);
void source_tree_destroy(SourceTree *tree);

#endif
