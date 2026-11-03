#pragma once

#include <stddef.h>

typedef struct {
  char *data;
  size_t length, capacity;
} TextBuffer;

bool text_buffer_reserve(TextBuffer *buffer, size_t size);
bool text_buffer_sanitize(TextBuffer *buffer, const char *text, size_t length);
void text_buffer_destroy(TextBuffer *buffer);

bool text_ascii_equal(const char *text, size_t length, const char *other);
char *text_copy(const char *text);
char *text_trim(char *text);
/* Allocates printable log text, removing control bytes and ANSI CSI sequences. */
char *text_sanitize(const char *text, size_t length);
