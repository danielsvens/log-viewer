#ifndef LOG_VIEWER_OPTIONS_H
#define LOG_VIEWER_OPTIONS_H

#include "log_viewer/log_level.h"

typedef struct {
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
