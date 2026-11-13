#include "log_viewer/core/log_level.h"
#include "log_viewer/core/text.h"

#include <ctype.h>
#include <string.h>

const char *log_level_name(LogLevel level) {
  switch (level) {
  case LOG_LEVEL_ERROR:
    return "error";
  case LOG_LEVEL_WARN:
    return "warn";
  case LOG_LEVEL_INFO:
    return "info";
  case LOG_LEVEL_DEBUG:
    return "debug";
  default:
    return "unknown";
  }
}

static LogLevel named_level(const char *text, size_t length) {
  if (text_ascii_equal(text, length, "error") || text_ascii_equal(text, length, "err") || text_ascii_equal(text, length, "fatal") ||
      text_ascii_equal(text, length, "critical") || text_ascii_equal(text, length, "crit") || text_ascii_equal(text, length, "panic") ||
      text_ascii_equal(text, length, "alert") || text_ascii_equal(text, length, "emerg") || text_ascii_equal(text, length, "emergency"))
    return LOG_LEVEL_ERROR;

  if (text_ascii_equal(text, length, "warn") || text_ascii_equal(text, length, "warning")) return LOG_LEVEL_WARN;

  if (text_ascii_equal(text, length, "info") || text_ascii_equal(text, length, "information") || text_ascii_equal(text, length, "informational") ||
      text_ascii_equal(text, length, "notice"))
    return LOG_LEVEL_INFO;

  if (text_ascii_equal(text, length, "debug") || text_ascii_equal(text, length, "trace") || text_ascii_equal(text, length, "verbose")) return LOG_LEVEL_DEBUG;

  return LOG_LEVEL_UNKNOWN;
}

bool log_levels_parse(const char *text, LogLevelMask *mask) {
  while (isspace((unsigned char)*text)) text++;

  size_t input_length = strlen(text);

  while (input_length && isspace((unsigned char)text[input_length - 1])) input_length--;

  if (text_ascii_equal(text, input_length, "all")) {
    *mask = LOG_LEVELS_ALL;
    return true;
  }

  if (text_ascii_equal(text, input_length, "none")) {
    *mask = 0;
    return true;
  }

  LogLevelMask result = 0;
  const char *part = text;

  for (;;) {
    while (isspace((unsigned char)*part)) part++;

    const char *end = strchr(part, ',');
    size_t length = end ? (size_t)(end - part) : strlen(part);

    while (length && isspace((unsigned char)part[length - 1])) length--;
    if (!length) return false;

    LogLevel level = named_level(part, length);

    if (level == LOG_LEVEL_UNKNOWN && !text_ascii_equal(part, length, "unknown")) return false;

    result |= 1u << level;
    if (!end) break;

    part = end + 1;
  }

  *mask = result;

  return true;
}

typedef enum { LEVEL_TEXT, LEVEL_PINO, LEVEL_SYSLOG, LEVEL_PYTHON, LEVEL_LOGBACK, LEVEL_OTEL } LevelEncoding;

static LogLevel numeric_level(const JsonFieldSpan *field, LevelEncoding encoding) {
  unsigned int value = 0;

  if (!field->length || field->length > 6) return LOG_LEVEL_UNKNOWN;

  for (size_t i = 0; i < field->length; i++) {
    unsigned char byte = (unsigned char)field->text[i];

    if (byte < '0' || byte > '9') return LOG_LEVEL_UNKNOWN;

    value = value * 10 + (unsigned int)(byte - '0');
  }

  static constexpr LogLevel SYSLOG_LEVELS[] = { LOG_LEVEL_ERROR, LOG_LEVEL_ERROR, LOG_LEVEL_ERROR, LOG_LEVEL_ERROR,
                                                LOG_LEVEL_WARN,  LOG_LEVEL_INFO,  LOG_LEVEL_INFO,  LOG_LEVEL_DEBUG };
  static constexpr LogLevel PINO_LEVELS[] = { LOG_LEVEL_UNKNOWN, LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG, LOG_LEVEL_INFO,
                                              LOG_LEVEL_WARN,    LOG_LEVEL_ERROR, LOG_LEVEL_ERROR };
  static constexpr LogLevel PYTHON_LEVELS[] = { LOG_LEVEL_UNKNOWN, LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_ERROR, LOG_LEVEL_ERROR };
  static constexpr LogLevel LOGBACK_LEVELS[] = { LOG_LEVEL_UNKNOWN, LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_ERROR };

  switch (encoding) {
  case LEVEL_SYSLOG:
    return value < sizeof(SYSLOG_LEVELS) / sizeof(*SYSLOG_LEVELS) ? SYSLOG_LEVELS[value] : LOG_LEVEL_UNKNOWN;
  case LEVEL_PINO:
    return !(value % 10) && value / 10 < sizeof(PINO_LEVELS) / sizeof(*PINO_LEVELS) ? PINO_LEVELS[value / 10] : LOG_LEVEL_UNKNOWN;
  case LEVEL_PYTHON:
    return !(value % 10) && value / 10 < sizeof(PYTHON_LEVELS) / sizeof(*PYTHON_LEVELS) ? PYTHON_LEVELS[value / 10] : LOG_LEVEL_UNKNOWN;
  case LEVEL_LOGBACK:
    if (value == 5000) return LOG_LEVEL_DEBUG;

    return !(value % 10000) && value / 10000 < sizeof(LOGBACK_LEVELS) / sizeof(*LOGBACK_LEVELS) ? LOGBACK_LEVELS[value / 10000] : LOG_LEVEL_UNKNOWN;
  case LEVEL_OTEL:
    if (!value || value > 24) return LOG_LEVEL_UNKNOWN;
    if (value <= 8) return LOG_LEVEL_DEBUG;
    if (value <= 12) return LOG_LEVEL_INFO;
    if (value <= 16) return LOG_LEVEL_WARN;

    return LOG_LEVEL_ERROR;
  default:
    return LOG_LEVEL_UNKNOWN;
  }
}

