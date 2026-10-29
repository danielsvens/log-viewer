#include "log_viewer/search.h"
#include "log_viewer/query.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEARCH_BATCH 8192
#define RESULTS_PER_PAGE 65536

struct Search {
  pthread_mutex_t mutex;
  pthread_cond_t wake;
  pthread_t thread;
  bool started, follow_file, all;
  atomic_bool stop;
  atomic_uint_fast64_t requested;
  uint64_t published;
  char expression[QUERY_CAPACITY];
  LogSource source;
  LogStore store;
  size_t **results, result_pages, result_capacity, matches, scanned;
  int error;
};

static size_t result_id(const Search *search, size_t index) {
  return search->all ? index : search->results[index / RESULTS_PER_PAGE][index % RESULTS_PER_PAGE];
}

static size_t lower_bound(const Search *search, size_t id) {
  size_t low = 0, high = search->matches;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (result_id(search, middle) < id) low = middle + 1;
    else high = middle;
  }
  return low;
}

static bool append_result(Search *search, size_t id) {
  size_t page = search->matches / RESULTS_PER_PAGE;
  if (page == search->result_pages) {
    if (page == search->result_capacity) {
      size_t capacity = search->result_capacity ? search->result_capacity * 2 : 16;
      if (capacity > SIZE_MAX / sizeof(size_t *)) {
        errno = ENOMEM;
        return false;
      }
      size_t **pages = realloc(search->results, capacity * sizeof(*pages));
      if (!pages) return false;
      search->results = pages;
      search->result_capacity = capacity;
    }
    size_t *ids = malloc(RESULTS_PER_PAGE * sizeof(*ids));
    if (!ids) return false;
    search->results[search->result_pages++] = ids;
  }
  search->results[page][search->matches++ % RESULTS_PER_PAGE] = id;
  return true;
}

static void wait_for_work(Search *search) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += 20 * 1000 * 1000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }
  pthread_cond_timedwait(&search->wake, &search->mutex, &until);
}

static QueryResult match_record(Search *search, uint64_t revision, const Query *query, RecordLocation location, RecordReader *reader, QueryScratch *scratch) {
  if (query->kind != QUERY_TEXT || location.length <= SOURCE_READ_BYTES) {
    const char *text;
    int read = record_reader_get(reader, location, &text);
    if (read < 0) return QUERY_FAILURE;
    return read ? query_match_span(query, text, (size_t)location.length, scratch) : QUERY_NO_MATCH;
  }
  if (!location.available) return QUERY_NO_MATCH;
  QueryStream stream = { 0 };
  uint64_t remaining = location.length;
  while (remaining) {
    if (atomic_load(&search->stop) || revision != atomic_load(&search->requested)) return QUERY_NO_MATCH;
    location.length = remaining > SOURCE_READ_BYTES ? SOURCE_READ_BYTES : remaining;
    const char *text;
    int read = record_reader_get(reader, location, &text);
    if (read < 0) return QUERY_FAILURE;
    if (!read) return QUERY_NO_MATCH;
    QueryResult result = query_match_text_chunk(query, text, (size_t)location.length, &stream, scratch);
    if (result != QUERY_NO_MATCH) return result;
    remaining -= location.length;
    location.offset += location.length;
  }
  return QUERY_NO_MATCH;
}

