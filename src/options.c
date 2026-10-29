#include "log_viewer/options.h"

#include <stdio.h>
#include <string.h>

static constexpr unsigned char USAGE_TEXT[] = {
#embed "../resources/usage.txt" suffix(, 0) if_empty(0)
};

const char *options_usage(void) { return (const char *)USAGE_TEXT; }

OptionsResult options_parse(Options *options, int argc, char **argv) {
  *options = (Options){ .filter = "" };

  for (int i = 1; i < argc; i++) {
    const char *argument = argv[i];

    if (!strcmp(argument, "--help")) return OPTIONS_HELP;

    if (!strcmp(argument, "--dump")) options->dump = true;
    else if (!strcmp(argument, "--filter") && i + 1 < argc) options->filter = argv[++i];
    else if (!options->path && (argument[0] != '-' || !strcmp(argument, "-"))) options->path = argument;
    else {
      snprintf(options->error, sizeof(options->error), "Unknown argument: %s", argument);
      return OPTIONS_ERROR;
    }
  }

  return OPTIONS_RUN;
}