static bool structured_level(JsonWorkspace *workspace, LogLevel *level) {
  static const struct {
    const char *field;
    LevelEncoding encoding;
  } fields[] = { { "level", LEVEL_PINO },         { "severity", LEVEL_TEXT },      { "log.level", LEVEL_PINO },      { "log_level", LEVEL_TEXT },
                 { "logLevel", LEVEL_TEXT },      { "lvl", LEVEL_TEXT },           { "log.severity", LEVEL_TEXT },   { "levelname", LEVEL_TEXT },
                 { "level_name", LEVEL_TEXT },    { "severity_text", LEVEL_TEXT }, { "severityText", LEVEL_TEXT },   { "@l", LEVEL_TEXT },
                 { "PRIORITY", LEVEL_SYSLOG },    { "levelno", LEVEL_PYTHON },     { "level_value", LEVEL_LOGBACK }, { "severity_number", LEVEL_OTEL },
                 { "severityNumber", LEVEL_OTEL } };

  for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
    JsonFieldSpan field;
    JsonFieldResult found = json_workspace_log_field(workspace, fields[i].field, &field);

    if (found == JSON_FIELD_ERROR) return false;
    if (found != JSON_FIELD_FOUND) continue;

    *level = named_level(field.text, field.length);
    if (*level == LOG_LEVEL_UNKNOWN) *level = numeric_level(&field, fields[i].encoding);
    if (*level != LOG_LEVEL_UNKNOWN) break;
  }

  return true;
}

static bool iso_date_prefix(const char *text) {
  for (size_t i = 0; i < 10; i++) {
    if (i == 4 || i == 7) {
      if (text[i] != '-') return false;
    } else if (!isdigit((unsigned char)text[i])) return false;
  }

  return true;
}

static LogLevel plain_level(const char *text) {
  const char *start = text;
  size_t scanned = 0;

  /* Read logfmt level assignments without mistaking words inside quoted messages. */
  while (*start && scanned < 256) {
    while (isspace((unsigned char)*start)) {
      start++;
      scanned++;
    }

    if (!*start) break;

    const char *key = start;
    while (isalnum((unsigned char)*start) || *start == '_' || *start == '.') start++;
    size_t length = (size_t)(start - key);
    bool assignment = *start == '=' || *start == ':';

    if (assignment && (text_ascii_equal(key, length, "message") || text_ascii_equal(key, length, "msg"))) break;

    bool is_level =
        assignment && (text_ascii_equal(key, length, "level") || text_ascii_equal(key, length, "severity") || text_ascii_equal(key, length, "log.level"));
    if (assignment) {
      start++;
      while (isspace((unsigned char)*start)) start++;
    }

    char quote = (*start == '"' || *start == '\'') ? *start++ : 0;
    const char *value = start;

    while (*start && (quote ? *start != quote : !isspace((unsigned char)*start))) {
      if (quote && *start == '\\' && start[1]) start++;
      start++;
    }

    if (is_level) {
      LogLevel level = named_level(value, (size_t)(start - value));

      if (level != LOG_LEVEL_UNKNOWN) return level;
    }
    if (quote && *start) start++;
    if (start == key) start++;

    scanned = (size_t)(start - text);
  }

  /* Stop at the message rather than searching its words for a severity. */
  start = text;

  if (iso_date_prefix(start)) {
    while (*start && !isspace((unsigned char)*start)) start++;
    while (isspace((unsigned char)*start)) start++;

    if (isdigit((unsigned char)*start)) {
      while (*start && !isspace((unsigned char)*start)) start++;
    }
  }

  while (*start && (size_t)(start - text) < 256) {
    while (isspace((unsigned char)*start)) start++;

    if (*start == '[') {
      const char *end = strchr(start + 1, ']');

      if (!end || (size_t)(end - text) >= 256) break;

      const char *word = start + 1;
      while (word < end && isspace((unsigned char)*word)) word++;

      size_t length = (size_t)(end - word);
      while (length && isspace((unsigned char)word[length - 1])) length--;

      LogLevel level = named_level(word, length);
      if (level != LOG_LEVEL_UNKNOWN) return level;

      start = end + 1;
      continue;
    }

    const char *word = start;
    while (isalpha((unsigned char)*start)) start++;

    if (!*start || isspace((unsigned char)*start) || *start == ':') return named_level(word, (size_t)(start - word));

    break;
  }

  return LOG_LEVEL_UNKNOWN;
}

bool log_level_detect(const char *text, size_t length, JsonWorkspace *workspace, LogLevel *level) {
  *level = LOG_LEVEL_UNKNOWN;
  workspace->root = nullptr;

  while (length && isspace((unsigned char)*text)) {
    text++;
    length--;
  }

  if (*text != '{' && *text != '[') {
    *level = plain_level(text);
    return true;
  }

  /* Bracketed plain prefixes are not JSON arrays. */
  if (*text == '[' && isalpha((unsigned char)text[1])) {
    *level = plain_level(text);
    return true;
  }

  JsonFieldResult parsed = json_workspace_parse(text, length, workspace);

  if (parsed == JSON_FIELD_ERROR) return false;

  if (parsed == JSON_FIELD_MISSING && *text == '[') *level = plain_level(text);

  return parsed != JSON_FIELD_FOUND || structured_level(workspace, level);
}
