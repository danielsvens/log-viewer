#include "log_viewer/search.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double seconds(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return time.tv_sec + time.tv_nsec / 1e9;
}

static SearchStatus wait_complete(Search *search, double start, double *first) {
  for (;;) {
    SearchStatus status = search_status(search);
    if (status.error) {
      fprintf(stderr, "Search failed: %d\n", status.error);
      exit(1);
    }
    if (status.matches && *first < 0) *first = seconds() - start;
    if (status.eof && !status.searching) return status;
    usleep(100);
  }
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  Search *search = NULL;
  double start = seconds(), first = -1;
  if (search_open(&search, argv[1], "", false) != SOURCE_OPENED) {
    perror("search_open");
    return 1;
  }
  SearchStatus status = wait_complete(search, start, &first);
  double index_seconds = seconds() - start;
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  long index_rss = usage.ru_maxrss;
  start = seconds();
  first = -1;
  search_request(search, argv[2]);
  status = wait_complete(search, start, &first);
  double query_seconds = seconds() - start;
  size_t samples[3] = { 0, status.matches / 2, status.matches ? status.matches - 1 : 0 };
  uint64_t ids[3] = { 0 };
  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  LogRecord record;
  for (size_t i = 0; i < 3 && status.matches; i++) {
    if (search_read_result(search, samples[i], &reader, &buffer, &record, 65536) != 1) return 1;
    ids[i] = record.id;
  }
  start = seconds();
  for (size_t i = 0; i < 1000 && status.matches; i++) {
    size_t index = (i * 7919) % status.matches;
    if (search_read_result(search, index, &reader, &buffer, &record, 65536) != 1) return 1;
  }
  double navigation_seconds = seconds() - start;
  struct stat file;
  stat(argv[1], &file);
  getrusage(RUSAGE_SELF, &usage);
  printf(
      "{\"lines\":%zu,\"bytes\":%lld,\"index_seconds\":%.6f,\"index_lines_per_second\":%.0f,\"index_MB_per_second\":%.1f,"
      "\"query_seconds\":%.6f,\"first_result_seconds\":%.6f,\"matches\":%zu,\"sample_ids\":[%llu,%llu,%llu],"
      "\"navigation_us_per_jump\":%.3f,\"index_peak_rss_kib\":%ld,\"peak_rss_kib\":%ld,\"metadata_bytes_per_line\":%zu}\n",
      status.count,
      (long long)file.st_size,
      index_seconds,
      status.count / index_seconds,
      file.st_size / index_seconds / 1e6,
      query_seconds,
      first,
      status.matches,
      (unsigned long long)ids[0],
      (unsigned long long)ids[1],
      (unsigned long long)ids[2],
      navigation_seconds * 1e6 / 1000,
      index_rss,
      usage.ru_maxrss,
      sizeof(LineIndex)
  );
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);
  search_close(search);
  return 0;
}