static void *search_worker(void *context) {
  Search *search = context;
  Query query = { 0 };
  QueryScratch scratch = { 0 };
  RecordReader reader = { 0 };
  uint64_t revision = 0;
  size_t scanned = 0;
  while (!atomic_load(&search->stop)) {
    pthread_mutex_lock(&search->mutex);
    uint64_t requested = atomic_load(&search->requested);
    if (requested != revision) {
      char expression[QUERY_CAPACITY];
      memcpy(expression, search->expression, sizeof(expression));
      pthread_mutex_unlock(&search->mutex);
      query_compile(&query, expression);
      pthread_mutex_lock(&search->mutex);
      if (query.allocation_failed) {
        search->error = ENOMEM;
        pthread_mutex_unlock(&search->mutex);
        break;
      }
      revision = requested;
      scanned = 0;
      search->published = revision;
      search->matches = search->scanned = 0;
      search->all = query.kind == QUERY_TEXT && !*query.error && !query.term_count;
    }
    size_t changed;
    bool ingested = source_poll(&search->source, &search->store, search->follow_file, &changed);
    if (!ingested) {
      search->error = errno ? errno : EIO;
      pthread_mutex_unlock(&search->mutex);
      break;
    }
    if (changed < scanned) {
      scanned = changed;
      search->matches = search->all ? changed : lower_bound(search, changed);
      reader.length = 0;
    }
    size_t count = search->store.count;
    bool idle = changed == SIZE_MAX && scanned == count && revision == atomic_load(&search->requested);
    if (search->all || *query.error) {
      search->matches = search->all ? count : 0;
      scanned = count;
      search->scanned = count;
      if (idle) wait_for_work(search);
      pthread_mutex_unlock(&search->mutex);
      continue;
    }
    pthread_mutex_unlock(&search->mutex);

    size_t ids[SEARCH_BATCH], matched = 0, processed = 0;
    int error = 0;
    while (scanned < count && processed++ < SEARCH_BATCH && revision == atomic_load(&search->requested) && !atomic_load(&search->stop)) {
      RecordLocation location;
      log_store_location(&search->store, scanned, &location);
      QueryResult result = match_record(search, revision, &query, location, &reader, &scratch);
      if (result == QUERY_FAILURE) {
        error = errno;
        break;
      }
      if (result == QUERY_MATCH) ids[matched++] = scanned;
      scanned++;
    }
    pthread_mutex_lock(&search->mutex);
    if (revision == atomic_load(&search->requested)) {
      for (size_t i = 0; i < matched && !error; i++)
        if (!append_result(search, ids[i])) error = errno;
      search->scanned = scanned;
    }
    if (error) search->error = error;
    if (idle && !error) wait_for_work(search);
    pthread_mutex_unlock(&search->mutex);
    if (error) break;
  }
  record_reader_destroy(&reader);
  query_scratch_destroy(&scratch);
  query_destroy(&query);
  return NULL;
}

SourceOpenResult search_open(Search **output, const char *path, const char *expression, bool follow) {
  *output = NULL;
  Search *search = calloc(1, sizeof(*search));
  if (!search) return SOURCE_OPEN_ERROR;
  search->source = (LogSource){ .fd = -1, .original_flags = -1, .spool_fd = -1 };
  atomic_init(&search->stop, false);
  atomic_init(&search->requested, 1);
  int error = pthread_mutex_init(&search->mutex, NULL);
  if (error) {
    free(search);
    errno = error;
    return SOURCE_OPEN_ERROR;
  }
  error = pthread_cond_init(&search->wake, NULL);
  if (error) {
    pthread_mutex_destroy(&search->mutex);
    free(search);
    errno = error;
    return SOURCE_OPEN_ERROR;
  }
  SourceOpenResult opened = source_open(&search->source, path);
  if (opened != SOURCE_OPENED) {
    int saved = errno;
    search_close(search);
    errno = saved;
    return opened;
  }
  log_store_init(&search->store);
  snprintf(search->expression, sizeof(search->expression), "%s", expression);
  search->follow_file = follow;
  error = pthread_create(&search->thread, NULL, search_worker, search);
  if (error) {
    search_close(search);
    errno = error;
    return SOURCE_OPEN_ERROR;
  }
  search->started = true;
  *output = search;
  return SOURCE_OPENED;
}

