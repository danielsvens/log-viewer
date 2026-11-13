#pragma once

#include <regex.h>
#include <stddef.h>
#include <stdint.h>
#include "log_viewer/core/text.h"
#include "log_viewer/core/log_level.h"
#include "log_viewer/core/json_support.h"

constexpr int QUERY_CAPACITY = 512;

typedef enum { QUERY_TEXT, QUERY_REGEX, QUERY_JSON } QueryKind;

typedef enum { QUERY_EQUAL, QUERY_NOT_EQUAL, QUERY_CONTAINS, QUERY_GREATER, QUERY_GREATER_EQUAL, QUERY_LESS, QUERY_LESS_EQUAL } QueryOperator;

constexpr int QUERY_MAX_TERMS = 32;
constexpr int QUERY_MAX_JSON_CONDITIONS = 32;
constexpr int QUERY_MAX_JSON_NODES = QUERY_MAX_JSON_CONDITIONS * 2 - 1;

typedef struct {
  char field[128], value[256];
  double numeric_value;
  bool numeric_valid;
  QueryOperator operator;
} JsonCondition;

typedef enum { JSON_EXPRESSION_CONDITION, JSON_EXPRESSION_AND, JSON_EXPRESSION_OR } JsonExpressionKind;

typedef struct {
  JsonExpressionKind kind;
  size_t left, right;
} JsonExpression;

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
  char text[QUERY_CAPACITY];
  JsonCondition conditions[QUERY_MAX_JSON_CONDITIONS];
  JsonExpression expressions[QUERY_MAX_JSON_NODES];
  size_t condition_count, expression_count, root;
  char error[160];
  QueryKind kind;
  regex_t regex;
  bool compiled, allocation_failed;
} Query;

/* Zero-initialize before first use. Compilation also replaces invalid queries. */
bool query_compile(Query *query, const char *text);
QueryResult query_match_filtered_span(const Query *query, const char *text, size_t length, QueryScratch *scratch, LogLevelMask levels);
QueryResult query_match_span(const Query *query, const char *text, size_t length, QueryScratch *scratch);
/* Streaming text/AND evaluation for giant records; state is reset per record. */
QueryResult query_match_text_chunk(const Query *query, const char *text, size_t length, QueryStream *stream, QueryScratch *scratch);
void query_scratch_destroy(QueryScratch *scratch);
bool query_matches(const Query *query, const char *text);
void query_destroy(Query *query);
