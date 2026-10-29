#include "log_viewer/source.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void source_close(LogSource *source) {
  if (source->fd >= 0) {
    if (source->file) close(source->fd);
    else if (source->original_flags >= 0) fcntl(source->fd, F_SETFL, source->original_flags);
  }
  if (source->spool_fd >= 0) close(source->spool_fd);
  free(source->buffer);
  *source = (LogSource){ .fd = -1, .original_flags = -1, .spool_fd = -1 };
}

SourceOpenResult source_open(LogSource *source, const char *path) {
  *source = (LogSource){ .fd = -1, .original_flags = -1, .spool_fd = -1, .path = path, .file = path && strcmp(path, "-") != 0 };
  if (!source->file && isatty(STDIN_FILENO)) return SOURCE_INPUT_REQUIRED;
  source->fd = source->file ? open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : STDIN_FILENO;
  if (source->fd < 0) return SOURCE_OPEN_ERROR;
  source->original_flags = fcntl(source->fd, F_GETFL);
  if (source->original_flags < 0 || fcntl(source->fd, F_SETFL, source->original_flags | O_NONBLOCK) < 0 || fstat(source->fd, &source->identity) < 0) goto error;
  source->regular = S_ISREG(source->identity.st_mode);
  if (source->regular && !source->file) {
    off_t offset = lseek(source->fd, 0, SEEK_CUR);
    if (offset < 0) goto error;
    source->offset = source->line_start = (uint64_t)offset;
  }
  if (!source->regular) {
    char path_template[PATH_MAX];
    const char *directory = getenv("TMPDIR");
    if (!directory || !*directory) directory = "/tmp";
    if (snprintf(path_template, sizeof(path_template), "%s/log-viewer-stream-XXXXXX", directory) >= (int)sizeof(path_template)) {
      errno = ENAMETOOLONG;
      goto error;
    }
    source->spool_fd = mkstemp(path_template);
    if (source->spool_fd < 0) goto error;
    unlink(path_template);
    fcntl(source->spool_fd, F_SETFD, FD_CLOEXEC);
  }
  source->buffer = malloc(SOURCE_READ_BYTES);
  if (!source->buffer) goto error;
  return SOURCE_OPENED;
error:;
  int error = errno;
  source_close(source);
  errno = error;
  return SOURCE_OPEN_ERROR;
}

static bool refresh_file(LogSource *source, LogStore *store, size_t *changed) {
  struct stat current;
  int status = source->file ? stat(source->path, &current) : fstat(source->fd, &current);
  if (status < 0) return true;
  bool replaced = current.st_dev != source->identity.st_dev || current.st_ino != source->identity.st_ino;
  if (replaced && !source->eof) return true;
  bool truncated = !replaced && current.st_size < source->identity.st_size;
  if (!replaced && !truncated) {
    source->identity.st_size = current.st_size;
    return true;
  }
  int next = source->file ? open(source->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : dup(source->fd);
  if (next < 0) return true;
  struct stat opened;
  if (fstat(next, &opened) < 0 || !S_ISREG(opened.st_mode)) {
    close(next);
    return true;
  }
  size_t segment;
  if (!log_store_add_file(store, next, &segment)) {
    close(next);
    return false;
  }
  if (truncated) {
    log_store_invalidate(store, source->segment);
    *changed = 0;
  } else *changed = store->count;
  if (source->file) close(source->fd); /* stdin is borrowed even after rotation. */
  else close(next);                    /* keep the original stdin descriptor for flag restoration */
  if (source->file) source->fd = next;
  source->segment = segment;
  source->identity = opened;
  source->offset = source->line_start = 0;
  source->tail_indexed = false;
  source->eof = false;
  return true;
}

static bool spool_bytes(LogSource *source, size_t length) {
  size_t written = 0;
  while (written < length) {
    ssize_t n = pwrite(source->spool_fd, source->buffer + written, length - written, (off_t)(source->offset + written));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      if (!n) errno = EIO;
      return false;
    }
    written += (size_t)n;
  }
  return true;
}

static bool index_bytes(LogSource *source, LogStore *store, size_t length) {
  const char *start = source->buffer, *end = start + length;
  while (start < end) {
    const char *newline = memchr(start, '\n', (size_t)(end - start));
    if (!newline) break;
    uint64_t offset = source->offset + (uint64_t)(newline - source->buffer);
    if (!log_store_append(store, source->line_start, offset - source->line_start)) return false;
    source->line_start = offset + 1;
    start = newline + 1;
  }
  source->offset += length;
  return true;
}

bool source_poll(LogSource *source, LogStore *store, bool follow, size_t *changed_from) {
  *changed_from = SIZE_MAX;
  if (!source->attached) {
    int backing = source->regular ? source->fd : source->spool_fd;
    if (!log_store_add_file(store, backing, &source->segment)) return false;
    source->attached = true;
  }
  if (source->regular && follow && !refresh_file(source, store, changed_from)) return false;
  ssize_t n =
      source->regular ? pread(source->fd, source->buffer, SOURCE_READ_BYTES, (off_t)source->offset) : read(source->fd, source->buffer, SOURCE_READ_BYTES);
  if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
  if (!n) {
    source->eof = true;
    if (source->line_start < source->offset && !source->tail_indexed) {
      if (*changed_from > store->count) *changed_from = store->count;
      if (!log_store_append(store, source->line_start, source->offset - source->line_start)) return false;
      source->tail_indexed = true;
    }
    return true;
  }
  source->eof = false;
  if (source->tail_indexed) {
    log_store_pop_tail(store);
    source->tail_indexed = false;
  }
  if (*changed_from > store->count) *changed_from = store->count;
  if (!source->regular && !spool_bytes(source, (size_t)n)) return false;
  if (!index_bytes(source, store, (size_t)n)) return false;
  if (source->regular && !source->file && lseek(source->fd, (off_t)source->offset, SEEK_SET) < 0) return false;
  return true;
}
