#include "log_viewer/log_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static_assert(sizeof(LineIndex) == 16, "Line metadata must remain compact");

static constexpr size_t READ_WINDOW = 1024 * 1024;

void log_store_init(LogStore *store) { *store = (LogStore){ 0 }; }

void log_store_destroy(LogStore *store) {
  for (size_t i = 0; i < store->page_count; i++) free(store->pages[i]);

  for (size_t i = 0; i < store->segment_count; i++) close(store->segments[i].fd);

  free(store->pages);
  free(store->segments);
  free(store->runs);

  *store = (LogStore){ 0 };
}

bool log_store_add_file(LogStore *store, int fd, size_t *segment) {
  if (store->segment_count == store->segment_capacity) {
    size_t capacity = store->segment_capacity ? store->segment_capacity * 2 : 8;

    if (capacity > SIZE_MAX / sizeof(FileSegment)) {
      errno = ENOMEM;
      return false;
    }

    FileSegment *segments = realloc(store->segments, capacity * sizeof(*segments));

    if (!segments) return false;

    store->segments = segments;
    store->segment_capacity = capacity;
  }

  int owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);

  if (owned < 0) return false;

  *segment = store->segment_count++;
  store->active_segment = *segment;
  store->segments[*segment] = (FileSegment){ .fd = owned, .first = store->count, .available = true };

  return true;
}

void log_store_invalidate(LogStore *store, size_t segment) {
  FileSegment *file = &store->segments[segment];

  if (!file->available) return;

  store->unavailable += file->count;
  file->available = false;
}

void log_store_select_file(LogStore *store, size_t segment) { store->active_segment = segment; }

void log_store_update(LogStore *store, size_t id, uint64_t offset, uint64_t length) {
  store->pages[id / INDEX_PAGE_RECORDS][id % INDEX_PAGE_RECORDS] = (LineIndex){ offset, length };
}

static bool append_run(LogStore *store) {
  if (store->run_count && store->runs[store->run_count - 1].segment == store->active_segment) return true;

  if (store->run_count == store->run_capacity) {
    size_t capacity = store->run_capacity ? store->run_capacity * 2 : 16;

    if (capacity > SIZE_MAX / sizeof(RecordRun)) {
      errno = ENOMEM;
      return false;
    }

    RecordRun *runs = realloc(store->runs, capacity * sizeof(*runs));

    if (!runs) return false;

    store->runs = runs;
    store->run_capacity = capacity;
  }

  store->runs[store->run_count++] = (RecordRun){ store->count, store->active_segment };

  return true;
}

bool log_store_append(LogStore *store, uint64_t offset, uint64_t length) {
  if (!append_run(store)) return false;

  size_t page = store->count / INDEX_PAGE_RECORDS;

  if (page == store->page_count) {
    if (page == store->page_capacity) {
      size_t capacity = store->page_capacity ? store->page_capacity * 2 : 16;

      if (capacity > SIZE_MAX / sizeof(LineIndex *)) {
        errno = ENOMEM;
        return false;
      }

      LineIndex **pages = realloc(store->pages, capacity * sizeof(*pages));

      if (!pages) return false;

      store->pages = pages;
      store->page_capacity = capacity;
    }

    LineIndex *records = malloc(INDEX_PAGE_RECORDS * sizeof(*records));

    if (!records) return false;

    store->pages[store->page_count++] = records;
  }

  store->pages[page][store->count % INDEX_PAGE_RECORDS] = (LineIndex){ offset, length };
  store->count++;
  store->segments[store->active_segment].count++;

  return true;
}

void log_store_pop_tail(LogStore *store) {
  if (!store->count) return;

  store->count--;
  store->segments[store->runs[store->run_count - 1].segment].count--;

  if (store->runs[store->run_count - 1].first == store->count) store->run_count--;
}

bool log_store_location(const LogStore *store, size_t id, RecordLocation *location) {
  if (id >= store->count || !store->run_count) return false;

  size_t low = 0, high = store->run_count;

  while (low + 1 < high) {
    size_t middle = low + (high - low) / 2;

    if (store->runs[middle].first <= id) low = middle;
    else high = middle;
  }

  LineIndex line = store->pages[id / INDEX_PAGE_RECORDS][id % INDEX_PAGE_RECORDS];
  FileSegment segment = store->segments[store->runs[low].segment];

  *location = (RecordLocation){ segment.fd, line.offset, line.length, segment.available, segment.name, segment.source_id };

  return true;
}

int record_reader_get(RecordReader *reader, RecordLocation location, const char **text) {
  if (!location.available) return 0;

  if (location.length > SIZE_MAX - 1 || location.offset > INT64_MAX || location.length > INT64_MAX - location.offset) {
    errno = EOVERFLOW;
    return -1;
  }

  size_t length = (size_t)location.length;

  if (reader->data && reader->fd == location.fd && location.offset >= reader->offset && location.offset - reader->offset <= reader->length &&
      length <= reader->length - (size_t)(location.offset - reader->offset)) {
    *text = reader->data + (size_t)(location.offset - reader->offset);
    return 1;
  }

  size_t capacity = length > READ_WINDOW ? length : READ_WINDOW;

  if (!reader->data || reader->capacity < capacity + 1) {
    char *data = realloc(reader->data, capacity + 1);

    if (!data) return -1;

    reader->data = data;
    reader->capacity = capacity + 1;
  }

  reader->length = 0;
  reader->fd = location.fd;
  reader->offset = location.offset;

  while (reader->length < length || !reader->length) {
    size_t remaining = capacity - reader->length;
    ssize_t n = pread(location.fd, reader->data + reader->length, remaining, (off_t)(location.offset + reader->length));

    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }

    if (!n) {
      if (!length) break;
      return 0;
    }

    reader->length += (size_t)n;
  }

  reader->data[reader->length] = 0;
  *text = reader->data;

  return 1;
}

void record_reader_destroy(RecordReader *reader) {
  free(reader->data);

  *reader = (RecordReader){ 0 };
}
