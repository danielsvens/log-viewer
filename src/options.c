#include "log_viewer/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr unsigned char USAGE_TEXT[] = {
#embed "../resources/usage.txt" suffix(, 0) if_empty(0)
};

const char *options_usage(void) { return (const char *)USAGE_TEXT; }

static bool kube_name_valid(const char *name) {
  size_t length = strlen(name);
  if (!length || length > 253 || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-.") != length) return false;

  for (const char *part = name; *part;) {
    size_t size = strcspn(part, ".");
    if (!size || part[0] == '-' || part[size - 1] == '-') return false;
    part += size;
    if (*part && !*++part) return false;
  }

  return true;
}

OptionsResult options_parse(Options *options, int argc, char **argv) {
  *options = (Options){ .filter = "", .levels = LOG_LEVELS_ALL, .docker_tail = "1000" };

  options->inputs = calloc((size_t)argc, sizeof(*options->inputs));
  if (!options->inputs) return OPTIONS_ERROR;

  bool positional = false;
  const char *kube_context = nullptr, *kube_namespace = nullptr, *kube_tail = "1000";

  for (int i = 1; i < argc; i++) {
    const char *argument = argv[i];

    if (!positional && !strcmp(argument, "--help")) return OPTIONS_HELP;

    if (!positional && !strcmp(argument, "--")) positional = true;
    else if (!positional && !strcmp(argument, "--dump")) options->dump = true;
    else if (!positional && !strcmp(argument, "--kube")) {
      if (++i == argc) {
        snprintf(options->error, sizeof(options->error), "Expected --kube deployment/NAME or pod/NAME");
        return OPTIONS_ERROR;
      }
      const char *name = strchr(argv[i], '/');
      InputKind kind = !strncmp(argv[i], "pod/", 4) ? INPUT_KUBE_POD : INPUT_KUBE_DEPLOYMENT;
      if (!name || !kube_name_valid(name + 1) || (strncmp(argv[i], "pod/", 4) && strncmp(argv[i], "deploy/", 7) && strncmp(argv[i], "deployment/", 11))) {
        snprintf(options->error, sizeof(options->error), "Expected --kube deployment/NAME or pod/NAME");
        return OPTIONS_ERROR;
      }
      options->inputs[options->input_count++] = (InputSpec){ .kind = kind, .value = name + 1 };
    } else if (!positional && (!strcmp(argument, "--kube-context") || !strcmp(argument, "--kube-namespace") || !strcmp(argument, "--kube-tail"))) {
      if (++i == argc || !*argv[i]) {
        snprintf(options->error, sizeof(options->error), "Expected a value for %s", argument);
        return OPTIONS_ERROR;
      }
      if (!strcmp(argument, "--kube-context")) kube_context = argv[i];
      else if (!strcmp(argument, "--kube-namespace")) kube_namespace = argv[i];
      else {
        if (strcmp(argv[i], "all") && strspn(argv[i], "0123456789") != strlen(argv[i])) {
          snprintf(options->error, sizeof(options->error), "Expected --kube-tail N|all (nonnegative integer)");
          return OPTIONS_ERROR;
        }
        kube_tail = argv[i];
      }
    } else if (!positional && !strcmp(argument, "--docker")) {
      if (++i == argc || !*argv[i]) {
        snprintf(options->error, sizeof(options->error), "Expected --docker CONTAINER");
        return OPTIONS_ERROR;
      }
      options->inputs[options->input_count++] = (InputSpec){ .kind = INPUT_DOCKER, .value = argv[i] };
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
      options->inputs[options->input_count++] = (InputSpec){ .kind = INPUT_PATH, .value = argument };
      if (!options->path_count) options->path = argument;
      argv[1 + options->path_count++] = argv[i];
      options->paths = argv + 1;
    } else {
      snprintf(options->error, sizeof(options->error), "Unknown argument: %s", argument);
      return OPTIONS_ERROR;
    }
  }

  for (size_t i = 0; i < options->input_count; i++) {
    options->inputs[i].kube_context = kube_context;
    options->inputs[i].kube_namespace = kube_namespace;
    options->inputs[i].kube_tail = kube_tail;
  }

  return OPTIONS_RUN;
}
