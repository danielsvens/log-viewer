#include "log_viewer/source.h"
#include "log_viewer/kube.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static bool poll_stream(LogSource *source, LogStore *store, bool follow, size_t *changed_from);
static bool capture_diagnostic(LogSource *source, LogStore *store, const char *message, size_t *changed);

void source_close(LogSource *source) {
  if (source->kube) kube_reader_close(source);
  if (source->child > 0) {
    kill(source->child, SIGKILL);
    while (waitpid(source->child, nullptr, 0) < 0 && errno == EINTR) {}
  }

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

  return SOURCE_OPENED;

error:
  int error = errno;

  source_close(source);
  errno = error;

  return SOURCE_OPEN_ERROR;
}

SourceOpenResult source_open_docker(LogSource *source, const char *name, const char *container, const char *tail) {
  *source = (LogSource){ .fd = -1, .spool_fd = -1, .original_flags = -1, .file = true, .managed = true, .path = name, .container = container, .tail = tail };

  char temporary[] = "/tmp/log-viewer-docker-XXXXXX";
  source->spool_fd = mkstemp(temporary);

  if (source->spool_fd < 0) return SOURCE_OPEN_ERROR;

  unlink(temporary);
  fcntl(source->spool_fd, F_SETFD, FD_CLOEXEC);

  return SOURCE_OPENED;
}

static bool start_process(LogSource *source, bool follow) {
  int output[2];
  if (pipe(output) < 0) return false;

  fcntl(output[0], F_SETFD, FD_CLOEXEC);
  fcntl(output[1], F_SETFD, FD_CLOEXEC);

  int error = 0;

  bool resolve = !*source->container_id;
  char *inspect[] = { "docker", "container", "inspect", "--format", "{{.Id}}", "--", (char *)source->container, nullptr };
  char *logs[] = { "docker", "logs", "--tail", (char *)source->tail, nullptr, nullptr, nullptr, nullptr };
  size_t argument = 4;

  if (follow) logs[argument++] = "--follow";

  logs[argument++] = "--";
  logs[argument] = source->container_id;

  if (!process_spawn(&source->child, resolve ? inspect : logs, output[1], output[1])) error = errno;

  close(output[1]);
  if (error) {
    close(output[0]);
    source->child = 0;
    errno = error;
    return false;
  }

  source->fd = output[0];
  fcntl(source->fd, F_SETFL, O_NONBLOCK);
  source->eof = false;
  source->connection = resolve ? CONNECTION_RESOLVING : CONNECTION_RUNNING;

  return true;
}

static void poll_resolution(LogSource *source) {
  char bytes[4096];
  ssize_t length = read(source->fd, bytes, sizeof(bytes));

  if (length > 0) {
    size_t available = sizeof(source->resolution) - 1 - source->resolution_length;
    size_t copy = (size_t)length < available ? (size_t)length : available;

    memcpy(source->resolution + source->resolution_length, bytes, copy);
    source->resolution_length += copy;
    source->resolution[source->resolution_length] = 0;
    return;
  }

  if (!length || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) source->eof = true;
}

static bool resolve_container_id(LogSource *source) {
  const char *id = source->resolution;

  while (*id) {
    size_t length = strcspn(id, "\r\n");

    if (length == 64 && strspn(id, "0123456789abcdef") == 64) {
      memcpy(source->container_id, id, 64);
      source->container_id[64] = 0;
      return true;
    }

    id += length;
    while (*id == '\r' || *id == '\n') id++;
  }

  return false;
}

