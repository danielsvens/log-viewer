#include "log_viewer/kube.h"
#include "log_viewer/json_support.h"
#include "log_viewer/text.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr long DISCOVERY_INTERVAL_SECONDS = 2;
static constexpr long HEALTHY_STREAM_SECONDS = 30;
static constexpr unsigned int RETRY_EXPONENTIAL_STEPS = 5;
static constexpr long RETRY_MAX_SECONDS = 30;

typedef enum { KUBE_CONFIG, KUBE_RESOURCE, KUBE_REPLICASETS, KUBE_PODS } KubeDiscoveryStage;

struct KubeTarget {
  char *name, *context, *namespace, *uid, *selector, *tail;
  InputKind kind;
  size_t group;
  ManagedProcess command;
  KubeDiscoveryStage stage;
  bool configured, done, failed;
  unsigned int retry;
  long next;
  struct json_value_s *replicas;
  char error[512];
};

struct ManagedKubeReader {
  ManagedProcess verify;
  bool verifying;
  int diagnostic;
};

static struct json_value_s *field(struct json_value_s *value, const char *key) {
  struct json_object_s *object = value ? json_value_as_object(value) : nullptr;

  if (!object) return nullptr;

  for (struct json_object_element_s *e = object->start; e; e = e->next)
    if (!strcmp(e->name->string, key)) return e->value;

  return nullptr;
}

static const char *string(struct json_value_s *value) {
  struct json_string_s *text = value ? json_value_as_string(value) : nullptr;
  return text ? text->string : "";
}

static const char *metadata(struct json_value_s *value, const char *key) { return string(field(field(value, "metadata"), key)); }

static struct json_array_s *array(struct json_value_s *value, const char *key) {
  struct json_value_s *v = field(value, key);
  return v ? json_value_as_array(v) : nullptr;
}

static bool owned(struct json_value_s *value, const char *kind, const char *uid) {
  struct json_array_s *owners = array(field(value, "metadata"), "ownerReferences");

  for (struct json_array_element_s *e = owners ? owners->start : nullptr; e; e = e->next) {
    struct json_value_s *controller = field(e->value, "controller");

    if (controller && controller->type == json_type_true && !strcmp(string(field(e->value, "kind")), kind) && !strcmp(string(field(e->value, "uid")), uid))
      return true;
  }

  return false;
}

static long backoff(unsigned int *attempt) {
  unsigned int n = *attempt;

  if (n < RETRY_EXPONENTIAL_STEPS) {
    (*attempt)++;
    return 1L << n;
  }

  return RETRY_MAX_SECONDS;
}

static size_t add_group(SourceSet *set, const char *name, size_t parent) {
  SourceGroup *groups = realloc(set->groups, (set->group_count + 1) * sizeof(*groups));

  if (!groups) return SIZE_MAX;

  set->groups = groups;

  char *copy = text_copy(name);

  if (!copy) return SIZE_MAX;

  size_t id = set->group_count++;
  set->groups[id] = (SourceGroup){ .name = copy, .parent = parent, .status = "waiting" };
  set->revision++;

  return id;
}

static bool membership(SourceSet *set, size_t group, size_t source) {
  SourceGroup *g = &set->groups[group];

  for (size_t i = 0; i < g->count; i++)
    if (g->members[i] == source) return true;

  size_t *members = realloc(g->members, (g->count + 1) * sizeof(*members));

  if (!members) return false;

  g->members = members;
  g->members[g->count++] = source;
  set->revision++;

  return true;
}

