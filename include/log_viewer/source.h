#ifndef LOG_VIEWER_SOURCE_H
#define LOG_VIEWER_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

#define SOURCE_LINE_MAX_BYTES ((size_t)1024 * 1024)

typedef enum { SOURCE_OPENED, SOURCE_OPEN_ERROR, SOURCE_INPUT_REQUIRED } SourceOpenResult;

/* A sink owns each supplied record, including when it returns false. */
typedef bool (*RecordSink)(void *context, char *record);

typedef struct {
  int fd, original_flags;
  const char *path;
  bool file, eof, dropping;
  char *pending;
  size_t pending_length;
  struct stat identity;
} LogSource;

SourceOpenResult source_open(LogSource *source, const char *path);
void source_close(LogSource *source);
/* Bounded ingestion, final pipe record flushing, and optional file following. */
bool source_poll(LogSource *source, bool follow_file, RecordSink sink, void *context);

#endif
