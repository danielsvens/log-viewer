#ifndef LOG_VIEWER_QUERY_H
#define LOG_VIEWER_QUERY_H

#include <regex.h>
#include <stdbool.h>

#define QUERY_CAPACITY 512

typedef enum { QUERY_TEXT, QUERY_REGEX, QUERY_JSON } QueryKind;

typedef enum { QUERY_EQUAL, QUERY_NOT_EQUAL, QUERY_CONTAINS, QUERY_GREATER, QUERY_GREATER_EQUAL, QUERY_LESS, QUERY_LESS_EQUAL } QueryOperator;

typedef struct {
  char text[QUERY_CAPACITY];
  char field[128];
  char value[256];
  char error[160];
  QueryKind kind;
  QueryOperator operator;
  regex_t regex;
  bool compiled;
} Query;

/* Zero-initialize before first use. Compilation also replaces invalid queries. */
bool query_compile(Query *query, const char *text);
bool query_matches(const Query *query, const char *text);
void query_destroy(Query *query);

#endif
