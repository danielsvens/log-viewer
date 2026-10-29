#ifndef LOG_VIEWER_TEXT_H
#define LOG_VIEWER_TEXT_H

#include <stddef.h>

typedef struct {
  char *data;
  size_t length, capacity;
} TextBuffer;

bool text_buffer_reserve(TextBuffer *buffer, size_t size);
bool text_buffer_sanitize(TextBuffer *buffer, const char *text, size_t length);
void text_buffer_destroy(TextBuffer *buffer);

char *text_copy(const char *text);
char *text_trim(char *text);
/* Allocates printable log text, removing control bytes and ANSI CSI sequences. */
char *text_sanitize(const char *text, size_t length);

typedef enum { LOG_NORMAL, LOG_ERROR, LOG_WARNING, LOG_DEBUG } LogTone;

/* Preserves the viewer's existing whole-line severity heuristic. */
LogTone text_log_tone(const char *text);

#endif
