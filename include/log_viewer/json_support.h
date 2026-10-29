#ifndef LOG_VIEWER_JSON_SUPPORT_H
#define LOG_VIEWER_JSON_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  void *data;
  size_t capacity;
  char *serialized;
} JsonWorkspace;

typedef struct {
  const char *text;
  size_t length;
  bool number;
} JsonFieldSpan;

typedef enum { JSON_FIELD_ERROR = -1, JSON_FIELD_MISSING, JSON_FIELD_FOUND } JsonFieldResult;

JsonFieldResult json_read_field_reuse(const char *text, size_t length, const char *path, JsonWorkspace *workspace, JsonFieldSpan *field);
void json_workspace_destroy(JsonWorkspace *workspace);

typedef struct {
  char *text;
  bool number;
} JsonField;

bool json_decode_string(const char *text, char *output, size_t capacity);
/* On success, caller owns field->text. Missing fields and invalid JSON fail. */
bool json_read_field(const char *text, const char *path, JsonField *field);
/* Caller owns the result; invalid JSON is copied unchanged. NULL means OOM. */
char *json_format_message(const char *text);

#endif
