# ===== Project =====
BIN    := log-viewer
MODE   ?= debug
OBJDIR := build/$(MODE)

ifneq ($(filter $(MODE),debug release),$(MODE))
  $(error Unsupported MODE '$(MODE)'; use debug or release)
endif

# ===== Toolchain =====
CC           := gcc
BEAR         ?= bear
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy
NCURSES_LIBS ?= -lncursesw
SYSTEM       := $(shell uname -s)

ifeq ($(SYSTEM),Darwin)
  CC := gcc-15
  NCURSES_PREFIX ?= $(shell brew --prefix ncurses 2>/dev/null)
  CPPFLAGS += -D_DARWIN_C_SOURCE
  ifneq ($(strip $(NCURSES_PREFIX)),)
    CPPFLAGS += -I$(NCURSES_PREFIX)/include
    LDFLAGS += -L$(NCURSES_PREFIX)/lib
  endif
endif

CPPFLAGS += -Iinclude -D_GNU_SOURCE -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -D_XOPEN_SOURCE=700
CFLAGS   += -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -MMD -MP -pthread
LDLIBS   += $(NCURSES_LIBS) -pthread

ifeq ($(MODE),release)
  CFLAGS += -O2
else
  CFLAGS += -O0 -g
endif

# ===== Sources / Objects =====
SRC  := $(sort $(shell find src -type f -name '*.c'))
OBJ  := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))
CORE_SRC := src/log_level.c src/json_support.c src/log_store.c src/query.c src/search.c src/source.c src/source_set.c src/source_tree.c src/text.c
CORE_OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(CORE_SRC))
TEST_OBJ := $(OBJDIR)/tests/core_test.o
TEST_BIN := $(OBJDIR)/core-test
DEPS := $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)

# Preserve the bundled third-party JSON header's upstream formatting.
FORMAT_FILES := $(sort $(SRC) $(filter-out include/json.h,$(shell find include -type f -name '*.h')) $(shell find tests benchmarks -type f -name '*.c'))

# ===== Targets =====
.DEFAULT_GOAL := all
.PHONY: all clean run test tui-test bear format format-check tidy help FORCE benchmark sanitize memcheck threadcheck
all: $(BIN)

# Relink so switching MODE always selects the corresponding objects.
$(BIN): $(OBJ) FORCE
	$(CC) $(LDFLAGS) $(OBJ) -o $@ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

FORCE:

run: $(BIN)
	./$(BIN) $(ARGS)

$(OBJDIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Core modules intentionally link without ncurses.
$(TEST_BIN): $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ -pthread

$(OBJDIR)/bench: benchmarks/bench.c $(CORE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE_OBJ) -o $@ $(LDFLAGS) -pthread

benchmark:
	$(MAKE) MODE=release build/release/bench
	python3 benchmarks/run.py $(BENCH_ARGS)

test: $(BIN) $(TEST_BIN)
	./$(TEST_BIN)
	python3 tests/test.py
	python3 tests/release_test.py

# Optional terminal interaction checks; excluded from release gates.
tui-test: $(BIN)
	python3 tests/tui_test.py

# Override ASAN_OPTIONS=detect_leaks=0 when LeakSanitizer is unavailable under ptrace.
ASAN_OPTIONS ?= detect_leaks=1
sanitize:
	@mkdir -p build/sanitize
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined $(SRC) -o build/sanitize/log-viewer $(LDFLAGS) $(LDLIBS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined tests/core_test.c $(CORE_SRC) -o build/sanitize/core-test $(LDFLAGS) -pthread
	ASAN_OPTIONS="$(ASAN_OPTIONS)" UBSAN_OPTIONS=halt_on_error=1 ./build/sanitize/core-test
	ASAN_OPTIONS="$(ASAN_OPTIONS)" UBSAN_OPTIONS=halt_on_error=1 LOG_VIEWER_BIN=build/sanitize/log-viewer python3 tests/test.py

threadcheck:
	@mkdir -p build/threadcheck
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=thread tests/core_test.c $(CORE_SRC) -o build/threadcheck/core-test $(LDFLAGS) -pthread
	TSAN_OPTIONS=halt_on_error=1 ./build/threadcheck/core-test

WRAP_ALLOC := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
memcheck:
	@mkdir -p build/memcheck
	$(CC) $(CPPFLAGS) $(CFLAGS) -fno-builtin tests/core_test.c $(CORE_SRC) tests/allocation_tracker.c $(WRAP_ALLOC) -o build/memcheck/core-test $(LDFLAGS) -pthread
	$(CC) $(CPPFLAGS) $(CFLAGS) -fno-builtin $(SRC) tests/allocation_tracker.c $(WRAP_ALLOC) -o build/memcheck/log-viewer $(LDFLAGS) $(LDLIBS)
	./build/memcheck/core-test
	LOG_VIEWER_BIN=build/memcheck/log-viewer python3 tests/test.py
	python3 tests/failures.py build/memcheck/log-viewer

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# Build first, then capture every compiler command without process interception.
# Requires Bear with the parse-sh subcommand.
bear:
	$(MAKE) -B all MODE=$(MODE)
	$(MAKE) --no-print-directory -Bn all MODE=$(MODE) | $(BEAR) parse-sh

# Regenerate the compilation database before analyzing project sources.
tidy: bear
	$(CLANG_TIDY) -p . $(SRC)

clean:
	rm -rf build $(BIN) compile_commands.json

help:
	@echo "make [MODE=debug|release]       # build (default: debug)"
	@echo "make run [ARGS=...]             # run the viewer"
	@echo "make test                      # run core and headless integration tests"
	@echo "make tui-test                  # run optional terminal interaction tests"
	@echo "make format                    # format project C sources and headers"
	@echo "make format-check              # verify project C formatting"
	@echo "make bear                      # regenerate compile_commands.json"
	@echo "make tidy                      # generate database and run clang-tidy"
	@echo "make benchmark [BENCH_ARGS=...] # run 1M/10M performance harness"
	@echo "make sanitize                  # ASan/UBSan and integration tests"
	@echo "make threadcheck               # ThreadSanitizer on concurrent search tests"
	@echo "make memcheck                  # verify application allocation balance"
	@echo "make clean                     # remove builds and compilation database"

-include $(DEPS)
