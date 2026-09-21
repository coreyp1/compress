# Building the Compress Library

This document describes how to build the Compress library from source.

## Prerequisites

### Required

- **C Compiler:** GCC or Clang with C17 support
- **C++ Compiler:** GCC or Clang with C++20 support (for tests only)
- **Make:** GNU Make
- **pkg-config:** For dependency management

### Optional

- **Google Test:** For running unit tests
- **Doxygen:** For generating API documentation
- **Graphviz:** For documentation diagrams
- **Valgrind:** For memory checking tests
- **AFL++:** For fuzzing

### Platform Notes

| Platform | Compiler | Notes |
|----------|----------|-------|
| Linux | GCC 10+, Clang 11+ | Primary development platform |
| macOS | Clang (Xcode) | Supported |
| Windows | MinGW-w64 | Via MSYS2 environment |

## Quick Start

```bash
# Clone the repository
git clone https://github.com/ghoti-io/compress.git
cd compress

# Build the library
make

# Run tests (requires Google Test)
make test

# Install system-wide (optional)
sudo make install
```

## Build Targets

### Library Targets

| Target | Description |
|--------|-------------|
| `make` or `make all` | Build shared and static libraries |
| `make debug` | Build in debug mode with extra checks |
| `make clean` | Remove all build artifacts |

### Test Targets

| Target | Description |
|--------|-------------|
| `make test` | Build and run all unit tests |
| `make test-quiet` | Run tests with minimal output |
| `make test-debug` | Run tests in debug mode |
| `make test-asan` | Run tests with AddressSanitizer + UBSan |
| `make test-tsan` | Run tests with ThreadSanitizer |
| `make test-valgrind` | Run tests under Valgrind |

### Documentation Targets

| Target | Description |
|--------|-------------|
| `make docs` | Generate HTML documentation |
| `make docs-pdf` | Generate PDF documentation |

### Benchmark Targets

| Target | Description |
|--------|-------------|
| `make bench` | Build all benchmarks (deflate, LZW, etc.) |
| `make bench-deflate` | Build and run the deflate benchmark |

After `make bench`, run benchmarks from the build `apps` directory with `LD_LIBRARY_PATH` set (e.g. `LD_LIBRARY_PATH=./build/linux/release/apps ./build/linux/release/apps/bench/bench_deflate` or `bench_lzw`). The `make bench` output prints the exact commands for your platform.

### Fuzzing Targets

| Target | Description |
|--------|-------------|
| `make fuzz-build` | Build fuzz harnesses with AFL |
| `make fuzz-corpus` | Generate seed corpus |
| `make fuzz-decoder` | Run decoder fuzzer |
| `make fuzz-roundtrip` | Run roundtrip fuzzer |
| `make fuzz-help` | Show fuzzing help |

### Installation Targets

| Target | Description |
|--------|-------------|
| `make install` | Install library system-wide |
| `make uninstall` | Remove installed files |
| `make install-debug` | Install debug build |

## Build Options

The Makefile supports several variables to customize the build:

### Compiler Selection

```bash
# Use Clang instead of GCC
make CC=clang CXX=clang++
```

### Build Type

```bash
# Debug build
make BUILD=debug

# Release build (default)
make BUILD=release
```

### Parallel Build

```bash
# Use 4 parallel jobs
make -j4
```

### CPU-specific code, and turning it off

Two checksums have a second implementation that uses instructions outside the
x86-64 baseline. Both are chosen at run time from `CPUID`, so one binary runs
correctly on every x86-64 CPU; nothing here changes what a build *requires*.

| Checksum | Extra implementation | Instruction set | Used by |
| --- | --- | --- | --- |
| CRC-32 | carry-less multiply folding | PCLMULQDQ (+SSE2) | gzip, PNG |
| Adler-32 | 32-byte groups via `PMADDUBSW` | SSSE3 | zlib, so every PNG |

**No compiler flags are involved.** Each function carries
`__attribute__((target(...)))`, so the widened instruction set applies to
those functions alone. Passing `-mpclmul` or `-mssse3` on the command line is
neither needed nor wanted: it would license the compiler to emit those
instructions anywhere in the unit, and a binary that does that dies with
SIGILL on a CPU that lacks them.

Both scalar implementations remain in the library. They are what runs on a
CPU without the extension, and they are what the vector paths are tested
against -- `Crc32Variants` and `Adler32Variants` in the test suite compare
every implementation against every other on ten thousand random buffers.

To force the scalar paths, define one or more of:

| Define | Effect |
| --- | --- |
| `GCOMP_NO_CHECKSUM_SIMD` | Both vector checksums; the translation units compile to stubs |
| `GCOMP_CRC32_NO_PCLMUL` | The folding CRC-32 only |
| `GCOMP_ADLER32_NO_SSSE3` | The vector Adler-32 only |
| `GCOMP_CRC32_NO_SLICE_BY_8` | Also the scalar slice-by-8 CRC, leaving the byte loop |

