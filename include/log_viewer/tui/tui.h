#pragma once

#include "log_viewer/core/query.h"
#include "log_viewer/core/search.h"

#include <signal.h>

/* Borrows search and query; the caller owns signal handling and cancellation. */
int tui_run(Search *search, Query *query, const volatile sig_atomic_t *interrupted);
