#include "log_viewer/app.h"
#include "log_viewer/query.h"
#include "log_viewer/search.h"
#include "log_viewer/tui.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void handle_signal(int number) {
  (void)number;
  interrupted = 1;
}

static int run_dump(Search *search) {
  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };

  size_t printed = 0;
  int result = 0;

  while (!interrupted) {
    SearchStatus status = search_status(search);

    if (status.error) {
      errno = status.error;
      result = 1;
      break;
    }

    while (printed < status.matches && !interrupted) {
      LogRecord record;
      int read = search_read_result(search, printed, &reader, &buffer, &record, 0);

      if (read < 0 || (read && puts(record.text) == EOF)) {
        result = 1;
        break;
      }
      if (!read) break;

      printed++;
    }

    if (result || (status.eof && !status.searching && printed == status.matches)) {
      if (status.connector_failed) {
        result = 1;
        errno = EIO;
      }
      break;
    }

    usleep(1000);
  }

  if (!result && fflush(stdout) == EOF) result = 1;

  int error = errno;
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);
  errno = error;

  return interrupted ? 130 : result;
}

static int run_tui(Search *search, Query *query, Tui *tui) {
  LogView view = { .follow = true };

  while (!interrupted && tui->running) {
    SearchStatus status = search_status(search);

    if (status.error) {
      errno = status.error;
      return 1;
    }

    search_sync_view(search, &view);
    tui_render(tui, search, &view, query, status.eof);

    if (tui->io_error) {
      errno = tui->io_error;
      return 1;
    }

    if (!tui_handle_key(tui, search, &view, query, getch())) return 1;
  }

  return interrupted ? 130 : 0;
}

int app_run(const Options *options) {
  Query query = { 0 };

  if (!query_compile(&query, options->filter)) {
    fprintf(stderr, "%s\n", query.error);
    query_destroy(&query);
    return query.allocation_failed ? 1 : 2;
  }

  Search *search = nullptr;
  Tui tui = { 0 };
  SourceOpenResult opened = search_open_inputs(&search, options->inputs, options->input_count, options->docker_tail, options->filter, !options->dump);

  if (opened != SOURCE_OPENED) {
    if (opened == SOURCE_INPUT_REQUIRED) fprintf(stderr, "Provide a log file or pipe: command | log-viewer\n");
    else perror(options->path_count == 1 ? options->path : "Log inputs");

    query_destroy(&query);
    return opened == SOURCE_INPUT_REQUIRED ? 2 : 1;
  }

  search_set_levels(search, options->levels);

  int result = 0;

  if (!options->dump && !tui_open(&tui)) result = 1;
  else {
    interrupted = 0;

    void (*previous_interrupt)(int) = signal(SIGINT, handle_signal);
    void (*previous_terminate)(int) = signal(SIGTERM, handle_signal);

    result = options->dump ? run_dump(search) : run_tui(search, &query, &tui);

    signal(SIGINT, previous_interrupt);
    signal(SIGTERM, previous_terminate);
  }

  int error = errno;
  tui_close(&tui);
  search_close(search);
  query_destroy(&query);

  if (result == 1) {
    errno = error;
    perror("Log viewer");
  }

  return result;
}
