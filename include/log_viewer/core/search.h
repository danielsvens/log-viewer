#pragma once

#include "log_viewer/core/source.h"
#include "log_viewer/core/log_level.h"
#include "log_viewer/core/text.h"

#include <stdint.h>

typedef struct Search Search;

constexpr size_t SEARCH_ALL_SOURCES = SIZE_MAX;

typedef struct {
  size_t count, matches, scanned, unavailable;
  /* Records reused on the current search; cache_bytes includes any visible preview. */
  size_t reused, cache_bytes;
  uint64_t revision;
  bool searching, eof, cached;
  int error;
  bool connector_failed;
} SearchStatus;

typedef struct {
  size_t count, matches, selected, unavailable;
  uint64_t anchor, revision;
  bool follow, searching, cached;
} LogView;

SourceOpenResult search_open(Search **search, const char *path, const char *expression, bool follow);
SourceOpenResult search_open_paths(Search **search, const char *const *paths, size_t count, const char *expression, bool follow);
/* Input values and tail must remain valid until search_close(). */
SourceOpenResult search_open_inputs(Search **search, const InputSpec *inputs, size_t count, const char *tail, const char *expression, bool follow);
void search_reconnect(Search *search);
const char *search_source_status(Search *search, size_t source);
void search_close(Search *search);
void search_request(Search *search, const char *expression);
/* Replace query, source, and levels together, cancelling the previous search. */
void search_configure(Search *search, const char *expression, size_t source, LogLevelMask levels);
LogLevelMask search_levels(Search *search);
void search_set_levels(Search *search, LogLevelMask levels);
/* Names are borrowed and remain valid until search_close(), including after discovery. */
size_t search_source_count(Search *search);
size_t search_registry_revision(Search *search);
size_t search_group_count(Search *search);
const char *search_group_name(Search *search, size_t group, size_t *parent);
bool search_scope_contains(Search *search, size_t scope, size_t source);
const char *search_source_name(Search *search, size_t source);
size_t search_selected_source(Search *search);
void search_select_source(Search *search, size_t source);
SearchStatus search_status(Search *search);
void search_sync_view(Search *search, LogView *view);
void log_view_move(Search *search, LogView *view, long delta);
void log_view_set_follow(Search *search, LogView *view, bool follow);
void log_view_first(LogView *view);
/* Returns 1=record, 0=stale result/index, -1=IO/allocation failure. max_bytes=0 reads the whole line. */
int search_read_result(Search *search, size_t index, RecordReader *reader, TextBuffer *buffer, LogRecord *record, size_t max_bytes);
