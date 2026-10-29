#include "log_viewer/query.h"
#include "log_viewer/json_support.h"
#include "log_viewer/text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *parse_field(Query *query, char *expression) {
  char *start = text_trim(expression);
  char *end = start;
  while (isalnum((unsigned char)*end) || *end == '_' || *end == '.' || *end == '-') end++;
  size_t length = (size_t)(end - start);
  if (!length || length >= sizeof(query->field)) return NULL;

  memcpy(query->field, start, length);
  query->field[length] = 0;
  return text_trim(end);
}

static char *parse_operator(Query *query, char *expression) {
  static const struct {
    const char *text;
    QueryOperator operator;
  } operators[] = { { "==", QUERY_EQUAL },  { "!=", QUERY_NOT_EQUAL }, { ">=", QUERY_GREATER_EQUAL }, { "<=", QUERY_LESS_EQUAL },
                    { ">", QUERY_GREATER }, { "<", QUERY_LESS },       { "contains", QUERY_CONTAINS } };

  for (size_t i = 0; i < sizeof(operators) / sizeof(*operators); i++) {
    size_t length = strlen(operators[i].text);
    if (strncmp(expression, operators[i].text, length) != 0) continue;
    if (operators[i].operator == QUERY_CONTAINS && !isspace((unsigned char)expression[length])) continue;
    query->operator = operators[i].operator;
    return text_trim(expression + length);
  }
  return NULL;
}

static bool compile_json_query(Query *query) {
  char expression[QUERY_CAPACITY];
  snprintf(expression, sizeof(expression), "%s", query->text + 6);

  char *operator = parse_field(query, expression);
  if (!operator) return false;
  char *value = parse_operator(query, operator);
  if (!value || !*value || strlen(value) >= sizeof(query->value)) return false;
  if (*value == '"') return json_decode_string(value, query->value, sizeof(query->value));

  snprintf(query->value, sizeof(query->value), "%s", value);
  return true;
}

bool query_compile(Query *query, const char *text) {
  /* Gather before resetting so callers may recompile query->text itself. */
  char expression[QUERY_CAPACITY];
  snprintf(expression, sizeof(expression), "%s", text);
  query_destroy(query);
  *query = (Query){ 0 };
  memcpy(query->text, expression, strlen(expression) + 1);

  if (*expression == '~') {
    query->kind = QUERY_REGEX;
    int result = regcomp(&query->regex, expression + 1, REG_EXTENDED | REG_ICASE | REG_NOSUB);
    query->compiled = result == 0;
    if (result) regerror(result, &query->regex, query->error, sizeof(query->error));
  } else if (!strncmp(expression, "json |", 6)) {
    query->kind = QUERY_JSON;
    if (!compile_json_query(query)) {
      snprintf(query->error, sizeof(query->error), "Expected: json | field == \"value\" (== != > >= < <= contains)");
    }
  }
  return !*query->error;
}

static bool compare_field(const Query *query, const JsonField *field) {
  if (query->operator == QUERY_EQUAL) return !strcmp(field->text, query->value);
  if (query->operator == QUERY_NOT_EQUAL) return strcmp(field->text, query->value) != 0;
  if (query->operator == QUERY_CONTAINS) return strstr(field->text, query->value) != NULL;
  if (!field->number) return false;

  char *field_end, *query_end;
  double actual = strtod(field->text, &field_end);
  double expected = strtod(query->value, &query_end);
  if (field_end == field->text || *field_end || query_end == query->value || *query_end) return false;

  switch (query->operator) {
  case QUERY_GREATER:
    return actual > expected;
  case QUERY_GREATER_EQUAL:
    return actual >= expected;
  case QUERY_LESS:
    return actual < expected;
  case QUERY_LESS_EQUAL:
    return actual <= expected;
  default:
    return false;
  }
}

bool query_matches(const Query *query, const char *text) {
  if (*query->error) return false;
  if (query->kind == QUERY_TEXT) return !*query->text || strcasestr(text, query->text) != NULL;
  if (query->kind == QUERY_REGEX) return query->compiled && regexec(&query->regex, text, 0, NULL, 0) == 0;

  JsonField field;
  if (!json_read_field(text, query->field, &field)) return false;
  bool matches = compare_field(query, &field);
  free(field.text);
  return matches;
}

void query_destroy(Query *query) {
  if (!query->compiled) return;
  regfree(&query->regex);
  query->compiled = false;
}
