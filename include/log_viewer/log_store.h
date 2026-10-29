#ifndef LOG_VIEWER_LOG_STORE_H
#define LOG_VIEWER_LOG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include "log_viewer/query.h"

#define LOG_STORE_CAPACITY 100000

typedef struct {
  char *text;
  unsigned long id;
} LogRecord;

typedef struct {
  LogRecord *records;
  size_t *visible;
  size_t count, matches, selected;
  unsigned long sequence;
  bool follow;
} LogStore;

bool log_store_init(LogStore *store);
void log_store_destroy(LogStore *store);
/* Takes ownership of text. Store must have been initialized successfully. */
void log_store_append(LogStore *store, const Query *query, char *text);
void log_store_refilter(LogStore *store, const Query *query);
const LogRecord *log_store_selected(const LogStore *store);
void log_store_move(LogStore *store, long delta);
void log_store_set_follow(LogStore *store, bool follow);
void log_store_first(LogStore *store);

#endif
