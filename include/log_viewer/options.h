#ifndef LOG_VIEWER_OPTIONS_H
#define LOG_VIEWER_OPTIONS_H

typedef struct {
  const char *path;
  const char *filter;
  bool dump;
  char error[160];
} Options;

typedef enum { OPTIONS_RUN, OPTIONS_HELP, OPTIONS_ERROR } OptionsResult;

OptionsResult options_parse(Options *options, int argc, char **argv);
const char *options_usage(void);

#endif