bool kube_add_target(SourceSet *set, const InputSpec *input) {
  KubeTarget *targets = realloc(set->targets, (set->target_count + 1) * sizeof(*targets));

  if (!targets) return false;

  set->targets = targets;

  KubeTarget *target = &targets[set->target_count++];
  *target = (KubeTarget){ .kind = input->kind, .group = SIZE_MAX, .command = { .output = -1, .diagnostic = -1 } };
  target->name = text_copy(input->value);
  target->context = input->kube_context ? text_copy(input->kube_context) : nullptr;
  target->namespace = input->kube_namespace ? text_copy(input->kube_namespace) : nullptr;
  target->tail = text_copy(input->kube_tail ? input->kube_tail : "1000");

  if (!target->name || !target->tail || (input->kube_context && !target->context) || (input->kube_namespace && !target->namespace)) return false;

  size_t length = strlen(input->value) + 12;
  char *name = malloc(length);

  if (!name) return false;

  snprintf(name, length, "%s/%s", input->kind == INPUT_KUBE_POD ? "pod" : "deployment", input->value);
  target->group = add_group(set, name, SIZE_MAX);
  free(name);

  return target->group != SIZE_MAX;
}

static bool launch_target(KubeTarget *target) {
  if (!target->configured) {
    char *args[] = { "kubectl", "config", "view", "-o", "json", nullptr };
    target->stage = KUBE_CONFIG;
    return process_capture(&target->command, args);
  }

  if (target->stage == KUBE_REPLICASETS || target->stage == KUBE_PODS) {
    char *resource = target->stage == KUBE_REPLICASETS ? "replicasets" : "pods";
    char *args[] = { "kubectl",        "--context", target->context, "--namespace", target->namespace, "--request-timeout=10s", "get", resource, "--selector",
                     target->selector, "-o",        "json",          nullptr };

    return process_capture(&target->command, args);
  }

  char *resource = target->kind == INPUT_KUBE_POD ? "pod" : "deployment";
  char *args[] = { "kubectl",    "--context", target->context, "--namespace", target->namespace, "--request-timeout=10s", "get", resource,
                   target->name, "-o",        "json",          nullptr };

  return process_capture(&target->command, args);
}

static bool configure(KubeTarget *target, struct json_value_s *root) {
  const char *context = target->context ? target->context : string(field(root, "current-context"));
  struct json_array_s *contexts = array(root, "contexts");
  for (struct json_array_element_s *e = contexts ? contexts->start : nullptr; e; e = e->next) {
    if (strcmp(string(field(e->value, "name")), context)) continue;
    struct json_value_s *config = field(e->value, "context");
    if (!target->context) target->context = text_copy(context);
    if (!target->namespace) {
      const char *ns = string(field(config, "namespace"));
      target->namespace = text_copy(*ns ? ns : "default");
    }
    target->configured = target->context && target->namespace;
    return target->configured;
  }

  return false;
}

static bool selector_append(TextBuffer *buffer, const char *text) {
  size_t length = strlen(text);

  if (!text_buffer_reserve(buffer, buffer->length + length + 1)) return false;

  memcpy(buffer->data + buffer->length, text, length + 1);
  buffer->length += length;

  return true;
}

static bool append_selector_expression(TextBuffer *out, struct json_value_s *expression) {
  const char *key = string(field(expression, "key"));
  const char *operator = string(field(expression, "operator"));

  if (!strcmp(operator, "Exists")) return selector_append(out, key);
  if (!strcmp(operator, "DoesNotExist")) return selector_append(out, "!") && selector_append(out, key);
  if (strcmp(operator, "In") && strcmp(operator, "NotIn")) return false;

  const char *operation = !strcmp(operator, "In") ? " in (" : " notin (";

  if (!selector_append(out, key) || !selector_append(out, operation)) return false;

  struct json_array_s *values = array(expression, "values");

  for (struct json_array_element_s *value = values ? values->start : nullptr; value; value = value->next) {
    const char *separator = value == values->start ? "" : ",";

    if (!selector_append(out, separator) || !selector_append(out, string(value->value))) return false;
  }

  return selector_append(out, ")");
}

