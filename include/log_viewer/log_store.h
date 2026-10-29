#ifndef LOG_VIEWER_LOG_STORE_H
#define LOG_VIEWER_LOG_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INDEX_PAGE_RECORDS 65536

/* Exactly 16 bytes per indexed line, independent of text size. */
typedef struct {
  uint64_t offset, length;
} LineIndex;

typedef struct {
  int fd;
  size_t first;
  bool available;
} FileSegment;

typedef struct {
  LineIndex **pages;
  size_t page_count, page_capacity, count;
  FileSegment *segments;
  size_t segment_count, segment_capacity, unavailable;
} LogStore;

typedef struct {
  int fd;
  uint64_t offset, length;
  bool available;
} RecordLocation;

typedef struct {
  char *text;
  uint64_t id;
} LogRecord;

typedef struct {
  char *data;
  size_t capacity, length;
  int fd;
  uint64_t offset;
} RecordReader;

void log_store_init(LogStore *store);
void log_store_destroy(LogStore *store);
bool log_store_add_file(LogStore *store, int fd, size_t *segment);
void log_store_invalidate(LogStore *store, size_t segment);
bool log_store_append(LogStore *store, uint64_t offset, uint64_t length);
void log_store_pop_tail(LogStore *store);
bool log_store_location(const LogStore *store, size_t id, RecordLocation *location);
/* 1 = read; 0 = source changed/unavailable; -1 = IO/allocation error. */
int record_reader_get(RecordReader *reader, RecordLocation location, const char **text);
void record_reader_destroy(RecordReader *reader);
#endif
