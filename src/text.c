#include "log_viewer/text.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *text_copy(const char *text) {
  size_t size = strlen(text) + 1;
  char *copy = malloc(size);

  if (!copy) return nullptr;

  memcpy(copy, text, size);

  return copy;
}

char *text_trim(char *text) {
  while (isspace((unsigned char)*text)) text++;

  size_t length = strlen(text);
  while (length && isspace((unsigned char)text[length - 1])) text[--length] = 0;

  return text;
}

bool text_buffer_reserve(TextBuffer *buffer, size_t size) {
  if (buffer->data && size <= buffer->capacity) return true;

  char *data = realloc(buffer->data, size);

  if (!data) return false;

  buffer->data = data;
  buffer->capacity = size;

  return true;
}

bool text_buffer_sanitize(TextBuffer *buffer, const char *text, size_t length) {
  if (length == SIZE_MAX) {
    errno = EOVERFLOW;
    return false;
  }

  if (!text_buffer_reserve(buffer, length + 1)) return false;

  size_t written = 0;

  for (size_t i = 0; i < length; i++) {
    unsigned char byte = (unsigned char)text[i];

    if (byte == 27) {
      if (i + 1 < length && text[i + 1] == '[') {
        i += 2;
        while (i < length && !(text[i] >= '@' && text[i] <= '~')) i++;
      }
      continue;
    }

    if (byte == '\t') buffer->data[written++] = ' ';
    else if (byte >= 32 && byte != 127) buffer->data[written++] = (char)byte;
  }

  buffer->data[written] = 0;
  buffer->length = written;

  return true;
}

void text_buffer_destroy(TextBuffer *buffer) {
  free(buffer->data);

  *buffer = (TextBuffer){ 0 };
}

char *text_sanitize(const char *text, size_t length) {
  TextBuffer buffer = { 0 };

  if (!text_buffer_sanitize(&buffer, text, length)) return nullptr;

  return buffer.data;
}

LogTone text_log_tone(const char *text) {
  if (strcasestr(text, "error") || strcasestr(text, "fatal") || strcasestr(text, "panic")) return LOG_ERROR;
  if (strcasestr(text, "warn")) return LOG_WARNING;
  if (strcasestr(text, "debug") || strcasestr(text, "trace")) return LOG_DEBUG;

  return LOG_NORMAL;
}
