#include "log_viewer/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr unsigned char USAGE_TEXT[] = {
#embed "../resources/usage.txt" suffix(, 0) if_empty(0)
};

const char *options_usage(void) { return (const char *)USAGE_TEXT; }

OptionsResult options_parse(Options *options, int argc, char **argv) {
  *options = (Options){ .filter = "", .levels = LOG_LEVELS_ALL, .docker_tail = "1000" };

  options->inputs = calloc((size_t)argc, sizeof(*options->inputs));
  if (!options->inputs) return OPTIONS_ERROR;

  bool positional = false;

  for (int i = 1; i < argc; i++) {
    const char *argument = argv[i];

    if (!positional && !strcmp(argument, "--help")) return OPTIONS_HELP;

    if (!positional && !strcmp(argument, "--")) positional = true;
    else if (!positional && !strcmp(argument, "--dump")) options->dump = true;
    else if (!positional && !strcmp(argument, "--docker")) {
      if (++i == argc || !*argv[i]) {
        snprintf(options->error, sizeof(options->error), "Expected --docker CONTAINER");
        return OPTIONS_ERROR;
      }
      options->inputs[options->input_count++] = (InputSpec){ INPUT_DOCKER, argv[i] };
    } else if (!positional && !strcmp(argument, "--docker-tail")) {
      if (++i == argc || (!strcmp(argv[i], "all") ? false : !*argv[i] || strspn(argv[i], "0123456789") != strlen(argv[i]))) {
        snprintf(options->error, sizeof(options->error), "Expected --docker-tail N|all (nonnegative integer)");
        return OPTIONS_ERROR;
      }
      options->docker_tail = argv[i];
    } else if (!positional && !strcmp(argument, "--levels")) {
      if (i + 1 == argc || !log_levels_parse(argv[++i], &options->levels)) {
        snprintf(options->error, sizeof(options->error), "Expected --levels error,warn,info,debug,unknown (or all/none)");
        return OPTIONS_ERROR;
      }
    } else if (!positional && !strcmp(argument, "--filter") && i + 1 < argc) options->filter = argv[++i];
    else if (positional || argument[0] != '-' || !strcmp(argument, "-")) {
      options->inputs[options->input_count++] = (InputSpec){ INPUT_PATH, argument };
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
