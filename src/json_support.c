#include "log_viewer/json_support.h"
#include "log_viewer/text.h"

#include <errno.h>
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
    } else if (*p == '}' || *p == ']') {
      if (depth <= 0) return false;
      depth--;
    } else if (*p == ',') items++;
    if (items > JSON_MAX_ITEMS) return false;
  }
  return true;
}

static struct json_value_s *parse_document(const char *text) {
  errno = 0;
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

static void *workspace_allocate(void *context, size_t size) {
  JsonWorkspace *workspace = context;
  if (size > workspace->capacity) {
    void *data = realloc(workspace->data, size);
    if (!data) return NULL;
    workspace->data = data;
    workspace->capacity = size;
  }
  return workspace->data;
}

JsonFieldResult json_read_field_reuse(const char *text, size_t length, const char *path, JsonWorkspace *workspace, JsonFieldSpan *field) {
  *field = (JsonFieldSpan){ 0 };
  /* The caller supplies a NUL-terminated scratch buffer for the limit check. */
  if (!within_json_limits(text)) return JSON_FIELD_MISSING;
  struct json_parse_result_s result;
  struct json_value_s *root = json_parse_ex(text, length, 0, workspace_allocate, workspace, &result);
  if (!root) {
    if (result.error == json_parse_error_allocator_failed) {
      errno = ENOMEM;
      return JSON_FIELD_ERROR;
    }
    return JSON_FIELD_MISSING;
  }
  struct json_value_s *value = find_field(root, path);
  if (!value) return JSON_FIELD_MISSING;
  field->number = value->type == json_type_number;
  if (value->type == json_type_string) {
    struct json_string_s *string = value->payload;
    field->text = string->string;
    field->length = string->string_size;
  } else if (field->number) {
    struct json_number_s *number = value->payload;
    field->text = number->number;
    field->length = number->number_size;
  } else if (value->type == json_type_true) {
    field->text = "true";
    field->length = 4;
  } else if (value->type == json_type_false) {
    field->text = "false";
    field->length = 5;
  } else if (value->type == json_type_null) {
    field->text = "null";
    field->length = 4;
  } else {
    free(workspace->serialized);
    workspace->serialized = json_write_minified(value, NULL);
    if (!workspace->serialized) {
      errno = ENOMEM;
      return JSON_FIELD_ERROR;
    }
    field->text = workspace->serialized;
    field->length = strlen(field->text);
  }
  return JSON_FIELD_FOUND;
}

void json_workspace_destroy(JsonWorkspace *workspace) {
  free(workspace->data);
  free(workspace->serialized);
  *workspace = (JsonWorkspace){ 0 };
}

bool json_read_field(const char *text, const char *path, JsonField *field) {
  *field = (JsonField){ 0 };
  JsonWorkspace workspace = { 0 };
  JsonFieldSpan span;
  if (json_read_field_reuse(text, strlen(text), path, &workspace, &span) == JSON_FIELD_FOUND) {
    field->text = malloc(span.length + 1);
    field->number = span.number;
    if (field->text) {
      memcpy(field->text, span.text, span.length);
      field->text[span.length] = 0;
    }
  }
  json_workspace_destroy(&workspace);
  return field->text != NULL;
}

char *json_format_message(const char *text) {
  struct json_value_s *root = parse_document(text);
  if (!root) return errno == ENOMEM ? NULL : text_copy(text);

  char *formatted = json_write_pretty(root, "  ", "\n", NULL);
  free(root);
  if (!formatted) errno = ENOMEM;
  return formatted;
}
