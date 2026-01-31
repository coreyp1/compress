SUITE := ghoti.io
PROJECT := compress

BUILD ?= release
BRANCH := -dev
# If BUILD is debug, append -debug
ifeq ($(BUILD),debug)
    BRANCH := $(BRANCH)-debug
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
STATIC_TARGET := $(BASE_NAME_PREFIX).a
ENV_VARS :=

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	# Additional Linux-specific variables
	PKG_CONFIG_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	BUILD := linux/$(BUILD)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	LIB_EXTENSION := dylib
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	# Additional macOS-specific variables
	BUILD := mac/$(BUILD)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PKG_CONFIG_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	BUILD := win32/$(BUILD)

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PKG_CONFIG_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))

endif


CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 -O1 -g
CC := cc
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c17 -O0 -g
# Library-specific compile flags (export symbols on Windows, PIC on Linux)
# GCOMP_BUILD enables DLL export on Windows (checked by GCOMP_API macro)
# GCOMP_TEST_BUILD enables export of internal functions for testing (checked by GCOMP_INTERNAL_API macro)
LIB_CFLAGS := $(CFLAGS) -DGCOMP_BUILD -DGCOMP_TEST_BUILD
LDFLAGS := -L /usr/lib -lstdc++ -lm
BUILD_DIR := ./build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/objects
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps


# Add OS-specific flags
ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC

else ifeq ($(UNAME_S), Darwin)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows

else
	$(error Unsupported OS: $(UNAME_S))

endif

# The standard include directories for the project.
INCLUDE := -I include/ -I $(GEN_DIR)/

# Additional include directories for tests (common helpers, method-specific data)
TEST_INCLUDE := $(INCLUDE) -I tests/common/ -I tests/methods/deflate/

# Automatically collect all .c source files under the src directory.
SOURCES := $(shell find src -type f -name '*.c')

# Convert each source file path to an object file path.
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))


TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs --cflags gtest`

# Valgrind flags (exclude "still reachable" as it's not a leak)
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1

# Test helper object file
TEST_HELPER_OBJ := $(OBJ_DIR)/tests/common/test_helpers.o

COMPRESSLIBRARY := -L $(APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)

# Automatically discover all test source files (excluding test_helpers.cpp)
TEST_SOURCES := $(filter-out tests/common/test_helpers.cpp, $(shell find tests -type f -name 'test*.cpp' 2>/dev/null))

# Function to convert test source file to executable name
# test.cpp -> testCompress, test_*.cpp -> test* (capitalized first letter, underscores removed)
test-name = $(if $(filter tests/test.cpp,$1),testCompress,$(shell echo $(basename $(notdir $1)) | sed 's/test_/test/; s/^test\([a-z]\)/test\U\1/'))

# Generate list of test executables
TEST_EXECUTABLES := $(foreach test,$(TEST_SOURCES),$(APP_DIR)/$(call test-name,$(test))$(EXE_EXTENSION))

# Automatically collect all example .c files under examples directories.
EXAMPLE_SOURCES := $(shell find examples -type f -name '*.c' 2>/dev/null)

# Convert each example source file path to an executable path.
EXAMPLES := $(patsubst examples/%.c,$(APP_DIR)/examples/%$(EXE_EXTENSION),$(EXAMPLE_SOURCES))

# Automatically collect all benchmark .c files under bench directories.
BENCH_SOURCES := $(shell find bench -type f -name '*.c' 2>/dev/null)

# Convert each benchmark source file path to an executable path.
BENCHMARKS := $(patsubst bench/%.c,$(APP_DIR)/bench/%$(EXE_EXTENSION),$(BENCH_SOURCES))


all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build shared + static libraries

####################################################################
# Dependency Inclusion
####################################################################

# Automatically include all generated dependency files.
-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/**/*.d)
-include $(wildcard $(OBJ_DIR)/tests/*.d)
-include $(wildcard $(APP_DIR)/test*.d)


####################################################################
# Object Files
####################################################################

# Pattern rule for C source files: compile .c files to .o files, generating dependency files.
$(OBJ_DIR)/%.o: src/%.c
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for C++ source files (if any):
$(OBJ_DIR)/%.o: src/%.cpp
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for test helper C++ files:
$(OBJ_DIR)/tests/%.o: tests/%.cpp
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@


####################################################################
# Shared Library
####################################################################

$(APP_DIR)/$(TARGET): \
		$(LIBOBJECTS)
	@printf "\n### Compiling Compress Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)

ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

####################################################################
# Static Library
####################################################################

$(APP_DIR)/$(STATIC_TARGET): \
		$(LIBOBJECTS)
	@printf "\n### Archiving Compress Static Library ###\n"
	@mkdir -p $(@D)
	ar rcs $@ $^

####################################################################
# Unit Tests
####################################################################

# Test helper object (compiled once, linked into all tests)
$(TEST_HELPER_OBJ): tests/common/test_helpers.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for compiling test source files to object files
# This allows tests to be compiled separately from linking
$(OBJ_DIR)/tests/%.o: tests/%.cpp
	@printf "\n### Compiling Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for building test executables
# This automatically handles all test_*.cpp files
# Tests are compiled to .o files first, then linked separately
# This optimization allows tests to only relink (fast) when library changes but headers don't
define test-executable-rule
# Generate test object file path from test source path
# e.g., tests/core/test_callback_api.cpp -> build/linux/release/objects/tests/core/test_callback_api.o
# The object file is built by the pattern rule $(OBJ_DIR)/tests/%.o: tests/%.cpp above
TEST_OBJ_$1 := $(OBJ_DIR)/tests/$(patsubst tests/%.cpp,%.o,$1)

# Rule to link test object file into executable
$(APP_DIR)/$(call test-name,$1)$(EXE_EXTENSION): \
		$$(TEST_OBJ_$1) \
		$(TEST_HELPER_OBJ) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Linking %s Test ###\n" "$(call test-name,$1)"
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) -o $$@ $$(TEST_OBJ_$1) $$(TEST_HELPER_OBJ) $$(LDFLAGS) $$(TESTFLAGS) $$(COMPRESSLIBRARY)
endef

# Generate build rules for all test sources
$(foreach test,$(TEST_SOURCES),$(eval $(call test-executable-rule,$(test))))

####################################################################
# Examples
####################################################################

# Pattern rule for example executables
$(APP_DIR)/examples/%$(EXE_EXTENSION): examples/%.c $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(COMPRESSLIBRARY)

####################################################################
# Benchmarks
####################################################################

# Pattern rule for benchmark executables
$(APP_DIR)/bench/%$(EXE_EXTENSION): bench/%.c $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Benchmark: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(COMPRESSLIBRARY)

####################################################################
# Fuzz Testing (AFL++)
####################################################################

# Fuzz harness source files
FUZZ_SOURCES := $(shell find fuzz -maxdepth 1 -type f -name 'fuzz_*.c' 2>/dev/null)

# Fuzz executables (built with afl-gcc)
FUZZ_EXECUTABLES := $(patsubst fuzz/%.c,$(APP_DIR)/fuzz/%$(EXE_EXTENSION),$(FUZZ_SOURCES))

# AFL compiler (can be overridden: make fuzz-build AFL_CC=afl-clang-fast)
AFL_CC ?= afl-gcc
AFL_CFLAGS := -O2 -g $(INCLUDE)

# AFL environment variables for running fuzzer
# Set AFL_SKIP_CRASHES=1 to skip core_pattern check (for WSL2/testing)
# For proper crash detection: echo core | sudo tee /proc/sys/kernel/core_pattern
AFL_ENV ?=

# AFL-instrumented object files and library (separate from regular build)
AFL_OBJ_DIR := $(BUILD_DIR)/afl-objects
AFL_LIBOBJECTS := $(patsubst src/%.c,$(AFL_OBJ_DIR)/%.o,$(SOURCES))
AFL_STATIC_TARGET := $(BASE_NAME_PREFIX)-afl.a

# Pattern rule for AFL-instrumented object files
$(AFL_OBJ_DIR)/%.o: src/%.c
	@printf "\n### Compiling (AFL instrumented): $< ###\n"
	@mkdir -p $(@D)
	$(AFL_CC) $(AFL_CFLAGS) -c $< -o $@

# AFL-instrumented static library
$(APP_DIR)/$(AFL_STATIC_TARGET): $(AFL_LIBOBJECTS)
	@printf "\n### Archiving AFL-instrumented Static Library ###\n"
	@mkdir -p $(@D)
	ar rcs $@ $^

# Corpus generator (built with regular gcc, no AFL instrumentation)
$(APP_DIR)/fuzz/generate_corpus$(EXE_EXTENSION): fuzz/generate_corpus.c
	@printf "\n### Compiling Corpus Generator ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $<

# Pattern rule for fuzz executables (linked against AFL-instrumented library)
$(APP_DIR)/fuzz/%$(EXE_EXTENSION): fuzz/%.c $(APP_DIR)/$(AFL_STATIC_TARGET)
	@printf "\n### Compiling Fuzz Harness: $* ###\n"
	@mkdir -p $(@D)
	$(AFL_CC) $(AFL_CFLAGS) -o $@ $< $(APP_DIR)/$(AFL_STATIC_TARGET) -lm

####################################################################
# Commands
####################################################################

# General commands
.PHONY: clean cloc docs docs-pdf examples bench bench-deflate
# Release build commands
.PHONY: all install test test-quiet test-valgrind test-valgrind-quiet test-watch uninstall watch
# Debug build commands
.PHONY: all-debug install-debug test-debug test-valgrind-debug test-watch-debug uninstall-debug watch-debug
# Fuzz commands
.PHONY: fuzz-build fuzz-corpus fuzz-decoder fuzz-encoder fuzz-roundtrip fuzz-help
.PHONY: fuzz-gzip-decoder fuzz-gzip-encoder fuzz-gzip-roundtrip
# Sanitizer commands
.PHONY: test-asan test-asan-quiet test-ubsan sanitizer-help


watch: ## Watch the file directory for changes and compile the target
	@while true; do \
		make --no-print-directory all; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

test-watch: ## Watch the file directory for changes and run the unit tests
	@while true; do \
		make --no-print-directory all; \
		make --no-print-directory test; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

examples: ## Build all examples
examples: $(APP_DIR)/$(TARGET) $(EXAMPLES)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Examples built       ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@printf "Examples are available in: $(APP_DIR)/examples/\n"
	@printf "\n"
	@printf "\033[0;33mTo run examples:\033[0m\n"
ifeq ($(OS_NAME), Linux)
	@printf "  Linux: Set LD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export LD_LIBRARY_PATH=\"$(APP_DIR):$$LD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/<example>\n"
else ifeq ($(OS_NAME), Mac)
	@printf "  macOS: Set DYLD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export DYLD_LIBRARY_PATH=\"$(APP_DIR):$$DYLD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/<example>\n"
else ifeq ($(OS_NAME), Windows)
	@printf "  Windows (MSYS2): The DLL must be in the same directory or in PATH.\n"
	@printf "  Option 1 - Run from the library directory:\n"
	@printf "    cd $(APP_DIR)\n"
	@printf "    ./examples/<example>$(EXE_EXTENSION)\n"
	@printf "  Option 2 - Add library directory to PATH:\n"
	@printf "    export PATH=\"$(APP_DIR):$$PATH\"\n"
	@printf "    $(APP_DIR)/examples/<example>$(EXE_EXTENSION)\n"
	@printf "  Option 3 - Copy DLL to example directories:\n"
	@printf "    cp $(APP_DIR)/$(TARGET) $(APP_DIR)/examples/\n"
	@printf "    Then run: $(APP_DIR)/examples/<example>$(EXE_EXTENSION)\n"
endif
	@printf "\n"

bench: ## Build all benchmarks
bench: $(APP_DIR)/$(TARGET) $(BENCHMARKS)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Benchmarks built     ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@printf "Benchmarks are available in: $(APP_DIR)/bench/\n"
	@printf "\n"
	@printf "\033[0;33mTo run benchmarks:\033[0m\n"
ifeq ($(OS_NAME), Linux)
	@printf "  Linux: Set LD_LIBRARY_PATH and run:\n"
	@printf "    LD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_deflate\n"
else ifeq ($(OS_NAME), Mac)
	@printf "  macOS: Set DYLD_LIBRARY_PATH and run:\n"
	@printf "    DYLD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_deflate\n"
else ifeq ($(OS_NAME), Windows)
	@printf "  Windows (MSYS2): Run from library directory:\n"
	@printf "    cd $(APP_DIR) && ./bench/bench_deflate$(EXE_EXTENSION)\n"
endif
	@printf "\n"

bench-deflate: ## Build and run the deflate benchmark
bench-deflate: $(APP_DIR)/$(TARGET) $(APP_DIR)/bench/bench_deflate$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Running Deflate Benchmark ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@LD_LIBRARY_PATH="$(APP_DIR)" $(APP_DIR)/bench/bench_deflate$(EXE_EXTENSION)

fuzz-help: ## Show fuzzing help and instructions
	@printf "\033[0;36m"
	@printf "####################################\n"
	@printf "### Fuzz Testing with AFL++      ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@printf "Prerequisites:\n"
	@printf "  sudo apt install afl++\n"
	@printf "\n"
	@printf "Available targets:\n"
	@printf "  make fuzz-build           - Build library and harnesses with AFL instrumentation\n"
	@printf "  make fuzz-corpus          - Generate seed corpus from test vectors\n"
	@printf "\n"
	@printf "  Deflate fuzzers:\n"
	@printf "    make fuzz-decoder       - Run deflate decoder fuzzer\n"
	@printf "    make fuzz-encoder       - Run deflate encoder fuzzer\n"
	@printf "    make fuzz-roundtrip     - Run deflate roundtrip fuzzer\n"
	@printf "\n"
	@printf "  Gzip fuzzers:\n"
	@printf "    make fuzz-gzip-decoder  - Run gzip decoder fuzzer\n"
	@printf "    make fuzz-gzip-encoder  - Run gzip encoder fuzzer\n"
	@printf "    make fuzz-gzip-roundtrip- Run gzip roundtrip fuzzer\n"
	@printf "\n"
	@printf "Workflow:\n"
	@printf "  1. make fuzz-corpus        # Generate seed inputs\n"
	@printf "  2. make fuzz-build         # Build library + harnesses with AFL\n"
	@printf "  3. make fuzz-gzip-decoder  # Start fuzzing (Ctrl+C to stop)\n"
	@printf "\n"
	@printf "Core pattern setup (for crash detection):\n"
	@printf "  echo core | sudo tee /proc/sys/kernel/core_pattern\n"
	@printf "\n"
	@printf "Or skip the check (for quick testing on WSL2):\n"
	@printf "  make fuzz-decoder AFL_ENV='AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1'\n"
	@printf "\n"
	@printf "For faster fuzzing (if LLVM is installed):\n"
	@printf "  make fuzz-build AFL_CC=afl-clang-fast\n"
	@printf "\n"
	@printf "Findings are saved to: fuzz/findings/<target>/\n"
	@printf "  - crashes/  : Inputs that caused crashes\n"
	@printf "  - hangs/    : Inputs that caused timeouts\n"
	@printf "  - queue/    : Interesting inputs for coverage\n"
	@printf "\n"
	@printf "Documentation: documentation/testing/fuzzing.md\n"
	@printf "\n"

fuzz-build: ## Build all fuzz harnesses with AFL instrumentation
fuzz-build: $(FUZZ_EXECUTABLES)
	@printf "\033[0;32m\n"
	@printf "###########################################\n"
	@printf "### Fuzz harnesses built (instrumented) ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@printf "AFL-instrumented library: $(APP_DIR)/$(AFL_STATIC_TARGET)\n"
	@printf "Harnesses are available in: $(APP_DIR)/fuzz/\n"
	@for exe in $(FUZZ_EXECUTABLES); do \
		printf "  - %s\n" "$$exe"; \
	done
	@printf "\n"
	@printf "Run 'make fuzz-help' for usage instructions.\n"
	@printf "\n"

fuzz-corpus: ## Generate seed corpus from test vectors
fuzz-corpus: $(APP_DIR)/fuzz/generate_corpus$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Generating Seed Corpus       ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@$(APP_DIR)/fuzz/generate_corpus$(EXE_EXTENSION)

fuzz-decoder: ## Run decoder fuzzer (Ctrl+C to stop)
fuzz-decoder: $(APP_DIR)/fuzz/fuzz_deflate_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Running Decoder Fuzzer       ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/decoder
	@if [ ! -d fuzz/corpus/decoder ] || [ -z "$$(ls -A fuzz/corpus/decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/decoder; \
		printf '\x01\x00\x00\xff\xff' > fuzz/corpus/decoder/empty.bin; \
	fi
	$(AFL_ENV) afl-fuzz -i fuzz/corpus/decoder -o fuzz/findings/decoder -- $(APP_DIR)/fuzz/fuzz_deflate_decoder$(EXE_EXTENSION)

fuzz-encoder: ## Run encoder fuzzer (Ctrl+C to stop)
fuzz-encoder: $(APP_DIR)/fuzz/fuzz_deflate_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Running Encoder Fuzzer       ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/encoder
	@if [ ! -d fuzz/corpus/encoder ] || [ -z "$$(ls -A fuzz/corpus/encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/encoder; \
		printf 'Hello' > fuzz/corpus/encoder/hello.bin; \
	fi
	$(AFL_ENV) afl-fuzz -i fuzz/corpus/encoder -o fuzz/findings/encoder -- $(APP_DIR)/fuzz/fuzz_deflate_encoder$(EXE_EXTENSION)

fuzz-roundtrip: ## Run roundtrip fuzzer (Ctrl+C to stop)
fuzz-roundtrip: $(APP_DIR)/fuzz/fuzz_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Running Roundtrip Fuzzer     ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/roundtrip
	@if [ ! -d fuzz/corpus/roundtrip ] || [ -z "$$(ls -A fuzz/corpus/roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/roundtrip; \
		printf 'Hello' > fuzz/corpus/roundtrip/hello.bin; \
	fi
	$(AFL_ENV) afl-fuzz -i fuzz/corpus/roundtrip -o fuzz/findings/roundtrip -- $(APP_DIR)/fuzz/fuzz_roundtrip$(EXE_EXTENSION)

fuzz-gzip-decoder: ## Run gzip decoder fuzzer (Ctrl+C to stop)
fuzz-gzip-decoder: $(APP_DIR)/fuzz/fuzz_gzip_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Gzip Decoder Fuzzer     ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/gzip_decoder
	@if [ ! -d fuzz/corpus/gzip_decoder ] || [ -z "$$(ls -A fuzz/corpus/gzip_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/gzip_decoder; \
		printf '\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00' > fuzz/corpus/gzip_decoder/empty.gz; \
	fi
	$(AFL_ENV) afl-fuzz -i fuzz/corpus/gzip_decoder -o fuzz/findings/gzip_decoder -- $(APP_DIR)/fuzz/fuzz_gzip_decoder$(EXE_EXTENSION)

fuzz-gzip-encoder: ## Run gzip encoder fuzzer (Ctrl+C to stop)
fuzz-gzip-encoder: $(APP_DIR)/fuzz/fuzz_gzip_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Gzip Encoder Fuzzer     ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/gzip_encoder
	@if [ ! -d fuzz/corpus/gzip_encoder ] || [ -z "$$(ls -A fuzz/corpus/gzip_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/gzip_encoder; \
		printf 'Hello' > fuzz/corpus/gzip_encoder/hello.bin; \
	fi
	$(AFL_ENV) afl-fuzz -i fuzz/corpus/gzip_encoder -o fuzz/findings/gzip_encoder -- $(APP_DIR)/fuzz/fuzz_gzip_encoder$(EXE_EXTENSION)

fuzz-gzip-roundtrip: ## Run gzip roundtrip fuzzer (Ctrl+C to stop)
fuzz-gzip-roundtrip: $(APP_DIR)/fuzz/fuzz_gzip_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Gzip Roundtrip Fuzzer   ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/gzip_roundtrip
	@if [ ! -d fuzz/corpus/gzip_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/gzip_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/gzip_roundtrip; \
		printf 'Hello' > fuzz/corpus/gzip_roundtrip/hello.bin; \
	fi
	$(AFL_ENV) afl-fuzz -i fuzz/corpus/gzip_roundtrip -o fuzz/findings/gzip_roundtrip -- $(APP_DIR)/fuzz/fuzz_gzip_roundtrip$(EXE_EXTENSION)

test: ## Make and run the Unit tests
test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
	@for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION) | sed 's/test/\u&/'); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		LD_LIBRARY_PATH="$(APP_DIR)" $$test_exe --gtest_brief=1; \
	done

test-quiet: ## Run tests with minimal output (one line per test suite)
test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;36m%-30s %8s %10s %s\033[0m\n" "Test Suite" "Tests" "Time" "Status"; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(APP_DIR)" $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi

test-valgrind: ## Run all tests under valgrind (Linux only)
test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION) | sed 's/test/\u&/'); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests under Valgrind ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		LD_LIBRARY_PATH="$(APP_DIR)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1; \
	done
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-valgrind-quiet: ## Run tests under valgrind with minimal output (Linux only)
test-valgrind-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;35m%-30s %8s %10s %s\033[0m\n" "Test Suite (Valgrind)" "Tests" "Time" "Status"; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(APP_DIR)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		has_leak=$$(echo "$$output" | grep -c "definitely lost\|indirectly lost\|possibly lost" || true); \
		if [ $$exit_code -eq 0 ] && [ $$has_leak -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=0; \
			if [ $$has_leak -gt 0 ]; then \
				status_msg="LEAK"; \
			else \
				status_msg="FAIL"; \
			fi; \
			total_failed=$$((total_failed + 1)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31m%s\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms" "$$status_msg"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d suites)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

####################################################################
# Sanitizer Builds (ASan, UBSan)
####################################################################

# Sanitizer flags
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer -g
UBSAN_FLAGS := -fsanitize=undefined -fno-omit-frame-pointer -g
# Combined sanitizer flags (ASan + UBSan work well together)
ASAN_UBSAN_FLAGS := $(ASAN_FLAGS) -fsanitize=undefined

# Sanitizer-specific build directories
ASAN_BUILD_DIR := ./build/$(BUILD)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps

# ASan-instrumented object files
ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES))
ASAN_TARGET := $(BASE_NAME_PREFIX)-asan.so
ASAN_STATIC_TARGET := $(BASE_NAME_PREFIX)-asan.a

# ASan test helper and executables
ASAN_TEST_HELPER_OBJ := $(ASAN_OBJ_DIR)/tests/common/test_helpers.o
ASAN_TEST_EXECUTABLES := $(foreach test,$(TEST_SOURCES),$(ASAN_APP_DIR)/$(call test-name,$(test))$(EXE_EXTENSION))

# Compile flags for ASan builds (include UBSan for comprehensive checking)
ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGCOMP_BUILD -DGCOMP_TEST_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_COMPRESSLIBRARY := -L $(ASAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-asan

# Add PIC on Linux
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

# Pattern rule for ASan-instrumented C object files
$(ASAN_OBJ_DIR)/%.o: src/%.c
	@printf "\n### Compiling (ASan+UBSan instrumented): $< ###\n"
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -o $@

# ASan-instrumented static library
$(ASAN_APP_DIR)/$(ASAN_STATIC_TARGET): $(ASAN_LIBOBJECTS)
	@printf "\n### Archiving ASan+UBSan-instrumented Static Library ###\n"
	@mkdir -p $(@D)
	ar rcs $@ $^

# ASan-instrumented shared library
$(ASAN_APP_DIR)/$(ASAN_TARGET): $(ASAN_LIBOBJECTS)
	@printf "\n### Compiling ASan+UBSan-instrumented Shared Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -shared -o $@ $^ $(ASAN_LDFLAGS)

# ASan test helper object
$(ASAN_OBJ_DIR)/tests/common/test_helpers.o: tests/common/test_helpers.cpp
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -o $@

# Pattern rule for ASan test object files
$(ASAN_OBJ_DIR)/tests/%.o: tests/%.cpp
	@printf "\n### Compiling ASan+UBSan Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -o $@

# Pattern rule for ASan test executables
define asan-test-executable-rule
ASAN_TEST_OBJ_$1 := $(ASAN_OBJ_DIR)/tests/$(patsubst tests/%.cpp,%.o,$1)

$(ASAN_APP_DIR)/$(call test-name,$1)$(EXE_EXTENSION): \
		$$(ASAN_TEST_OBJ_$1) \
		$(ASAN_TEST_HELPER_OBJ) \
		| $(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Linking ASan+UBSan %s Test ###\n" "$(call test-name,$1)"
	@mkdir -p $$(@D)
	$$(CXX) $$(ASAN_CXXFLAGS) -o $$@ $$(ASAN_TEST_OBJ_$1) $$(ASAN_TEST_HELPER_OBJ) $$(ASAN_LDFLAGS) $$(TESTFLAGS) $$(ASAN_COMPRESSLIBRARY)
endef

# Generate ASan build rules for all test sources
$(foreach test,$(TEST_SOURCES),$(eval $(call asan-test-executable-rule,$(test))))

test-asan: ## Run all tests with AddressSanitizer + UndefinedBehaviorSanitizer (Linux only)
test-asan: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m\n"
	@printf "###########################################\n"
	@printf "### Running tests with ASan + UBSan    ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION) | sed 's/test/\u&/'); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests (ASan+UBSan) ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		LD_LIBRARY_PATH="$(ASAN_APP_DIR)" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\n"
	@printf "###########################################\n"
	@printf "### All tests passed with ASan + UBSan ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-ubsan: ## Alias for test-asan (ASan+UBSan are run together)
test-ubsan: test-asan

test-asan-quiet: ## Run ASan+UBSan tests with minimal output (Linux only)
test-asan-quiet: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;33m%-30s %8s %10s %s\033[0m\n" "Test Suite (ASan+UBSan)" "Tests" "Time" "Status"; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(ASAN_APP_DIR)" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

sanitizer-help: ## Show sanitizer build help
	@printf "\033[0;36m"
	@printf "###########################################\n"
	@printf "### Sanitizer Builds                   ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@printf "Available targets:\n"
	@printf "  make test-asan   - Run tests with AddressSanitizer + UndefinedBehaviorSanitizer\n"
	@printf "  make test-ubsan  - Alias for test-asan (both run together)\n"
	@printf "\n"
	@printf "What these sanitizers detect:\n"
	@printf "  ASan (AddressSanitizer):\n"
	@printf "    - Buffer overflows (heap, stack, global)\n"
	@printf "    - Use-after-free\n"
	@printf "    - Use-after-return (with ASAN_OPTIONS=detect_stack_use_after_return=1)\n"
	@printf "    - Double-free\n"
	@printf "    - Memory leaks\n"
	@printf "  UBSan (UndefinedBehaviorSanitizer):\n"
	@printf "    - Signed integer overflow\n"
	@printf "    - Divide by zero\n"
	@printf "    - Null pointer dereference\n"
	@printf "    - Invalid shift operations\n"
	@printf "    - Out-of-bounds array access\n"
	@printf "    - Misaligned memory access\n"
	@printf "\n"
	@printf "Note: Sanitizer builds are slower than regular builds but catch\n"
	@printf "bugs that may not cause immediate crashes in release builds.\n"
	@printf "\n"

clean: ## Remove all contents of the build directories.
	-@rm -rvf $(BUILD_DIR)

# Files will be as follows:
# /usr/local/lib/(SUITE)/
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR).(MINOR)
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR) link to previous
#   lib(SUITE)-(PROJECT)(BRANCH).so link to previous
# /etc/ld.so.conf.d/(SUITE)-(PROJECT)(BRANCH).conf will point to /usr/local/lib/(SUITE)
# /usr/local/include/(SUITE)/(PROJECT)(BRANCH)
#   *.h copied from ./include/(PROJECT)
# /usr/local/share/pkgconfig
#   (SUITE)-(PROJECT)(BRANCH).pc created

install: ## Install the library globally, requires sudo
	# Installing the shared library.
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
# Install the .so file
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	# Installing the ld configuration file.
	@echo "/usr/local/lib/$(SUITE)" > /etc/ld.so.conf.d/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
# The .dll file and the .dll.a file
	@mkdir -p $(BIN_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)
endif
	# Installing the headers.
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -d $(GEN_DIR) ] && [ -n "$$(ls -A $(GEN_DIR) 2>/dev/null)" ]; then \
		mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT); \
		cp $(GEN_DIR)/*.h $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT)/; \
	fi
	# Installing the pkg-config files.
	@mkdir -p $(PKG_CONFIG_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(LIB)|$(LIB_INSTALL_PATH)|g; s|(INCLUDE)|$(INCLUDE_INSTALL_PATH)|g' > $(PKG_CONFIG_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@ldconfig >> /dev/null 2>&1
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the globally-installed files.  Requires sudo.
	# Deleting the shared library.
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	# Deleting the ld configuration file.
	@rm -f /etc/ld.so.conf.d/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	# Deleting the headers.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	# Deleting the pkg-config files.
	@rm -f $(PKG_CONFIG_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	# Cleaning up (potentially) no longer needed directories.
	@rmdir --ignore-fail-on-non-empty $(INCLUDE_INSTALL_PATH)/$(SUITE)
	@rmdir --ignore-fail-on-non-empty $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@ldconfig >> /dev/null 2>&1
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build the project in DEBUG mode
	make all BUILD=debug

install-debug: ## Install the DEBUG library globally, requires sudo
	make install BUILD=debug

uninstall-debug: ## Delete the DEBUG globally-installed files.  Requires sudo.
	make uninstall BUILD=debug

test-debug: ## Make and run the Unit tests in DEBUG mode
	make test BUILD=debug

test-valgrind-debug: ## Run all tests under valgrind in DEBUG mode (Linux only)
	make test-valgrind BUILD=debug

watch-debug: ## Watch the file directory for changes and compile the target in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch the file directory for changes and run the unit tests in DEBUG mode
	make test-watch BUILD=debug

docs: ## Generate the documentation in the ./docs subdirectory
	doxygen

docs-pdf: docs ## Generate the documentation as a pdf, at ./docs/(SUITE)-(PROJECT)(BRANCH).pdf
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include tests Makefile

help: ## Display this help
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' Makefile | sort | sed 's/\([^:]*\):.*## \(.*\)/\1:\2/' | awk -F: '{printf "%-15s %s\n", $$1, $$2}' | sed "s/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g"