/* Never wait for a connector while holding up the UI. EOF precedes exit publication. */
static bool managed_poll(LogSource *source, LogStore *store, bool follow, size_t *changed) {
  if (source->reconnect) {
    source->reconnect = false;
    source->replayed = true;
    source->connection = CONNECTION_PENDING;
    source->eof = false;

    source->line_start = source->offset;
    source->tail_indexed = false;

    source->resolution_length = 0;
    source->resolution[0] = 0;
  }

  if (source->connection == CONNECTION_PENDING && !start_process(source, follow)) {
    int error = errno;
    source->connection = CONNECTION_ERROR;
    source->eof = true;
    return capture_diagnostic(source, store, strerror(error), changed);
  }

  if (source->connection == CONNECTION_ERROR || source->connection == CONNECTION_STOPPED) return true;

  if (source->connection == CONNECTION_RESOLVING && !source->eof) {
    poll_resolution(source);
  } else if (source->connection == CONNECTION_RUNNING && !source->eof) {
    if (!poll_stream(source, store, follow, changed)) return false;
  }

  if (!source->eof) return true;

  int status;
  pid_t ended = waitpid(source->child, &status, WNOHANG);

  if (!ended || (ended < 0 && errno == EINTR)) {
    source->eof = false;
    return true;
  }

  source->child = 0;
  close(source->fd);
  source->fd = -1;

  bool success = ended > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (source->connection == CONNECTION_RUNNING) {
    source->connection = success ? CONNECTION_STOPPED : CONNECTION_ERROR;
    return true;
  }

  if (source->connection == CONNECTION_RESOLVING && success && resolve_container_id(source)) {
    source->connection = CONNECTION_PENDING;
    source->eof = false;
    return true;
  }

  source->connection = CONNECTION_ERROR;
  return capture_diagnostic(source, store, *source->resolution ? source->resolution : "Docker container resolution failed", changed);
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
    if (source->tail_indexed) {
      log_store_update(store, source->tail_id, source->line_start, offset - source->line_start);
      source->tail_indexed = false;
    } else if (!log_store_append(store, source->line_start, offset - source->line_start)) return false;

    source->line_start = offset + 1;
    start = newline + 1;
  }

  source->offset += length;

  return true;
}

static bool poll_stream(LogSource *source, LogStore *store, bool follow, size_t *changed_from) {
  *changed_from = SIZE_MAX;

  if (!source->buffer) {
    source->buffer = malloc(SOURCE_READ_BYTES);

    if (!source->buffer) return false;
  }

  if (!source->attached) {
    int backing = source->regular ? source->fd : source->spool_fd;

    if (!log_store_add_file(store, backing, &source->segment)) return false;

    source->attached = true;
  }

  if (source->regular && follow && !refresh_file(source, store, changed_from)) return false;

  log_store_select_file(store, source->segment);
  store->segments[source->segment].source_id = source->source_id;
  store->segments[source->segment].name = source->path ? source->path : "stdin";

  ssize_t n =
      source->regular ? pread(source->fd, source->buffer, SOURCE_READ_BYTES, (off_t)source->offset) : read(source->fd, source->buffer, SOURCE_READ_BYTES);

  if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;

  if (!n) {
    source->eof = true;

    if (source->line_start < source->offset && !source->tail_indexed) {
      if (*changed_from > store->count) *changed_from = store->count;
      if (!log_store_append(store, source->line_start, source->offset - source->line_start)) return false;

      source->tail_id = store->count - 1;
      source->tail_indexed = true;
    }

    return true;
  }

  source->eof = false;
  if (source->tail_indexed) {
    if (*changed_from > source->tail_id) *changed_from = source->tail_id;
  }

  if (*changed_from > store->count) *changed_from = store->count;

  if (!source->regular && !spool_bytes(source, (size_t)n)) return false;
  if (!index_bytes(source, store, (size_t)n)) return false;

  if (source->tail_indexed) log_store_update(store, source->tail_id, source->line_start, source->offset - source->line_start);
  if (source->regular && !source->file && lseek(source->fd, (off_t)source->offset, SEEK_SET) < 0) return false;

  return true;
}

static bool capture_diagnostic(LogSource *source, LogStore *store, const char *message, size_t *changed) {
  if (!source->attached) {
    if (!log_store_add_file(store, source->spool_fd, &source->segment)) return false;
    source->attached = true;
  }

  log_store_select_file(store, source->segment);
  store->segments[source->segment].source_id = source->source_id;
  store->segments[source->segment].name = source->path;
  *changed = store->count;
  source->line_start = source->offset;
  source->tail_indexed = false;

  size_t length = strlen(message);
  memcpy(source->buffer, message, length);
  source->buffer[length++] = '\n';
  return spool_bytes(source, length) && index_bytes(source, store, length);
}

bool source_poll(LogSource *source, LogStore *store, bool follow, size_t *changed_from) {
  if (!source->managed) return poll_stream(source, store, follow, changed_from);

  *changed_from = SIZE_MAX;
  if (!source->buffer) {
    source->buffer = malloc(SOURCE_READ_BYTES);
    if (!source->buffer) return false;
  }

  if (source->kube) return kube_reader_poll(source, store, follow, changed_from);

  return managed_poll(source, store, follow, changed_from);
}

bool source_poll_stream(LogSource *source, LogStore *store, bool follow, size_t *changed) { return poll_stream(source, store, follow, changed); }