/* Build the Kubernetes label selector, including set-based expressions. */
static char *selector_text(struct json_value_s *selector) {
  TextBuffer out = { 0 };
  bool comma = false;
  struct json_value_s *label_value = field(selector, "matchLabels");
  struct json_object_s *labels = label_value ? json_value_as_object(label_value) : nullptr;

  for (struct json_object_element_s *label = labels ? labels->start : nullptr; label; label = label->next) {
    if (!selector_append(&out, comma ? "," : "") || !selector_append(&out, label->name->string) || !selector_append(&out, "=") ||
        !selector_append(&out, string(label->value)))
      goto error;

    comma = true;
  }

  struct json_array_s *expressions = array(selector, "matchExpressions");

  for (struct json_array_element_s *expression = expressions ? expressions->start : nullptr; expression; expression = expression->next) {
    if (!selector_append(&out, comma ? "," : "") || !append_selector_expression(&out, expression->value)) goto error;

    comma = true;
  }

  if (!out.length) goto error;

  return out.data;

error:
  text_buffer_destroy(&out);
  return nullptr;
}

static bool pod_owned(KubeTarget *target, struct json_value_s *pod) {
  struct json_array_s *replicas = array(target->replicas, "items");
  for (struct json_array_element_s *e = replicas ? replicas->start : nullptr; e; e = e->next)
    if (owned(e->value, "Deployment", target->uid) && owned(pod, "ReplicaSet", metadata(e->value, "uid"))) return true;
  return false;
}

static struct json_value_s *container_status(struct json_value_s *pod, const char *name) {
  struct json_array_s *statuses = array(field(pod, "status"), "containerStatuses");
  for (struct json_array_element_s *e = statuses ? statuses->start : nullptr; e; e = e->next)
    if (!strcmp(string(field(e->value, "name")), name)) return e->value;
  return nullptr;
}

static size_t pod_group(SourceSet *set, const KubeTarget *target, const char *name, const char *uid) {
  if (target->kind == INPUT_KUBE_POD) return target->group;

  size_t group = SIZE_MAX;

  for (size_t i = 0; i < set->group_count; i++) {
    const SourceGroup *candidate = &set->groups[i];

    if (candidate->parent == target->group && candidate->uid && !strcmp(candidate->uid, uid)) group = i;
  }

  if (group != SIZE_MAX) return group;

  group = add_group(set, name, target->group);

  if (group == SIZE_MAX) return SIZE_MAX;

  set->groups[group].uid = text_copy(uid);

  return set->groups[group].uid ? group : SIZE_MAX;
}

static size_t container_source(SourceSet *set, const KubeTarget *target, const char *name, const char *uid, const char *container) {
  for (size_t i = 0; i < set->count; i++) {
    const LogSource *source = &set->entries[i].source;

    if (!source->kube) continue;
    if (strcmp(source->kube_context, target->context) || strcmp(source->kube_namespace, target->namespace)) continue;
    if (!strcmp(source->kube_uid, uid) && !strcmp(source->kube_container, container)) return i;
  }

  if (set->count == set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 8;
    SourceEntry *entries = realloc(set->entries, capacity * sizeof(*entries));

    if (!entries) return SIZE_MAX;

    set->entries = entries;
    set->capacity = capacity;
  }

  size_t length = strlen(target->context) + strlen(target->namespace) + strlen(name) + strlen(container) + 9;
  char *path = malloc(length);

  if (!path) return SIZE_MAX;

  snprintf(path, length, "kube:%s/%s/%s/%s", target->context, target->namespace, name, container);

  LogSource source;

  if (source_open_docker(&source, path, nullptr, target->tail) != SOURCE_OPENED) {
    free(path);
    return SIZE_MAX;
  }

  source.kube = true;
  source.kube_context = text_copy(target->context);
  source.kube_namespace = text_copy(target->namespace);
  source.kube_pod = text_copy(name);
  source.kube_uid = text_copy(uid);
  source.kube_container = text_copy(container);

  if (!source.kube_context || !source.kube_namespace || !source.kube_pod || !source.kube_uid || !source.kube_container) {
    source_close(&source);
    free(path);
    return SIZE_MAX;
  }

  size_t id = set->count++;
  source.source_id = id;
  set->entries[id] = (SourceEntry){ source, path };
  set->revision++;

  return id;
}

