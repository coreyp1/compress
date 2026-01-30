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

# Automatically collect all .c source files under the src directory.
SOURCES := $(shell find src -type f -name '*.c')

# Convert each source file path to an object file path.
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))


TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs --cflags gtest`

# Valgrind flags (exclude "still reachable" as it's not a leak)
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1

# Test helper object file
TEST_HELPER_OBJ := $(OBJ_DIR)/tests/test_helpers.o

COMPRESSLIBRARY := -L $(APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)

# Automatically discover all test source files (excluding test_helpers.cpp)
TEST_SOURCES := $(filter-out tests/test_helpers.cpp, $(shell find tests -type f -name 'test*.cpp' 2>/dev/null))

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
$(TEST_HELPER_OBJ): tests/test_helpers.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for compiling test source files to object files
# This allows tests to be compiled separately from linking
$(OBJ_DIR)/tests/%.o: tests/%.cpp
	@printf "\n### Compiling Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for building test executables
# This automatically handles all test_*.cpp files
# Tests are compiled to .o files first, then linked separately
# This optimization allows tests to only relink (fast) when library changes but headers don't
define test-executable-rule
# Generate test object file path from test source path
# e.g., tests/test_callback_api.cpp -> build/linux/release/objects/tests/test_callback_api.o
# The object file is built by the pattern rule $(OBJ_DIR)/tests/%.o: tests/%.cpp above
TEST_OBJ_$1 := $(OBJ_DIR)/tests/$(basename $(notdir $1)).o

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
# Commands
####################################################################

# General commands
.PHONY: clean cloc docs docs-pdf examples bench bench-deflate
# Release build commands
.PHONY: all install test test-valgrind test-watch uninstall watch
# Debug build commands
.PHONY: all-debug install-debug test-debug test-valgrind-debug test-watch-debug uninstall-debug watch-debug


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
