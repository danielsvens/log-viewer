#include "log_viewer/query.h"
#include "log_viewer/json_support.h"
#include "log_viewer/text.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *parse_field(Query *query, char *expression) {
  char *start = text_trim(expression);
  char *end = start;

  while (isalnum((unsigned char)*end) || *end == '_' || *end == '.' || *end == '-') end++;

  size_t length = (size_t)(end - start);

  if (!length || length >= sizeof(query->field)) return nullptr;

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

  return nullptr;
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

static bool prepare_terms(Query *query);

bool query_compile(Query *query, const char *text) {
  /* Gather before resetting so callers may recompile query->text itself. */
  char expression[QUERY_CAPACITY];
  bool too_long = strlen(text) >= sizeof(expression);
  snprintf(expression, sizeof(expression), "%s", text);

  query_destroy(query);
  *query = (Query){ 0 };
  memcpy(query->text, expression, strlen(expression) + 1);

  if (too_long) {
    snprintf(query->error, sizeof(query->error), "Query exceeds %d bytes", QUERY_CAPACITY - 1);
    return false;
  }

  errno = 0;

  if (*expression == '~') {
    query->kind = QUERY_REGEX;

    int result = regcomp(&query->regex, expression + 1, REG_EXTENDED | REG_ICASE | REG_NOSUB);

    query->compiled = result == 0;
    query->allocation_failed = result == REG_ESPACE;

    if (result) regerror(result, &query->regex, query->error, sizeof(query->error));
  } else if (!strncmp(expression, "json |", 6)) {
    query->kind = QUERY_JSON;

    if (!compile_json_query(query)) {
      query->allocation_failed = errno == ENOMEM;
      snprintf(query->error, sizeof(query->error), "Expected: json | field == \"value\" (== != > >= < <= contains)");
    }
  }

  if (query->kind == QUERY_TEXT && !prepare_terms(query)) snprintf(query->error, sizeof(query->error), "Expected nonempty AND terms (max %d)", QUERY_MAX_TERMS);

  if (query->kind == QUERY_JSON) {
    char *end;
    query->numeric_value = strtod(query->value, &end);
    query->numeric_valid = end != query->value && !*end;
  }

  return !*query->error;
}

static unsigned char folded(unsigned char byte) { return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte + ('a' - 'A')) : byte; }

static bool prepare_terms(Query *query) {
  snprintf(query->terms_text, sizeof(query->terms_text), "%s", query->text);

  bool conjunction = strstr(query->text, " AND ") != nullptr;
  char *part = query->terms_text;

  for (;;) {
    char *end = strstr(part, " AND ");

    if (end) *end = 0;

    char *term = conjunction ? text_trim(part) : part;

    if (!*term && end) return false;

    if (*term) {
      if (query->term_count == QUERY_MAX_TERMS) return false;

      QueryTerm *prepared = &query->terms[query->term_count++];
      prepared->offset = (size_t)(term - query->terms_text);
      prepared->length = strlen(term);

      for (size_t i = 0; i < 256; i++) prepared->skip[i] = (uint16_t)prepared->length;

      for (size_t i = 0; i + 1 < prepared->length; i++) prepared->skip[folded((unsigned char)term[i])] = (uint16_t)(prepared->length - i - 1);
    } else if (query->term_count) return false;

    if (!end) break;

    part = end + 5;
  }

  return true;
}

static bool contains_term(const char *text, size_t length, const char *needle, const QueryTerm *term) {
  size_t size = term->length;

  if (size > length) return false;

  size_t offset = 0;

  while (offset <= length - size) {
    size_t i = size;

    while (i && folded((unsigned char)text[offset + i - 1]) == folded((unsigned char)needle[i - 1])) i--;

    if (!i) return true;

    offset += term->skip[folded((unsigned char)text[offset + size - 1])];
  }

  return false;
}