static bool update_container_state(LogSource *source, struct json_value_s *pod, const char *container) {
  struct json_value_s *status = container_status(pod, container);
  struct json_value_s *state = field(status, "state");
  bool available = field(state, "running") || field(state, "terminated");
  bool completed = field(state, "terminated") != nullptr;

  struct json_value_s *restart_value = field(status, "restartCount");
  struct json_number_s *restart = restart_value ? json_value_as_number(restart_value) : nullptr;
  const char *instance = string(field(status, "containerID"));
  char attempt[256];

  snprintf(attempt, sizeof(attempt), "%s:%s", restart ? restart->number : "0", instance);

  if (*source->resolution && strcmp(source->resolution, attempt)) {
    source->reconnect = true;
    source->retry_at = 0;
  }

  snprintf(source->resolution, sizeof(source->resolution), "%s", attempt);
  source->kube_available = available;
  source->kube_completed = completed;

  return available;
}

static bool attach_pod(SourceSet *set, KubeTarget *target, struct json_value_s *pod) {
  const char *uid = metadata(pod, "uid"), *name = metadata(pod, "name");

  if (!*uid || !*name) return false;

  size_t group = pod_group(set, target, name, uid);

  if (group == SIZE_MAX) return false;

  struct json_array_s *containers = array(field(pod, "spec"), "containers");

  if (!containers) return false;

  bool unavailable = false;

  for (struct json_array_element_s *entry = containers->start; entry; entry = entry->next) {
    const char *container = string(field(entry->value, "name"));

    if (!*container) return false;

    size_t id = container_source(set, target, name, uid, container);

    if (id == SIZE_MAX) return false;
    if (!membership(set, group, id) || !membership(set, target->group, id)) return false;

    LogSource *source = &set->entries[id].source;

    if (!update_container_state(source, pod, container)) unavailable = true;
  }

  set->groups[group].status = unavailable ? "waiting for containers" : "";
  if (unavailable) target->failed = true;

  return true;
}

static void target_error(SourceSet *set, KubeTarget *target, const char *message, bool follow) {
  snprintf(target->error, sizeof(target->error), "error: %s", message && *message ? message : "Kubernetes metadata request failed or timed out");
  set->groups[target->group].status = target->error;

  target->failed = true;
  target->done = !follow;
  target->next = process_seconds() + backoff(&target->retry);
  target->stage = target->configured ? KUBE_RESOURCE : KUBE_CONFIG;
  set->revision++;
}

static bool configure_targets(SourceSet *set, KubeTarget *target, struct json_value_s *root) {
  if (!configure(target, root)) return false;

  /* All invocation targets share the resolved defaults, pinned once. */
  bool valid = true;

  for (size_t i = 0; i < set->target_count; i++) {
    KubeTarget *other = &set->targets[i];

    if (!other->context) other->context = text_copy(target->context);
    if (!other->namespace) other->namespace = text_copy(target->namespace);

    other->configured = other->context && other->namespace;
    if (!other->configured) valid = false;
    if (!other->command.pid) other->stage = KUBE_RESOURCE;
  }

  target->stage = KUBE_RESOURCE;

  return valid;
}

static void mark_pods_unavailable(SourceSet *set, const KubeTarget *target) {
  const SourceGroup *group = &set->groups[target->group];

  for (size_t i = 0; i < group->count; i++) set->entries[group->members[i]].source.kube_available = false;
}

static bool resolve_target(SourceSet *set, KubeTarget *target, struct json_value_s *root) {
  const char *uid = metadata(root, "uid");

  if (!*uid || (target->uid && strcmp(target->uid, uid))) {
    if (target->uid) mark_pods_unavailable(set, target);
    return false;
  }

  if (!target->uid) {
    target->uid = text_copy(uid);

    if (!target->uid) return false;
  }

  if (target->kind == INPUT_KUBE_POD) {
    target->failed = false;
    return attach_pod(set, target, root);
  }

  free(target->selector);
  target->selector = selector_text(field(field(root, "spec"), "selector"));
  target->stage = KUBE_REPLICASETS;

  return target->selector != nullptr;
}

