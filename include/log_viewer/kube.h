#pragma once
#include "log_viewer/source_set.h"
#include "log_viewer/process.h"

typedef struct KubeTarget KubeTarget;
bool kube_add_target(SourceSet *set, const InputSpec *input);
bool kube_poll(SourceSet *set, bool follow);
void kube_close(SourceSet *set);
void kube_refresh(SourceSet *set, size_t scope);
bool kube_reader_poll(LogSource *source, LogStore *store, bool follow, size_t *changed);
void kube_reader_close(LogSource *source);
