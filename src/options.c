#include "log_viewer/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr unsigned char USAGE_TEXT[] = {
#embed "../resources/usage.txt" suffix(, 0) if_empty(0)
};

typedef enum {
  OPTION_HELP,
  OPTION_POSITIONAL,
  OPTION_DUMP,
  OPTION_KUBE,
  OPTION_KUBE_CONTEXT,
  OPTION_KUBE_NAMESPACE,
  OPTION_KUBE_TAIL,
  OPTION_DOCKER,
  OPTION_DOCKER_TAIL,
  OPTION_LEVELS,
  OPTION_FILTER,
} OptionKind;

typedef struct {
  const char *name, *value_name;
  OptionKind kind;
} OptionDefinition;

static const OptionDefinition OPTION_DEFINITIONS[] = {
  { "--help", nullptr, OPTION_HELP },
  { "--", nullptr, OPTION_POSITIONAL },
  { "--dump", nullptr, OPTION_DUMP },
  { "--kube", "deployment/NAME or pod/NAME", OPTION_KUBE },
  { "--kube-context", "NAME", OPTION_KUBE_CONTEXT },
  { "--kube-namespace", "NAME", OPTION_KUBE_NAMESPACE },
  { "--kube-tail", "N|all (nonnegative integer)", OPTION_KUBE_TAIL },
  { "--docker", "CONTAINER", OPTION_DOCKER },
  { "--docker-tail", "N|all (nonnegative integer)", OPTION_DOCKER_TAIL },
  { "--levels", "error,warn,info,debug,unknown (or all/none)", OPTION_LEVELS },
  { "--filter", "QUERY", OPTION_FILTER },
};

typedef struct {
  Options *options;
  const char *kube_context, *kube_namespace, *kube_tail;
  bool positional;
} OptionParser;

const char *options_usage(void) { return (const char *)USAGE_TEXT; }

static bool kube_name_valid(const char *name) {
  size_t length = strlen(name);

  if (!length || length > 253 || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-.") != length) return false;

  for (const char *part = name; *part;) {
    size_t size = strcspn(part, ".");

    if (!size || part[0] == '-' || part[size - 1] == '-') return false;

    part += size;
    if (*part == '.') {
      part++;
      if (!*part) return false;
    }
  }

  return true;
}

static bool add_kube_input(Options *options, const char *value) {
  static const struct {
    const char *prefix;
    InputKind kind;
  } kinds[] = {
    { "deployment/", INPUT_KUBE_DEPLOYMENT },
    { "deploy/", INPUT_KUBE_DEPLOYMENT },
    { "pod/", INPUT_KUBE_POD },
  };

  for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) {
    size_t length = strlen(kinds[i].prefix);

    if (strncmp(value, kinds[i].prefix, length)) continue;
    if (!kube_name_valid(value + length)) return false;

    options->inputs[options->input_count++] = (InputSpec){ .kind = kinds[i].kind, .value = value + length };
    return true;
  }

  return false;
}

static void add_path_input(Options *options, char *value, char **argv) {
  options->inputs[options->input_count++] = (InputSpec){ .kind = INPUT_PATH, .value = value };
  if (!options->path_count) options->path = value;

  argv[1 + options->path_count++] = value;
  options->paths = argv + 1;
}

static bool tail_valid(const char *value) { return !strcmp(value, "all") || (*value && strspn(value, "0123456789") == strlen(value)); }

static const OptionDefinition *find_option(const char *argument) {
  for (size_t i = 0; i < sizeof(OPTION_DEFINITIONS) / sizeof(*OPTION_DEFINITIONS); i++)
    if (!strcmp(argument, OPTION_DEFINITIONS[i].name)) return &OPTION_DEFINITIONS[i];

  return nullptr;
}

static OptionsResult expected_value(Options *options, const OptionDefinition *definition) {
  snprintf(options->error, sizeof(options->error), "Expected %s %s", definition->name, definition->value_name);

  return OPTIONS_ERROR;
}

static OptionsResult apply_option(OptionParser *parser, const OptionDefinition *definition, const char *value) {
  Options *options = parser->options;

  switch (definition->kind) {
  case OPTION_HELP:
    return OPTIONS_HELP;
  case OPTION_POSITIONAL:
    parser->positional = true;
    break;
  case OPTION_DUMP:
    options->dump = true;
    break;
  case OPTION_KUBE:
    if (!add_kube_input(options, value)) return expected_value(options, definition);
    break;
  case OPTION_KUBE_CONTEXT:
    parser->kube_context = value;
    break;
  case OPTION_KUBE_NAMESPACE:
    parser->kube_namespace = value;
    break;
  case OPTION_KUBE_TAIL:
    if (!tail_valid(value)) return expected_value(options, definition);
    parser->kube_tail = value;
    break;
  case OPTION_DOCKER:
    options->inputs[options->input_count++] = (InputSpec){ .kind = INPUT_DOCKER, .value = value };
    break;
  case OPTION_DOCKER_TAIL:
    if (!tail_valid(value)) return expected_value(options, definition);
    options->docker_tail = value;
    break;
  case OPTION_LEVELS:
    if (!log_levels_parse(value, &options->levels)) return expected_value(options, definition);
    break;
  case OPTION_FILTER:
    options->filter = value;
    break;
  }

  return OPTIONS_RUN;
}

static OptionsResult parse_arguments(OptionParser *parser, int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    const char *argument = argv[i];

    if (parser->positional || argument[0] != '-' || !strcmp(argument, "-")) {
      add_path_input(parser->options, argv[i], argv);
      continue;
    }

    const OptionDefinition *definition = find_option(argument);

    if (!definition) {
      snprintf(parser->options->error, sizeof(parser->options->error), "Unknown argument: %s", argument);
      return OPTIONS_ERROR;
    }

    const char *value = nullptr;

    if (definition->value_name) {
      if (++i == argc) return expected_value(parser->options, definition);

      value = argv[i];
      if (!*value && definition->kind != OPTION_FILTER) return expected_value(parser->options, definition);
    }

    OptionsResult result = apply_option(parser, definition, value);

    if (result != OPTIONS_RUN) return result;
  }

  return OPTIONS_RUN;
}

OptionsResult options_parse(Options *options, int argc, char **argv) {
  *options = (Options){ .filter = "", .levels = LOG_LEVELS_ALL, .docker_tail = "1000" };
  options->inputs = calloc((size_t)argc, sizeof(*options->inputs));

  if (!options->inputs) return OPTIONS_ERROR;

  OptionParser parser = { .options = options, .kube_tail = "1000" };
  OptionsResult result = parse_arguments(&parser, argc, argv);

  if (result != OPTIONS_RUN) return result;

  for (size_t i = 0; i < options->input_count; i++) {
    options->inputs[i].kube_context = parser.kube_context;
    options->inputs[i].kube_namespace = parser.kube_namespace;
    options->inputs[i].kube_tail = parser.kube_tail;
  }

  return OPTIONS_RUN;
}
