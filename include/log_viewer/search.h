#ifndef LOG_VIEWER_SEARCH_H
#define LOG_VIEWER_SEARCH_H
#include "log_viewer/source.h"
#include "log_viewer/text.h"
#include <stdint.h>

typedef struct Search Search;

typedef struct {
  size_t count, matches, scanned, unavailable;
  uint64_t revision;
  bool searching, eof;
  int error;
} SearchStatus;

typedef struct {
  size_t count, matches, selected, unavailable;
  uint64_t anchor, revision;
  bool follow, searching;
} LogView;

SourceOpenResult search_open(Search **search, const char *path, const char *expression, bool follow);
void search_close(Search *search);
void search_request(Search *search, const char *expression);
SearchStatus search_status(Search *search);
void search_sync_view(Search *search, LogView *view);
void log_view_move(Search *search, LogView *view, long delta);
void log_view_set_follow(Search *search, LogView *view, bool follow);
void log_view_first(LogView *view);
/* Returns 1=record, 0=stale result/index, -1=IO/allocation failure. max_bytes=0 reads the whole line. */
int search_read_result(Search *search, size_t index, RecordReader *reader, TextBuffer *buffer, LogRecord *record, size_t max_bytes);
#endif
