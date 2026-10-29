#include "log_viewer/app.h"
#include "log_viewer/log_store.h"
#include "log_viewer/query.h"
#include "log_viewer/source.h"
#include "log_viewer/tui.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

/* Signal handlers only communicate through this async-signal-safe flag. */
static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number) {
  (void)signal_number;
  interrupted = 1;
}

typedef struct {
  const Options *options;
  Query query;
  LogSource source;
  LogStore store;
  Tui tui;
  const char *runtime_error;
} Application;

static bool consume_record(void *context, char *record) {
  Application *app = context;
  if (!app->options->dump) {
    log_store_append(&app->store, &app->query, record);
    return true;
  }

  bool matches = query_matches(&app->query, record);
  bool written = !matches || puts(record) != EOF;
  free(record);
  return written;
}

static int prepare_application(Application *app) {
  if (!query_compile(&app->query, app->options->filter)) {
    fprintf(stderr, "%s\n", app->query.error);
    return 2;
  }
  SourceOpenResult opened = source_open(&app->source, app->options->path);
  if (opened == SOURCE_INPUT_REQUIRED) {
    fprintf(stderr, "Provide a log file or pipe: command | log-viewer\n");
    return 2;
  }
  if (opened == SOURCE_OPEN_ERROR) {
    perror(app->options->path ? app->options->path : "Log input");
    return 1;
  }
  if (app->options->dump) return 0;
  if (!log_store_init(&app->store)) {
    perror("Allocating log store");
    return 1;
  }
  if (!tui_open(&app->tui)) {
    perror("Opening terminal (use --dump for noninteractive output)");
    return 1;
  }
  return 0;
}

static int run_application(Application *app) {
  bool dump = app->options->dump;
  while (!interrupted && (dump || app->tui.running)) {
    if (!source_poll(&app->source, !dump, consume_record, app)) {
      app->runtime_error = "Reading logs";
      return 1;
    }
    if (dump) {
      if (app->source.eof) break;
      usleep(20000);
      continue;
    }
    tui_render(&app->tui, &app->store, &app->query, app->source.eof);
    if (!tui_handle_key(&app->tui, &app->store, &app->query, getch())) {
      app->runtime_error = "Opening log detail";
      return 1;
    }
  }
  return interrupted ? 130 : 0;
}

static void destroy_application(Application *app) {
  tui_close(&app->tui);
  source_close(&app->source);
  log_store_destroy(&app->store);
  query_destroy(&app->query);
}

int app_run(const Options *options) {
  Application app = { .options = options, .source = { .fd = -1, .original_flags = -1 } };
  int result = prepare_application(&app);
  if (result == 0) {
    interrupted = 0;
    void (*previous_interrupt)(int) = signal(SIGINT, handle_signal);
    void (*previous_terminate)(int) = signal(SIGTERM, handle_signal);
    result = run_application(&app);
    signal(SIGINT, previous_interrupt);
    signal(SIGTERM, previous_terminate);
  }
  int error = errno;
  destroy_application(&app);
  if (app.runtime_error) {
    errno = error;
    perror(app.runtime_error);
  }
  return result;
}
