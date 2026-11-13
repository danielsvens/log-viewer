#pragma once

#include <stddef.h>

typedef struct {
  void *data, *root;
  size_t capacity;
  char *serialized;
} JsonWorkspace;

typedef struct {
  const char *text;
  size_t length;
  bool number;
} JsonFieldSpan;

typedef enum { JSON_FIELD_ERROR = -1, JSON_FIELD_MISSING, JSON_FIELD_FOUND } JsonFieldResult;

/* Parse once, then borrow field spans until the next parse or lookup serialization. */
JsonFieldResult json_workspace_parse(const char *text, size_t length, JsonWorkspace *workspace);
JsonFieldResult json_workspace_field(JsonWorkspace *workspace, const char *path, JsonFieldSpan *field);
JsonFieldResult json_read_field_reuse(const char *text, size_t length, const char *path, JsonWorkspace *workspace, JsonFieldSpan *field);
/* Level metadata accepts case-insensitive keys and literal dotted top-level names. */
JsonFieldResult json_workspace_log_field(JsonWorkspace *workspace, const char *path, JsonFieldSpan *field);
void json_workspace_destroy(JsonWorkspace *workspace);

typedef struct {
  char *text;
  bool number;
} JsonField;

bool json_decode_string(const char *text, char *output, size_t capacity);
/* On success, caller owns field->text. Missing fields and invalid JSON fail. */
bool json_read_field(const char *text, const char *path, JsonField *field);
/* Caller owns the result; invalid JSON is copied unchanged. nullptr means OOM. */
char *json_format_message(const char *text);
