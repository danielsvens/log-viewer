#ifndef LOG_VIEWER_QUERY_H
#define LOG_VIEWER_QUERY_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "log_viewer/text.h"
#include "log_viewer/json_support.h"

#define QUERY_CAPACITY 512

typedef enum { QUERY_TEXT, QUERY_REGEX, QUERY_JSON } QueryKind;

typedef enum { QUERY_EQUAL, QUERY_NOT_EQUAL, QUERY_CONTAINS, QUERY_GREATER, QUERY_GREATER_EQUAL, QUERY_LESS, QUERY_LESS_EQUAL } QueryOperator;

#define QUERY_MAX_TERMS 32

typedef struct {
  size_t offset, length;
  uint16_t skip[256];
} QueryTerm;

typedef struct {
  TextBuffer normalized;
  JsonWorkspace json;
} QueryScratch;

typedef struct {
  bool found[QUERY_MAX_TERMS];
  char carry[QUERY_CAPACITY];
  size_t carry_length;
  bool escape, csi;
} QueryStream;

typedef enum { QUERY_FAILURE = -1, QUERY_NO_MATCH, QUERY_MATCH } QueryResult;

typedef struct {
  char terms_text[QUERY_CAPACITY];
  QueryTerm terms[QUERY_MAX_TERMS];
  size_t term_count;
  double numeric_value;
  bool numeric_valid;
  char text[QUERY_CAPACITY];
  char field[128];
  char value[256];
  char error[160];
  QueryKind kind;
  QueryOperator operator;
  regex_t regex;
  bool compiled, allocation_failed;
} Query;

/* Zero-initialize before first use. Compilation also replaces invalid queries. */
bool query_compile(Query *query, const char *text);
QueryResult query_match_span(const Query *query, const char *text, size_t length, QueryScratch *scratch);
/* Streaming text/AND evaluation for giant records; state is reset per record. */
QueryResult query_match_text_chunk(const Query *query, const char *text, size_t length, QueryStream *stream, QueryScratch *scratch);
void query_scratch_destroy(QueryScratch *scratch);
bool query_matches(const Query *query, const char *text);
void query_destroy(Query *query);

#endif
