#include "log_viewer/app.h"
#include "log_viewer/options.h"

#include <locale.h>
#include <stdio.h>

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  Options options;
  OptionsResult result = options_parse(&options, argc, argv);
  if (result == OPTIONS_HELP) {
    puts(options_usage());
    return 0;
  }
  if (result == OPTIONS_ERROR) {
    fprintf(stderr, "%s\n", options.error);
    return 2;
  }
  return app_run(&options);
}
