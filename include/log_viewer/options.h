#ifndef LOG_VIEWER_OPTIONS_H
#define LOG_VIEWER_OPTIONS_H

#include "log_viewer/log_level.h"
#include "log_viewer/source.h"

typedef struct {
  InputSpec *inputs;
  size_t input_count;
  const char *docker_tail;
  const char *path;
  char **paths;
  unsigned int path_count;
  const char *filter;
  bool dump;
  LogLevelMask levels;
  char error[160];
} Options;

typedef enum { OPTIONS_RUN, OPTIONS_HELP, OPTIONS_ERROR } OptionsResult;

OptionsResult options_parse(Options *options, int argc, char **argv);
const char *options_usage(void);

#endif
