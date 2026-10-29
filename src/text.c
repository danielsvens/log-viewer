#include "log_viewer/text.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *text_copy(const char *text) {
  size_t size = strlen(text) + 1;
  char *copy = malloc(size);
  if (!copy) return NULL;
  memcpy(copy, text, size);
  return copy;
}

char *text_trim(char *text) {
  while (isspace((unsigned char)*text)) text++;
  size_t length = strlen(text);
  while (length && isspace((unsigned char)text[length - 1])) text[--length] = 0;
  return text;
}

char *text_sanitize(const char *text, size_t length) {
  char *clean = malloc(length + 1);
  if (!clean) return NULL;

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
    if (byte == '\t') clean[written++] = ' ';
    else if (byte >= 32 && byte != 127) clean[written++] = (char)byte;
  }
  clean[written] = 0;
  return clean;
}

LogTone text_log_tone(const char *text) {
  if (strcasestr(text, "error") || strcasestr(text, "fatal") || strcasestr(text, "panic")) return LOG_ERROR;
  if (strcasestr(text, "warn")) return LOG_WARNING;
  if (strcasestr(text, "debug") || strcasestr(text, "trace")) return LOG_DEBUG;
  return LOG_NORMAL;
}
