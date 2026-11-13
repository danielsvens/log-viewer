#pragma once

#include <stddef.h>
#include "log_viewer/core/json_support.h"

typedef enum { LOG_LEVEL_ERROR, LOG_LEVEL_WARN, LOG_LEVEL_INFO, LOG_LEVEL_DEBUG, LOG_LEVEL_UNKNOWN, LOG_LEVEL_COUNT } LogLevel;

typedef unsigned int LogLevelMask;

constexpr LogLevelMask LOG_LEVELS_ALL = (1u << (unsigned int)LOG_LEVEL_COUNT) - 1u;

const char *log_level_name(LogLevel level);
bool log_levels_parse(const char *text, LogLevelMask *mask);
/* Input is normalized and NUL-terminated. False means an allocation failure. */
bool log_level_detect(const char *text, size_t length, JsonWorkspace *workspace, LogLevel *level);
