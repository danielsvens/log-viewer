#include "log_viewer/app.h"
#include "log_viewer/options.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  Options options;
  OptionsResult result = options_parse(&options, argc, argv);

  if (result == OPTIONS_HELP) {
    free(options.inputs);
    puts(options_usage());
    return 0;
  }

  if (result == OPTIONS_ERROR) {
    free(options.inputs);
    fprintf(stderr, "%s\n", options.error);
    return 2;
  }

  int status = app_run(&options);
  free(options.inputs);
  return status;
}
