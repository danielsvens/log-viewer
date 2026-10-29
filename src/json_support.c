#include "log_viewer/json_support.h"
#include "log_viewer/text.h"

#include <stdlib.h>
#include <string.h>
#include "json.h"

#define JSON_MAX_DEPTH 128
#define JSON_MAX_ITEMS 16384

/* Check limits before entering the third-party parser's recursive descent. */
static bool within_json_limits(const char *text) {
  int depth = 0;
  size_t items = 0;
  bool quoted = false, escaped = false;
  for (const char *p = text; *p; p++) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (*p == '\\') escaped = true;
      else if (*p == '"') quoted = false;
    } else if (*p == '"') {
      quoted = true;
      items++;
    } else if (*p == '{' || *p == '[') {
      if (++depth > JSON_MAX_DEPTH) return false;
      items++;
    } else if (*p == '}' || *p == ']') depth--;
    else if (*p == ',') items++;
    if (items > JSON_MAX_ITEMS) return false;
  }
  return true;
}

static struct json_value_s *parse_document(const char *text) {
  if (!within_json_limits(text)) return NULL;
  size_t length = strlen(text);
  return json_parse(text, length);
}

static struct json_value_s *find_field(struct json_value_s *value, const char *path) {
  for (const char *part = path; *part;) {
    struct json_object_s *object = json_value_as_object(value);
    if (!object) return NULL;

    const char *end = strchr(part, '.');
    size_t length = end ? (size_t)(end - part) : strlen(part);
    struct json_object_element_s *element = object->start;
    while (element && (element->name->string_size != length || memcmp(element->name->string, part, length) != 0)) element = element->next;
    if (!element) return NULL;

    value = element->value;
    if (!end) return value;
    part = end + 1;
  }
  return value;
}

bool json_decode_string(const char *text, char *output, size_t capacity) {
  struct json_value_s *root = parse_document(text);
  if (!root) return false;

  struct json_string_s *string = json_value_as_string(root);
  bool fits = string && string->string_size < capacity;
  if (fits) {
    memcpy(output, string->string, string->string_size);
    output[string->string_size] = 0;
  }
  free(root);
  return fits;
}

bool json_read_field(const char *text, const char *path, JsonField *field) {
  *field = (JsonField){ 0 };
  struct json_value_s *root = parse_document(text);
  if (!root) return false;

  struct json_value_s *value = find_field(root, path);
  if (value) {
    field->number = value->type == json_type_number;
    if (value->type == json_type_string) field->text = text_copy(((struct json_string_s *)value->payload)->string);
    else if (field->number) field->text = text_copy(((struct json_number_s *)value->payload)->number);
    else field->text = json_write_minified(value, NULL);
  }
  free(root);
  return field->text != NULL;
}

char *json_format_message(const char *text) {
  struct json_value_s *root = parse_document(text);
  if (!root) return text_copy(text);

  char *formatted = json_write_pretty(root, "  ", "\n", NULL);
  free(root);
  return formatted ? formatted : text_copy(text);
}