QueryResult query_match_text_chunk(const Query *query, const char *text, size_t length, QueryStream *stream, QueryScratch *scratch) {
  if (!text_buffer_reserve(&scratch->normalized, length + QUERY_CAPACITY + 1)) return QUERY_FAILURE;

  char *normalized = scratch->normalized.data;
  size_t written = stream->carry_length;
  memcpy(normalized, stream->carry, written);

  for (size_t i = 0; i < length; i++) {
    unsigned char byte = (unsigned char)text[i];

    if (stream->csi) {
      if (byte >= '@' && byte <= '~') stream->csi = false;
      continue;
    }

    if (stream->escape) {
      stream->escape = false;
      if (byte == '[') {
        stream->csi = true;
        continue;
      }
    }

    if (byte == 27) {
      stream->escape = true;
      continue;
    }

    if (byte == '\t') normalized[written++] = ' ';
    else if (byte >= 32 && byte != 127) normalized[written++] = (char)byte;
  }

  size_t longest = 0;
  bool all = true;

  for (size_t i = 0; i < query->term_count; i++) {
    const QueryTerm *term = &query->terms[i];

    if (term->length > longest) longest = term->length;
    if (!stream->found[i]) stream->found[i] = contains_term(normalized, written, query->terms_text + term->offset, term);

    all = all && stream->found[i];
  }

  stream->carry_length = longest && written >= longest ? longest - 1 : written;
  memcpy(stream->carry, normalized + written - stream->carry_length, stream->carry_length);

  return all ? QUERY_MATCH : QUERY_NO_MATCH;
}

static bool compare_field(const Query *query, const JsonFieldSpan *field) {
  size_t length = strlen(query->value);
  bool equal = field->length == length && !memcmp(field->text, query->value, length);

  if (query->operator == QUERY_EQUAL) return equal;
  if (query->operator == QUERY_NOT_EQUAL) return !equal;
  if (query->operator == QUERY_CONTAINS) return memmem(field->text, field->length, query->value, length) != nullptr;
  if (!field->number || !query->numeric_valid) return false;

  char *end;
  double actual = strtod(field->text, &end);

  if ((size_t)(end - field->text) != field->length) return false;

  switch (query->operator) {
  case QUERY_GREATER:
    return actual > query->numeric_value;

  case QUERY_GREATER_EQUAL:
    return actual >= query->numeric_value;

  case QUERY_LESS:
    return actual < query->numeric_value;

  case QUERY_LESS_EQUAL:
    return actual <= query->numeric_value;

  default:
    return false;
  }
}

QueryResult query_match_span(const Query *query, const char *text, size_t length, QueryScratch *scratch) {
  if (*query->error) return QUERY_NO_MATCH;

  bool needs_copy = query->kind == QUERY_JSON;

  for (size_t i = 0; !needs_copy && i < length; i++) {
    unsigned char byte = (unsigned char)text[i];

    if (byte < 32 || byte == 127) needs_copy = true;
  }

  if (needs_copy) {
    if (!text_buffer_sanitize(&scratch->normalized, text, length)) return QUERY_FAILURE;

    text = scratch->normalized.data;
    length = scratch->normalized.length;
  }

  if (query->kind == QUERY_TEXT) {
    for (size_t i = 0; i < query->term_count; i++) {
      const QueryTerm *term = &query->terms[i];

      if (!contains_term(text, length, query->terms_text + term->offset, term)) return QUERY_NO_MATCH;
    }

    return QUERY_MATCH;
  }

  if (query->kind == QUERY_REGEX) {
    if (length > INT32_MAX) {
      errno = EOVERFLOW;
      return QUERY_FAILURE;
    }

    regmatch_t range = { .rm_so = 0, .rm_eo = (regoff_t)length };
    int result = regexec(&query->regex, text, 1, &range, REG_STARTEND);

    if (result != 0 && result != REG_NOMATCH) {
      errno = result == REG_ESPACE ? ENOMEM : EINVAL;
      return QUERY_FAILURE;
    }

    return result == 0 ? QUERY_MATCH : QUERY_NO_MATCH;
  }

  JsonFieldSpan field;
  JsonFieldResult found = json_read_field_reuse(text, length, query->field, &scratch->json, &field);

  if (found == JSON_FIELD_ERROR) return QUERY_FAILURE;

  return found == JSON_FIELD_FOUND && compare_field(query, &field) ? QUERY_MATCH : QUERY_NO_MATCH;
}

bool query_matches(const Query *query, const char *text) {
  QueryScratch scratch = { 0 };
  QueryResult result = query_match_span(query, text, strlen(text), &scratch);

  query_scratch_destroy(&scratch);

  return result == QUERY_MATCH;
}

void query_scratch_destroy(QueryScratch *scratch) {
  text_buffer_destroy(&scratch->normalized);
  json_workspace_destroy(&scratch->json);
}

void query_destroy(Query *query) {
  if (!query->compiled) return;

  regfree(&query->regex);
  query->compiled = false;
}
