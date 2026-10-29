#ifndef LOG_VIEWER_TEXT_H
#define LOG_VIEWER_TEXT_H

#include <stddef.h>

char *text_copy(const char *text);
char *text_trim(char *text);
/* Allocates printable log text, removing control bytes and ANSI CSI sequences. */
char *text_sanitize(const char *text, size_t length);

typedef enum { LOG_NORMAL, LOG_ERROR, LOG_WARNING, LOG_DEBUG } LogTone;

/* Preserves the viewer's existing whole-line severity heuristic. */
LogTone text_log_tone(const char *text);

#endif
