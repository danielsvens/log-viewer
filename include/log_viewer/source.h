#ifndef LOG_VIEWER_SOURCE_H
#define LOG_VIEWER_SOURCE_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "log_viewer/log_store.h"

constexpr size_t SOURCE_GROUP_BASE = SIZE_MAX / 2;
constexpr size_t SOURCE_READ_BYTES = 1024 * 1024;

typedef enum { SOURCE_OPENED, SOURCE_OPEN_ERROR, SOURCE_INPUT_REQUIRED } SourceOpenResult;

typedef enum { INPUT_PATH, INPUT_DOCKER, INPUT_KUBE_DEPLOYMENT, INPUT_KUBE_POD } InputKind;

typedef struct {
  InputKind kind;
  const char *value;
  const char *kube_context, *kube_namespace, *kube_tail;
} InputSpec;

typedef enum { CONNECTION_PENDING, CONNECTION_RESOLVING, CONNECTION_RUNNING, CONNECTION_STOPPED, CONNECTION_ERROR } ConnectionState;

typedef struct {
  int fd, original_flags, spool_fd;
  const char *path;
  bool file, regular, eof, attached, tail_indexed;
  char *buffer;
  uint64_t offset, line_start;
  size_t segment, tail_id, source_id;
  struct stat identity;
  bool managed, reconnect, replayed;
  pid_t child;
  ConnectionState connection;
  char container_id[65], resolution[256];
  size_t resolution_length;
  const char *container, *tail;
  bool kube, kube_available, kube_completed;
  char *kube_context, *kube_namespace, *kube_pod, *kube_uid, *kube_container;
  unsigned int retry;
  long retry_at, running_since;
  char kube_error[512];
  struct ManagedKubeReader *kube_reader;
} LogSource;

SourceOpenResult source_open(LogSource *source, const char *path);
SourceOpenResult source_open_docker(LogSource *source, const char *name, const char *container, const char *tail);
void source_close(LogSource *source);
/* changed_from is the earliest changed record, or SIZE_MAX. Mutates only metadata. */
bool source_poll_stream(LogSource *source, LogStore *store, bool follow, size_t *changed);
bool source_poll(LogSource *source, LogStore *store, bool follow, size_t *changed_from);

#endif