void search_close(Search *search) {
  if (!search) return;
  atomic_store(&search->stop, true);
  pthread_mutex_lock(&search->mutex);
  pthread_cond_signal(&search->wake);
  pthread_mutex_unlock(&search->mutex);
  if (search->started) pthread_join(search->thread, NULL);
  source_close(&search->source);
  log_store_destroy(&search->store);
  for (size_t i = 0; i < search->result_pages; i++) free(search->results[i]);
  free(search->results);
  pthread_cond_destroy(&search->wake);
  pthread_mutex_destroy(&search->mutex);
  free(search);
}

void search_request(Search *search, const char *expression) {
  pthread_mutex_lock(&search->mutex);
  snprintf(search->expression, sizeof(search->expression), "%s", expression);
  atomic_fetch_add(&search->requested, 1);
  pthread_cond_signal(&search->wake);
  pthread_mutex_unlock(&search->mutex);
}

static SearchStatus status_locked(Search *search) {
  bool current = search->published == atomic_load(&search->requested);
  return (SearchStatus){ .count = search->store.count,
                         .matches = current ? search->matches : 0,
                         .scanned = current ? search->scanned : 0,
                         .unavailable = search->store.unavailable,
                         .revision = search->published,
                         .eof = search->source.eof,
                         .searching = !current || search->scanned < search->store.count || (search->source.regular && !search->source.eof),
                         .error = search->error };
}

SearchStatus search_status(Search *search) {
  pthread_mutex_lock(&search->mutex);
  SearchStatus status = status_locked(search);
  pthread_mutex_unlock(&search->mutex);
  return status;
}

void search_sync_view(Search *search, LogView *view) {
  pthread_mutex_lock(&search->mutex);
  SearchStatus status = status_locked(search);
  view->count = status.count;
  view->matches = status.matches;
  view->unavailable = status.unavailable;
  view->searching = status.searching;
  if (view->follow) view->selected = status.matches ? status.matches - 1 : 0;
  else if (status.matches) {
    size_t nearest = lower_bound(search, (size_t)view->anchor);
    if (nearest == status.matches || result_id(search, nearest) > view->anchor) {
      if (nearest) nearest--;
    }
    view->selected = nearest;
  } else view->selected = 0;
  view->revision = status.revision;
  pthread_mutex_unlock(&search->mutex);
}

void log_view_move(Search *search, LogView *view, long delta) {
  pthread_mutex_lock(&search->mutex);
  size_t matches = status_locked(search).matches;
  long selected = (long)view->selected + delta;
  if (selected < 0) selected = 0;
  if (matches && (size_t)selected >= matches) selected = (long)matches - 1;
  view->follow = false;
  view->selected = matches ? (size_t)selected : 0;
  if (matches) view->anchor = result_id(search, view->selected);
  pthread_mutex_unlock(&search->mutex);
}

void log_view_set_follow(Search *search, LogView *view, bool follow) {
  if (!follow) log_view_move(search, view, 0);
  view->follow = follow;
}

void log_view_first(LogView *view) {
  view->follow = false;
  view->selected = 0;
  view->anchor = 0;
}

int search_read_result(Search *search, size_t index, RecordReader *reader, TextBuffer *buffer, LogRecord *record, size_t max_bytes) {
  pthread_mutex_lock(&search->mutex);
  size_t matches = status_locked(search).matches;
  RecordLocation location;
  size_t id = index < matches ? result_id(search, index) : SIZE_MAX;
  bool exists = log_store_location(&search->store, id, &location);
  pthread_mutex_unlock(&search->mutex);
  if (!exists) return 0;
  if (max_bytes && location.length > max_bytes) location.length = max_bytes;
  const char *raw;
  int read = record_reader_get(reader, location, &raw);
  if (read < 0) return -1;
  if (!read) {
    raw = "[record unavailable: source was truncated]";
    location.length = strlen(raw);
  }
  if (!text_buffer_sanitize(buffer, raw, (size_t)location.length)) return -1;
  *record = (LogRecord){ .id = id + 1, .text = buffer->data };
  return 1;
}
