# Ghoti.io Compress Library

Cross-platform C library implementing streaming compression with no external dependencies.

## Overview

The `compress` library provides:
- Streaming compression and decompression for files, memory buffers, and pipes/sockets
- Support for multiple compression methods (deflate, gzip, zstd, lz4)
- Global default registry and explicit registries for compression methods
- Key/value option system for rich configuration
- Intelligent safety defaults with overridable resource limits

## Dependencies

- No external dependencies (self-contained implementation)

## Building

```bash
make
```

## Testing

```bash
make test
```

### Running Tests with Valgrind

```bash
make test-valgrind
```

### Fuzz Testing

The library includes fuzz testing infrastructure using [AFL++](https://github.com/AFLplusplus/AFLplusplus) to find edge cases and security issues.

**Prerequisites:**
```bash
sudo apt install afl++
```

**Running fuzz tests:**
```bash
# Generate seed corpus from test vectors
make fuzz-corpus

# Build fuzz harnesses with AFL instrumentation
make fuzz-build

# Run a fuzzer (Ctrl+C to stop)
make fuzz-decoder    # Test decoder with random compressed data
make fuzz-encoder    # Test encoder with random input
make fuzz-roundtrip  # Encode then decode, verify match

# Show help and available targets
make fuzz-help
```

Findings are saved to `fuzz/findings/<target>/crashes/`. See `documentation/testing/fuzzing.md` for detailed documentation.

## Installation

```bash
sudo make install
```

## Usage

See the examples directory for usage examples.

## Documentation

- [Modules](@ref modules) - Detailed documentation for library modules
- [Examples](@ref examples) - Example programs demonstrating library usage
- [Function Index](@ref functions_index) - Complete API reference

## Macros and Utilities

The library provides cross-compiler macros in `include/ghoti.io/compress/macros.h`:

- `GCOMP_MAYBE_UNUSED(X)` - Mark unused function parameters
- `GCOMP_DEPRECATED` - Mark deprecated functions
- `GCOMP_API` - Mark functions for library export
- `GCOMP_ARRAY_SIZE(a)` - Get compile-time array size
- `GCOMP_BIT(x)` - Create bitmask with bit x set

Example:
```c
#include <ghoti.io/compress/macros.h>

void my_function(int GCOMP_MAYBE_UNUSED(param)) {
    // param is intentionally unused
}
```