static bool update_replicasets(KubeTarget *target, struct json_value_s *root, const char *output) {
  if (!array(root, "items")) return false;

  free(target->replicas);
  target->replicas = json_parse(output, strlen(output));
  target->stage = KUBE_PODS;

  return target->replicas != nullptr;
}

static bool update_pods(SourceSet *set, KubeTarget *target, struct json_value_s *root) {
  struct json_array_s *pods = array(root, "items");
  target->failed = false;

  if (!pods) return false;

  /* Only a successful snapshot confirms a pod's removal. */
  mark_pods_unavailable(set, target);

  for (struct json_array_element_s *entry = pods->start; entry; entry = entry->next) {
    if (!pod_owned(target, entry->value)) continue;
    if (!attach_pod(set, target, entry->value)) return false;
  }

  return true;
}

static bool apply_discovery(SourceSet *set, KubeTarget *target, struct json_value_s *root, const char *output, bool *cycle_complete) {
  switch (target->stage) {
  case KUBE_CONFIG:
    return configure_targets(set, target, root);
  case KUBE_RESOURCE:
    *cycle_complete = target->kind == INPUT_KUBE_POD;
    return resolve_target(set, target, root);
  case KUBE_REPLICASETS:
    return update_replicasets(target, root, output);
  case KUBE_PODS:
    *cycle_complete = true;
    return update_pods(set, target, root);
  }

  return false;
}

static void finish_discovery(SourceSet *set, KubeTarget *target, bool follow) {
  target->retry = 0;
  target->next = process_seconds() + DISCOVERY_INTERVAL_SECONDS;
  target->stage = KUBE_RESOURCE;
  target->done = !follow;

  SourceGroup *group = &set->groups[target->group];

  group->status = group->count ? "" : "waiting for pods";
  if (target->failed) group->status = "waiting for containers";
}

static void complete_discovery_command(SourceSet *set, KubeTarget *target, int result, bool follow) {
  char *output = process_output(target->command.output);
  char *diagnostic = process_output(target->command.diagnostic);
  process_close(&target->command);

  JsonWorkspace workspace = { 0 };
  bool parsed = output && json_workspace_parse(output, strlen(output), &workspace) == JSON_FIELD_FOUND;
  bool valid = result > 0 && parsed;
  bool cycle_complete = false;

  if (valid) valid = apply_discovery(set, target, workspace.root, output, &cycle_complete);

  if (!valid) {
    const char *message = result < 0 ? diagnostic : "Invalid Kubernetes metadata or attached target UID changed";
    target_error(set, target, message, follow);
  } else if (cycle_complete) {
    finish_discovery(set, target, follow);
  }

  free(output);
  free(diagnostic);
  json_workspace_destroy(&workspace);
}

bool kube_poll(SourceSet *set, bool follow) {
  set->kube_pending = set->kube_failed = false;

  for (size_t i = 0; i < set->target_count; i++) {
    KubeTarget *target = &set->targets[i];

    if (target->done) {
      set->kube_failed |= target->failed;
      continue;
    }

    set->kube_pending = true;

    if (!target->command.pid) {
      if (!target->configured && i && !set->targets[0].done) continue;
      if (process_seconds() < target->next) continue;

      if (!launch_target(target)) target_error(set, target, strerror(errno), follow);
      continue;
    }

    int result = process_poll(&target->command);

    if (!result) continue;

    complete_discovery_command(set, target, result, follow);
    set->kube_failed |= target->failed;
  }

  return true;
}

