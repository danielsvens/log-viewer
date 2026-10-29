/* Linker wrappers count application-owned allocations, including JSON workspaces.
 * Allocations internal to shared libraries are outside this check. */
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);

static atomic_long outstanding, calls;
static long fail_on;

__attribute__((constructor)) static void configure_failures(void) {
  const char *value = getenv("LOG_VIEWER_FAIL_ALLOCATION");

  if (value) fail_on = strtol(value, nullptr, 10);
}

static bool fail_allocation(void) {
  if (atomic_fetch_add(&calls, 1) + 1 != fail_on) return false;

  errno = ENOMEM;

  return true;
}

void *__wrap_malloc(size_t size) {
  if (fail_allocation()) return nullptr;

  void *pointer = __real_malloc(size);

  if (pointer) atomic_fetch_add(&outstanding, 1);

  return pointer;
}

void *__wrap_calloc(size_t count, size_t size) {
  if (fail_allocation()) return nullptr;

  void *pointer = __real_calloc(count, size);

  if (pointer) atomic_fetch_add(&outstanding, 1);

  return pointer;
}

void *__wrap_realloc(void *pointer, size_t size) {
  if (fail_allocation()) return nullptr;

  void *replacement = __real_realloc(pointer, size);

  if (!pointer && replacement) atomic_fetch_add(&outstanding, 1);
  else if (pointer && !size && !replacement) atomic_fetch_sub(&outstanding, 1);

  return replacement;
}

void __wrap_free(void *pointer) {
  if (pointer) atomic_fetch_sub(&outstanding, 1);

  __real_free(pointer);
}

__attribute__((destructor)) static void check_allocations(void) {
  long remaining = atomic_load(&outstanding);

  if (remaining) {
    fprintf(stderr, "Unreleased application allocations: %ld\n", remaining);
    _Exit(1);
  }
}
