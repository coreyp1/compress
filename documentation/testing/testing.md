# Testing Guide

This document describes how to run tests for the compress library, including unit tests, memory checking with valgrind, and sanitizer builds.

## Quick Reference

| Command | Description |
|---------|-------------|
| `make test` | Run all tests (release build) |
| `make test-debug` | Run all tests (debug build) |
| `make test-valgrind` | Run all tests under valgrind |
| `make test-valgrind-debug` | Run all tests under valgrind (debug) |
| `make test-asan` | Run tests with AddressSanitizer + UBSan |
| `make test-ubsan` | Alias for `test-asan` |

## Unit Tests

The test suite uses Google Test (gtest) and covers:
- Core infrastructure (options, registry, stream API)
- Deflate encoder and decoder
- Malformed input handling
- Stress and stability tests
- Cross-tool validation (oracle tests)

### Running Tests

```bash
# Run all tests (release build)
make test

# Run all tests (debug build with assertions)
make test-debug

# Run a specific test file
./build/linux/release/apps/tests/test_deflate_decoder --gtest_brief=1

# Run tests matching a pattern
./build/linux/release/apps/tests/test_deflate_decoder --gtest_filter="*StoredBlock*"
```

### Test Output

Tests use brief output mode by default. For verbose output:

```bash
./build/linux/release/apps/tests/test_deflate_decoder
```

## Valgrind Memory Checking

Valgrind detects memory errors that may not cause immediate crashes:
- Memory leaks (allocated memory never freed)
- Use-after-free
- Invalid reads/writes
- Uninitialized memory usage

### Running Valgrind Tests

```bash
# Run all tests under valgrind (Linux only)
make test-valgrind

# Run with debug build for better stack traces
make test-valgrind-debug
```

### What Valgrind Checks

The test target uses these valgrind flags:
- `--leak-check=full` - Detailed memory leak reporting
- `--show-leak-kinds=all` - Report all types of leaks
- `--track-origins=yes` - Track where uninitialized values came from
- `--error-exitcode=1` - Fail the build if errors are found

### Understanding Valgrind Output

**Clean output:**
```
==12345== HEAP SUMMARY:
==12345==     in use at exit: 0 bytes in 0 blocks
==12345==   total heap usage: 1,234 allocs, 1,234 frees, 56,789 bytes allocated
==12345== All heap blocks were freed -- no leaks are possible
```

**Memory leak example:**
```
==12345== 64 bytes in 1 blocks are definitely lost in loss record 1 of 1
==12345==    at 0x4C2BBAF: malloc (vg_replace_malloc.c:299)
==12345==    by 0x401234: my_function (myfile.c:42)
==12345==    by 0x401345: main (main.c:10)
```

**Invalid read example:**
```
==12345== Invalid read of size 4
==12345==    at 0x401234: my_function (myfile.c:45)
==12345==  Address 0x5204044 is 0 bytes after a block of size 4 alloc'd
```

### Running Valgrind Manually

For debugging a specific test or binary:

```bash
# Basic memory check
valgrind ./build/linux/release/apps/tests/test_deflate_decoder

# With full leak checking
valgrind --leak-check=full --show-leak-kinds=all \
    ./build/linux/release/apps/tests/test_deflate_decoder

# With origin tracking (slower but more informative)
valgrind --leak-check=full --track-origins=yes \
    ./build/linux/release/apps/tests/test_deflate_decoder

# Check a specific test
valgrind --leak-check=full \
    ./build/linux/release/apps/tests/test_deflate_decoder \
    --gtest_filter="*RoundTrip*"
```

### Valgrind Tips

1. **Use debug builds** - Debug builds have symbols and no optimization, making stack traces more useful:
   ```bash
   make test-valgrind-debug
   ```

2. **Focus on one test** - When debugging a leak, isolate it:
   ```bash
   valgrind --leak-check=full ./build/linux/debug/apps/tests/test_options
   ```

3. **Suppressions** - If you encounter false positives from system libraries, you can create a suppressions file. Currently, no suppressions are needed.

4. **Platform note** - Valgrind is only available on Linux. On Windows, use the sanitizer builds or Windows debugging tools.

## Sanitizer Builds

Address Sanitizer (ASan) and Undefined Behavior Sanitizer (UBSan) catch errors at runtime with lower overhead than valgrind.

### Running Sanitizer Tests

