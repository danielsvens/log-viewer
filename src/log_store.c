#include "log_viewer/log_store.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

bool log_store_init(LogStore *store) {
  *store = (LogStore){ .follow = true };
  store->records = calloc(LOG_STORE_CAPACITY, sizeof(*store->records));
  store->visible = calloc(LOG_STORE_CAPACITY, sizeof(*store->visible));
  if (!store->records || !store->visible) {
    log_store_destroy(store);
    return false;
  }
  return true;
}

void log_store_destroy(LogStore *store) {
  for (size_t i = 0; i < store->count; i++) free(store->records[i].text);
  free(store->records);
  free(store->visible);
  *store = (LogStore){ 0 };
}

const LogRecord *log_store_selected(const LogStore *store) {
  if (!store->matches || store->selected >= store->matches) return NULL;
  assert(store->records && store->visible);
  size_t index = store->visible[store->selected];
  return &store->records[index];
}

void log_store_refilter(LogStore *store, const Query *query) {
  assert(store->records && store->visible);
  const LogRecord *selected = log_store_selected(store);
  unsigned long id = selected ? selected->id : 0;
  size_t matches = 0, restore = 0;
  for (size_t i = 0; i < store->count; i++) {
    if (!query_matches(query, store->records[i].text)) continue;
    if (store->records[i].id <= id) restore = matches;
    store->visible[matches++] = i;
  }
  store->matches = matches;
  store->selected = store->follow && matches ? matches - 1 : restore;
}

static void evict_old_records(LogStore *store, const Query *query) {
  if (store->count < LOG_STORE_CAPACITY) return;

  const LogRecord *selected = log_store_selected(store);
  unsigned long id = selected ? selected->id : 0;
  size_t removed = LOG_STORE_CAPACITY / 10;
  size_t remaining = store->count - removed;
  for (size_t i = 0; i < removed; i++) free(store->records[i].text);
  memmove(store->records, store->records + removed, remaining * sizeof(*store->records));
  store->count = remaining;
  /* Old visible indices no longer address the same records. Rebuild first. */
  store->matches = 0;
  log_store_refilter(store, query);
  if (!store->follow) {
    for (size_t i = 0; i < store->matches; i++) {
      if (store->records[store->visible[i]].id > id) break;
      store->selected = i;
    }
  }
}

void log_store_append(LogStore *store, const Query *query, char *text) {
  assert(store->records && store->visible && text);
  evict_old_records(store, query);
  size_t index = store->count;
  LogRecord record = { .text = text, .id = ++store->sequence };
  bool matches = query_matches(query, text);

  store->records[index] = record;
  if (matches) store->visible[store->matches++] = index;
  store->count++;
  if (store->follow && store->matches) store->selected = store->matches - 1;
}

void log_store_move(LogStore *store, long delta) {
  store->follow = false;
  if (!store->matches) return;

  long next = (long)store->selected + delta;
  if (next < 0) next = 0;
  if ((size_t)next >= store->matches) next = (long)store->matches - 1;
  store->selected = (size_t)next;
}

void log_store_set_follow(LogStore *store, bool follow) {
  store->follow = follow;
  if (follow && store->matches) store->selected = store->matches - 1;
}

void log_store_first(LogStore *store) {
  store->follow = false;
  store->selected = 0;
}
