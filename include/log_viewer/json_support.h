#ifndef LOG_VIEWER_JSON_SUPPORT_H
#define LOG_VIEWER_JSON_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>

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
