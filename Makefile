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

CPPFLAGS += -Iinclude -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
CFLAGS   += -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -MMD -MP
LDLIBS   += -lncursesw

ifeq ($(MODE),release)
  CFLAGS += -O2
else
  CFLAGS += -O0 -g
endif

# ===== Sources / Objects =====
SRC  := $(sort $(shell find src -type f -name '*.c'))
OBJ  := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))
CORE_OBJ := $(addprefix $(OBJDIR)/,json_support.o log_store.o query.o source.o text.o)
TEST_OBJ := $(OBJDIR)/tests/core_test.o
TEST_BIN := $(OBJDIR)/core-test
DEPS := $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)

# Preserve the bundled third-party JSON header's upstream formatting.
FORMAT_FILES := $(sort $(SRC) $(filter-out include/json.h,$(shell find include -type f -name '*.h')) tests/core_test.c)

# ===== Targets =====
.DEFAULT_GOAL := all
.PHONY: all clean run test bear format format-check tidy help FORCE
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
	$(CC) $(LDFLAGS) $^ -o $@

test: $(BIN) $(TEST_BIN)
	./$(TEST_BIN)
	python3 tests/test.py

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
	@echo "make test                      # run integration tests"
	@echo "make format                    # format project C sources and headers"
	@echo "make format-check              # verify project C formatting"
	@echo "make bear                      # regenerate compile_commands.json"
	@echo "make tidy                      # generate database and run clang-tidy"
	@echo "make clean                     # remove builds and compilation database"

-include $(DEPS)
