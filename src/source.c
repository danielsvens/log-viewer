#include "log_viewer/source.h"
#include "log_viewer/text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READ_BUFFER_BYTES 32768
#define READ_BATCHES_PER_POLL 8

void source_close(LogSource *source) {
  if (source->fd >= 0) {
    if (source->file) close(source->fd);
    else if (source->original_flags >= 0) fcntl(source->fd, F_SETFL, source->original_flags);
  }
  free(source->pending);
  *source = (LogSource){ .fd = -1, .original_flags = -1 };
}

SourceOpenResult source_open(LogSource *source, const char *path) {
  *source = (LogSource){ .fd = -1, .original_flags = -1, .path = path, .file = path && strcmp(path, "-") != 0 };
  if (!source->file && isatty(STDIN_FILENO)) return SOURCE_INPUT_REQUIRED;

  source->fd = source->file ? open(path, O_RDONLY | O_NONBLOCK) : STDIN_FILENO;
  if (source->fd < 0) return SOURCE_OPEN_ERROR;
  source->original_flags = fcntl(source->fd, F_GETFL);
  bool ready = source->original_flags >= 0 && fcntl(source->fd, F_SETFL, source->original_flags | O_NONBLOCK) >= 0;
  if (ready && source->file) ready = fstat(source->fd, &source->identity) == 0;
  if (ready) {
    source->pending = malloc(SOURCE_LINE_MAX_BYTES + 1);
    ready = source->pending != NULL;
  }
  if (!ready) {
    int error = errno;
    source_close(source);
    errno = error;
    return SOURCE_OPEN_ERROR;
  }
  return SOURCE_OPENED;
}

static bool emit_record(LogSource *source, RecordSink sink, void *context) {
  char *record = text_sanitize(source->pending, source->pending_length);
  if (!record) return false;
  source->pending_length = 0;
  source->dropping = false;
  return sink(context, record);
}

static bool process_bytes(LogSource *source, const char *bytes, size_t length, RecordSink sink, void *context) {
  for (size_t i = 0; i < length; i++) {
    if (bytes[i] == '\n') {
      if (!emit_record(source, sink, context)) return false;
    } else if (!source->dropping) {
      if (source->pending_length < SOURCE_LINE_MAX_BYTES) source->pending[source->pending_length++] = bytes[i];
      else source->dropping = true;
    }
  }
  return true;
}

static bool read_available(LogSource *source, RecordSink sink, void *context) {
  char bytes[READ_BUFFER_BYTES];
  for (int batch = 0; batch < READ_BATCHES_PER_POLL; batch++) {
    ssize_t length = read(source->fd, bytes, sizeof(bytes));
    if (length == 0) {
      source->eof = true;
      return true;
    }
    if (length < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;

    source->eof = false;
    if (!process_bytes(source, bytes, (size_t)length, sink, context)) return false;
  }
  return true;
}

static bool follow_replacement(LogSource *source, RecordSink sink, void *context) {
  int next = open(source->path, O_RDONLY | O_NONBLOCK);
  if (next < 0) return true; /* Keep following the old descriptor until replacement is readable. */
  struct stat opened;
  if (fstat(next, &opened) < 0) {
    close(next);
    return true;
  }
  if (source->pending_length && !emit_record(source, sink, context)) {
    close(next);
    return false;
  }
  close(source->fd);
  source->fd = next;
  source->identity = opened;
  source->eof = false;
  return true;
}

static bool follow_file_changes(LogSource *source, RecordSink sink, void *context) {
  struct stat current;
  if (stat(source->path, &current) < 0) return true;
  bool replaced = current.st_dev != source->identity.st_dev || current.st_ino != source->identity.st_ino;
  if (replaced) return follow_replacement(source, sink, context);

  off_t offset = lseek(source->fd, 0, SEEK_CUR);
  if (offset >= 0 && current.st_size < offset) {
    if (lseek(source->fd, 0, SEEK_SET) < 0) return false;
    source->pending_length = 0;
    source->dropping = false;
    source->eof = false;
  }
  return true;
}

bool source_poll(LogSource *source, bool follow_file, RecordSink sink, void *context) {
  if (!read_available(source, sink, context)) return false;
  bool flush_final = source->eof && source->pending_length && (!source->file || !follow_file);
  if (flush_final && !emit_record(source, sink, context)) return false;
  if (source->file && follow_file) return follow_file_changes(source, sink, context);
  return true;
}
