#ifndef LOG_VIEWER_SOURCE_H
#define LOG_VIEWER_SOURCE_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include "log_viewer/log_store.h"

#define SOURCE_READ_BYTES ((size_t)1024 * 1024)

typedef enum { SOURCE_OPENED, SOURCE_OPEN_ERROR, SOURCE_INPUT_REQUIRED } SourceOpenResult;

typedef struct {
  int fd, original_flags, spool_fd;
  const char *path;
  bool file, regular, eof, attached, tail_indexed;
  char *buffer;
  uint64_t offset, line_start;
  size_t segment;
  struct stat identity;
} LogSource;

SourceOpenResult source_open(LogSource *source, const char *path);
void source_close(LogSource *source);
/* changed_from is the earliest changed record, or SIZE_MAX. Mutates only metadata. */
bool source_poll(LogSource *source, LogStore *store, bool follow, size_t *changed_from);
#endif
