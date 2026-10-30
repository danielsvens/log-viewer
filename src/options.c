#include "log_viewer/options.h"

#include <stdio.h>
#include <string.h>

static constexpr unsigned char USAGE_TEXT[] = {
#embed "../resources/usage.txt" suffix(, 0) if_empty(0)
};

const char *options_usage(void) { return (const char *)USAGE_TEXT; }

OptionsResult options_parse(Options *options, int argc, char **argv) {
  *options = (Options){ .filter = "" };

  bool positional = false;

  for (int i = 1; i < argc; i++) {
    const char *argument = argv[i];

    if (!positional && !strcmp(argument, "--help")) return OPTIONS_HELP;

    if (!positional && !strcmp(argument, "--")) positional = true;
    else if (!positional && !strcmp(argument, "--dump")) options->dump = true;
    else if (!positional && !strcmp(argument, "--filter") && i + 1 < argc) options->filter = argv[++i];
    else if (positional || argument[0] != '-' || !strcmp(argument, "-")) {
      if (!options->path_count) options->path = argument;
      argv[1 + options->path_count++] = argv[i];
      options->paths = argv + 1;
    } else {
      snprintf(options->error, sizeof(options->error), "Unknown argument: %s", argument);
      return OPTIONS_ERROR;
    }
  }

  return OPTIONS_RUN;
}