```bash
# Run tests with ASan + UBSan
make test-asan

# Show help about sanitizers
make sanitizer-help
```

### What Sanitizers Detect

**AddressSanitizer (ASan):**
- Buffer overflows (heap, stack, global)
- Use-after-free
- Use-after-return
- Double-free
- Memory leaks (at exit)

**UndefinedBehaviorSanitizer (UBSan):**
- Signed integer overflow
- Null pointer dereference
- Misaligned pointer access
- Division by zero
- Out-of-bounds array access
- Invalid enum values

### Understanding Sanitizer Output

**ASan buffer overflow example:**
```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000014
READ of size 1 at 0x602000000014 thread T0
    #0 0x401234 in my_function myfile.c:42
    #1 0x401345 in main main.c:10
```

**UBSan signed overflow example:**
```
myfile.c:42:15: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
```

### Sanitizers vs Valgrind

| Aspect | Valgrind | ASan/UBSan |
|--------|----------|------------|
| Speed | 10-50x slower | 2x slower |
| Memory overhead | High (~2x) | Moderate |
| Compilation | Not required | Requires recompilation |
| Leak detection | Excellent | Good (ASan) |
| Undefined behavior | Limited | Excellent (UBSan) |
| Platform | Linux only | Linux, macOS, Windows |

**Recommendation:** Use ASan/UBSan during development for fast iteration, and valgrind for thorough final verification.

## Oracle Tests

Oracle tests compare our implementation against Python's zlib library to verify correctness.

### Running Oracle Tests

```bash
# Oracle tests run automatically with make test
make test

# Skip oracle tests (if Python unavailable)
GCOMP_SKIP_ORACLE_TESTS=1 make test

# Verbose oracle test output
GCOMP_ORACLE_VERBOSE=1 make test
```

### Requirements

- Python 3 with zlib module (standard library)
- Tests skip gracefully if Python is unavailable

## Stress Tests

Stress tests verify stability under heavy load.

### Running Stress Tests

```bash
# Run with default settings
make test

# Configure stress test iterations
GCOMP_STRESS_ITERATIONS=1000 make test

# Configure large file size (bytes)
GCOMP_STRESS_LARGE_SIZE=10485760 make test  # 10 MB
```

### What Stress Tests Cover

- Rapid create/destroy cycles (memory leaks)
- Large file compression (1+ MB)
- Many small file compressions
- All compression levels
- Multiple simultaneous encoders/decoders
- Streaming with tiny buffers

## Fuzz Testing

See [fuzzing.md](fuzzing.md) for comprehensive fuzz testing documentation.

## Test Organization

```
tests/
├── test_options.cpp          # Options API tests
├── test_registry.cpp         # Registry tests
├── test_stream.cpp           # Stream infrastructure tests
├── test_limits.cpp           # Safety limits tests
├── test_crc32.cpp            # CRC32 utility tests
├── test_buffer_wrappers.cpp  # Buffer convenience API tests
├── test_callback_api.cpp     # Callback streaming API tests
├── test_schema.cpp           # Option schema introspection tests
├── test_autoreg.cpp          # Auto-registration tests
├── test_passthru.cpp         # Pass-thru method tests
├── test_deflate_bitio.cpp    # Bit I/O primitives tests
├── test_deflate_huffman.cpp  # Huffman table tests
├── test_deflate_decoder.cpp  # Deflate decoder tests
├── test_deflate_encoder.cpp  # Deflate encoder tests
├── test_deflate_malformed.cpp # Malformed input tests
├── test_expansion_ratio.cpp  # Decompression bomb protection tests
├── test_state_machine.cpp    # State machine robustness tests
├── test_oracle.cpp           # Cross-tool validation tests
├── test_stress.cpp           # Stress and stability tests
├── test_helpers.h            # Test utility functions
├── test_helpers.cpp          # Test utility implementations
├── passthru_method.h         # Pass-thru method for testing
└── data/                     # Test data files
    └── deflate/
        └── golden_vectors.h  # Known good test vectors
```

## Continuous Integration

For CI environments, run the full test suite:

```bash
# Build and test
make all
make test
make test-valgrind
make test-asan

# Quick smoke test (faster)
make test
```

### Exit Codes

All test commands return:
- `0` - All tests passed
- `1` - One or more tests failed

This allows CI systems to detect failures automatically.