```bash
# Scalar checksums throughout
make EXTRA_CFLAGS=-DGCOMP_NO_CHECKSUM_SIMD
```

That is also how the two are A/B measured, since building both and swapping
the shared object is the only way to compare them on the same machine.

On any target that is not x86 with GCC or Clang, the vector translation units
compile to stubs whose availability predicates answer false, and the scalar
paths are all there is. Nothing needs to be defined for that.

## Output Directories

```
compress/
├── build/
│   └── linux/
│       └── release/
│           ├── apps/           # Executables and libraries
│           │   ├── libghoti.io-compress-0.so
│           │   ├── libghoti.io-compress-0.a
│           │   ├── bench/      # Benchmark executables (bench_deflate, bench_lzw)
│           │   └── test*       # Test executables
│           ├── objects/        # Compiled object files
│           └── generated/      # Auto-generated files
└── docs/
    ├── html/                   # Generated HTML docs
    └── latex/                  # LaTeX files for PDF
```

## Dependencies

### No Runtime Dependencies

The library has **no external runtime dependencies**. It is self-contained and uses only standard C library functions.

### Build-Time Dependencies

| Dependency | Purpose | Required |
|------------|---------|----------|
| Google Test | Unit testing | For `make test` |
| Doxygen | Documentation | For `make docs` |
| Graphviz | Doc diagrams | For `make docs` |
| Valgrind | Memory checking | For `make test-valgrind` |
| AFL++ | Fuzzing | For `make fuzz-*` |

### Installing Dependencies

**Ubuntu/Debian:**
```bash
# Required
sudo apt-get install build-essential pkg-config

# Testing
sudo apt-get install libgtest-dev cmake

# Documentation
sudo apt-get install doxygen graphviz

# Memory checking
sudo apt-get install valgrind

# Fuzzing
sudo apt-get install afl++
```

**macOS (Homebrew):**
```bash
# Required (Xcode command line tools)
xcode-select --install

# Testing
brew install googletest

# Documentation
brew install doxygen graphviz

# Fuzzing
brew install afl++
```

**Windows (MSYS2):**
```bash
# Required
pacman -S mingw-w64-x86_64-gcc make pkg-config

# Testing
pacman -S mingw-w64-x86_64-gtest
```

## Integration

### pkg-config

After installation, the library can be found via pkg-config:

```bash
# Compile flags
pkg-config --cflags ghoti.io-compress

# Link flags
pkg-config --libs ghoti.io-compress
```

### CMake

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(GCOMP REQUIRED ghoti.io-compress)

target_include_directories(myapp PRIVATE ${GCOMP_INCLUDE_DIRS})
target_link_libraries(myapp ${GCOMP_LIBRARIES})
```

### Direct Linking

```bash
# Compile
gcc -c myapp.c -I/usr/local/include

# Link (shared library)
gcc -o myapp myapp.o -lghoti.io-compress

# Link (static library)
gcc -o myapp myapp.o /usr/local/lib/libghoti.io-compress.a
```

## Troubleshooting

### Test Failures

If tests fail to build:

1. Ensure Google Test is installed and pkg-config can find it:
   ```bash
   pkg-config --libs gtest
   ```

2. On Ubuntu, you may need to build gtest from source:
   ```bash
   cd /usr/src/gtest
   sudo cmake .
   sudo make
   sudo cp lib/*.a /usr/lib/
   ```

### Library Not Found

If the installed library isn't found:

1. Update the library cache:
   ```bash
   sudo ldconfig
   ```

2. Check `LD_LIBRARY_PATH`:
   ```bash
   export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
   ```

### Compilation Errors

If you see C17/C++20 errors:

1. Check compiler version:
   ```bash
   gcc --version   # Need GCC 10+
   clang --version # Need Clang 11+
   ```

2. Update or install a newer compiler.

## Development

### Watch Mode

For development, use watch mode to automatically rebuild on changes:

```bash
# Rebuild library on source changes
make watch

# Rebuild and run tests on changes
make test-watch
```

### Code Formatting

The project uses clang-format for code formatting:

```bash
# Format all source files (if configured)
clang-format -i src/**/*.c include/**/*.h
```

### Running Individual Tests

```bash
# Run specific test suite
LD_LIBRARY_PATH=./build/linux/release/apps \
  ./build/linux/release/apps/testGzip_decoder

# Run with filter
LD_LIBRARY_PATH=./build/linux/release/apps \
  ./build/linux/release/apps/testGzip_decoder --gtest_filter="*CRC*"
```