void kube_refresh(SourceSet *set, size_t scope) {
  for (size_t i = 0; i < set->target_count; i++) {
    KubeTarget *target = &set->targets[i];
    bool selected = scope == SIZE_MAX || scope == SOURCE_GROUP_BASE + target->group;
    for (size_t j = 0; !selected && j < set->groups[target->group].count; j++) {
      size_t id = set->groups[target->group].members[j];
      selected = source_set_contains(set, scope, id);
    }
    if (selected) {
      target->next = 0;
      target->done = false;
    }
  }
}

void kube_close(SourceSet *set) {
  for (size_t i = 0; i < set->target_count; i++) {
    KubeTarget *t = &set->targets[i];
    process_close(&t->command);
    free(t->name);
    free(t->context);
    free(t->namespace);
    free(t->uid);
    free(t->selector);
    free(t->tail);
    free(t->replicas);
  }
  free(set->targets);
}

void kube_reader_close(LogSource *source) {
  if (source->kube_reader) {
    process_close(&source->kube_reader->verify);
    if (source->kube_reader->diagnostic >= 0) close(source->kube_reader->diagnostic);
    free(source->kube_reader);
  }
  free(source->kube_context);
  free(source->kube_namespace);
  free(source->kube_pod);
  free(source->kube_uid);
  free(source->kube_container);
}

static void stop_reader(LogSource *source) {
  if (source->child > 0) {
    kill(source->child, SIGKILL);
    while (waitpid(source->child, nullptr, 0) < 0 && errno == EINTR) {}
    source->child = 0;
  }
  if (source->fd >= 0) close(source->fd);
  source->fd = -1;
  source->line_start = source->offset;
  source->tail_indexed = false;
}

static bool reader_start(LogSource *source, bool follow) {
  int output[2];

  if (pipe(output) < 0) return false;

  fcntl(output[0], F_SETFD, FD_CLOEXEC);
  fcntl(output[1], F_SETFD, FD_CLOEXEC);

  struct ManagedKubeReader *reader = source->kube_reader;

  if (reader->diagnostic >= 0) close(reader->diagnostic);

  char temporary[] = "/tmp/log-viewer-kube-stderr-XXXXXX";
  reader->diagnostic = mkstemp(temporary);

  if (reader->diagnostic >= 0) {
    unlink(temporary);
    fcntl(reader->diagnostic, F_SETFD, FD_CLOEXEC);
  }

  char *args[] = { "kubectl",
                   "--context",
                   source->kube_context,
                   "--namespace",
                   source->kube_namespace,
                   "logs",
                   source->kube_pod,
                   "--container",
                   source->kube_container,
                   "--tail",
                   !strcmp(source->tail, "all") ? "-1" : (char *)source->tail,
                   nullptr,
                   nullptr };
  if (follow && !source->kube_completed) args[11] = "--follow";

  bool success = reader->diagnostic >= 0 && process_spawn(&source->child, args, output[1], reader->diagnostic);
  close(output[1]);

  if (!success) {
    close(output[0]);
    return false;
  }

  source->fd = output[0];
  fcntl(source->fd, F_SETFL, O_NONBLOCK);
  source->kube_error[0] = 0;
  source->connection = CONNECTION_RUNNING;
  source->running_since = process_seconds();
  source->eof = false;

  return true;
}

static void reader_failed(LogSource *source, bool follow) {
  source->connection = CONNECTION_ERROR;
  source->eof = true;
  source->retry_at = follow ? process_seconds() + backoff(&source->retry) : 0;
}

static bool drain_reader_attempt(LogSource *source, LogStore *store, size_t *changed) {
  if (source->fd < 0) return true;

  /* Drain available bytes before publishing a final partial record. */
  for (;;) {
    uint64_t before = source->offset;
    size_t local = SIZE_MAX;

    if (!source_poll_stream(source, store, false, &local)) return false;

    if (local < *changed) *changed = local;
    if (before == source->offset) break;
  }

  if (!source->tail_indexed && source->line_start < source->offset) {
    log_store_select_file(store, source->segment);

    if (!log_store_append(store, source->line_start, source->offset - source->line_start)) return false;
    if (*changed > store->count - 1) *changed = store->count - 1;
  }

  return true;
}

