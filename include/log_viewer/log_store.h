#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t INDEX_PAGE_RECORDS = 65536;

/* Exactly 16 bytes per indexed line, independent of text size. */
typedef struct {
  uint64_t offset, length;
} LineIndex;

typedef struct {
  int fd;
  size_t first, count;
  bool available;
  const char *name;
  size_t source_id;
} FileSegment;

typedef struct {
  size_t first, segment;
} RecordRun;

typedef struct {
  LineIndex **pages;
  size_t page_count, page_capacity, count;
  FileSegment *segments;
  size_t segment_count, segment_capacity, unavailable, active_segment;
  RecordRun *runs;
  size_t run_count, run_capacity;
} LogStore;

typedef struct {
  int fd;
  uint64_t offset, length;
  bool available;
  const char *name;
  size_t source_id;
} RecordLocation;

typedef struct {
  char *text;
  const char *source;
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
void log_store_select_file(LogStore *store, size_t segment);
void log_store_update(LogStore *store, size_t id, uint64_t offset, uint64_t length);
bool log_store_location(const LogStore *store, size_t id, RecordLocation *location);
/* 1 = read; 0 = source changed/unavailable; -1 = IO/allocation error. */
int record_reader_get(RecordReader *reader, RecordLocation location, const char **text);
void record_reader_destroy(RecordReader *reader);