static bool poll_running_reader(LogSource *source, LogStore *store, bool follow, size_t *changed) {
  if (process_seconds() - source->running_since >= HEALTHY_STREAM_SECONDS) source->retry = 0;

  if (!source_poll_stream(source, store, follow, changed)) return false;
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

  if (success && (!follow || source->kube_completed || !source->kube_available)) {
    source->connection = CONNECTION_STOPPED;
    return true;
  }

  char *diagnostic = process_output(source->kube_reader->diagnostic);
  snprintf(source->kube_error, sizeof(source->kube_error), "error: %s", diagnostic && *diagnostic ? diagnostic : "log reader disconnected");
  free(diagnostic);
  reader_failed(source, follow);

  return true;
}

static bool poll_reader_verification(LogSource *source, bool follow) {
  struct ManagedKubeReader *reader = source->kube_reader;

  source->eof = false;

  if (!reader->verifying) {
    char *args[] = {
      "kubectl", "--context", source->kube_context, "--namespace", source->kube_namespace, "--request-timeout=10s", "get", "pod", source->kube_pod, "-o",
      "json",    nullptr
    };

    if (!process_capture(&reader->verify, args)) {
      reader_failed(source, follow);
      return true;
    }

    reader->verifying = true;
    source->connection = CONNECTION_RESOLVING;
    return true;
  }

  int result = process_poll(&reader->verify);

  if (!result) return true;

  char *text = process_output(reader->verify.output);
  JsonWorkspace workspace = { 0 };
  bool valid = result > 0 && text && json_workspace_parse(text, strlen(text), &workspace) == JSON_FIELD_FOUND &&
               !strcmp(metadata(workspace.root, "uid"), source->kube_uid);

  if (!valid) {
    char *diagnostic = process_output(reader->verify.diagnostic);
    snprintf(
        source->kube_error,
        sizeof(source->kube_error),
        "error: %s",
        result < 0 && diagnostic && *diagnostic ? diagnostic : "pod verification failed or UID changed"
    );
    free(diagnostic);
  }

  free(text);
  json_workspace_destroy(&workspace);
  process_close(&reader->verify);
  reader->verifying = false;

  if (!valid || !reader_start(source, follow)) reader_failed(source, follow);

  return true;
}

bool kube_reader_poll(LogSource *source, LogStore *store, bool follow, size_t *changed) {
  if (!source->kube_reader) {
    source->kube_reader = calloc(1, sizeof(*source->kube_reader));

    if (!source->kube_reader) return false;

    *source->kube_reader = (struct ManagedKubeReader){ .verify = { .output = -1, .diagnostic = -1 }, .diagnostic = -1 };
  }

  struct ManagedKubeReader *reader = source->kube_reader;

  if (source->reconnect) {
    source->reconnect = false;

    if (!drain_reader_attempt(source, store, changed)) return false;

    stop_reader(source);
    process_close(&reader->verify);
    reader->verifying = false;
    source->replayed = true;
    source->connection = CONNECTION_PENDING;
    source->retry_at = 0;
  }

  if (source->connection == CONNECTION_RUNNING) return poll_running_reader(source, store, follow, changed);

  if (source->connection == CONNECTION_STOPPED) return true;

  if (source->connection == CONNECTION_ERROR) {
    if (!follow || !source->kube_available || process_seconds() < source->retry_at) return true;

    source->connection = CONNECTION_PENDING;
    source->replayed = true;
    source->line_start = source->offset;
    source->tail_indexed = false;
  }

  if (!source->kube_available) {
    if (reader->verifying) {
      if (!process_poll(&reader->verify)) return true;

      process_close(&reader->verify);
      reader->verifying = false;
      source->connection = CONNECTION_PENDING;
    }

    source->eof = !follow;
    if (!follow) source->connection = CONNECTION_ERROR;
    return true;
  }

  return poll_reader_verification(source, follow);
}
