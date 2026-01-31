# Architecture Overview

This document provides a high-level overview of the Ghoti.io Compress library architecture for contributors and maintainers.

## Component Overview

The library is organized into the following major components:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           User Application                              │
├─────────────────────────────────────────────────────────────────────────┤
│                          Public API Layer                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │
│  │   compress.h │  │   stream.h   │  │  registry.h  │  │  options.h  │  │
│  │  (Buffers)   │  │  (Streaming) │  │  (Methods)   │  │  (Config)   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  └─────────────┘  │
├─────────────────────────────────────────────────────────────────────────┤
│                           Core Layer                                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │
│  │  allocator.c │  │  registry.c  │  │   stream.c   │  │  options.c  │  │
│  │  limits.c    │  │   errors.c   │  │  stream_cb.c │  │  buffer.c   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  └─────────────┘  │
├─────────────────────────────────────────────────────────────────────────┤
│                         Method Layer                                    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                        deflate/                                 │    │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │ deflate_encode.c │  │ deflate_decode.c │  │   huffman.c   │  │    │
│  │  │   bitwriter.c    │  │   bitreader.c    │  │  (RFC 1951)   │  │    │
│  │  └──────────────────┘  └──────────────────┘  └───────────────┘  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                      (future: gzip, zstd, lz4)                  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

### Core Components

| Component | Header | Purpose |
|-----------|--------|---------|
| **Registry** | `registry.h` | Method lookup and storage |
| **Stream** | `stream.h` | Encoder/decoder lifecycle and streaming API |
| **Options** | `options.h` | Configuration key/value system |
| **Allocator** | `allocator.h` | Memory allocation abstraction |
| **Limits** | `limits.h` | Safety limits and memory tracking |
| **Errors** | `errors.h` | Status codes and error handling |
| **Method** | `method.h` | Method vtable interface |

### Method Layer

Each compression method (deflate, gzip, etc.) implements the `gcomp_method_t` interface:

```c
struct gcomp_method_s {
    uint32_t abi_version;                    // Forward compatibility
    size_t size;                             // Structure size
    const char *name;                        // Method name (e.g., "deflate")
    gcomp_capabilities_t capabilities;       // ENCODE, DECODE, or both
    
    // Encoder/decoder factory functions
    gcomp_status_t (*create_encoder)(...);
    gcomp_status_t (*create_decoder)(...);
    void (*destroy_encoder)(...);
    void (*destroy_decoder)(...);
    
    // Option schema (for introspection)
    const gcomp_method_schema_t *(*get_schema)(void);
};
```

## Data Flow

### Encoding Flow

```
                        ┌─────────────────┐
                        │   Application   │
                        └────────┬────────┘
                                 │ Input data
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_encoder_create()                          │
│  1. Look up method in registry                                         │
│  2. Allocate encoder structure                                         │
│  3. Call method->create_encoder() to initialize state                  │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_encoder_update()                          │
│  ┌─────────────┐    ┌─────────────────────────┐    ┌────────────────┐  │
│  │ Input buffer│ ─► │ Method-specific encoder │ ─► │ Output buffer  │  │
│  │  (raw data) │    │  (LZ77, Huffman, etc.)  │    │ (compressed)   │  │
│  └─────────────┘    └─────────────────────────┘    └────────────────┘  │
│  • input.used = bytes consumed from input                              │
│  • output.used = bytes produced to output                              │
│  • May be called multiple times for streaming                          │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_encoder_finish()                          │
│  • Flushes any buffered data                                           │
│  • Writes final block marker                                           │
│  • Flushes to byte boundary                                            │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                       gcomp_encoder_destroy()                          │
│  1. Call method->destroy_encoder() to clean up state                   │
│  2. Free encoder structure                                             │
└────────────────────────────────────────────────────────────────────────┘
```

### Decoding Flow

```
                        ┌─────────────────┐
                        │   Application   │
                        └────────┬────────┘
                                 │ Compressed data
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_decoder_create()                          │
│  1. Look up method in registry                                         │
│  2. Allocate decoder structure                                         │
│  3. Call method->create_decoder() to initialize state                  │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_decoder_update()                          │
│  ┌────────────────┐    ┌────────────────────┐    ┌─────────────────┐   │
│  │  Input buffer  │ ─► │ Method-specific    │ ─► │  Output buffer  │   │
│  │  (compressed)  │    │ decoder (Huffman,  │    │   (raw data)    │   │
│  │                │    │ LZ77 window, etc.) │    │                 │   │
│  └────────────────┘    └────────────────────┘    └─────────────────┘   │
│  • Checks limits: max_output_bytes, max_expansion_ratio                │
│  • Returns GCOMP_ERR_LIMIT if exceeded                                 │
│  • Returns GCOMP_ERR_CORRUPT on invalid data                           │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_decoder_finish()                          │
│  • Validates stream is complete (final block seen)                     │
│  • Drains any remaining output                                         │
│  • Returns GCOMP_ERR_CORRUPT if stream is truncated                    │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                       gcomp_decoder_destroy()                          │
│  1. Call method->destroy_decoder() to clean up state                   │
│  2. Free decoder structure                                             │
└────────────────────────────────────────────────────────────────────────┘
```

## Memory Ownership Model

### Ownership Rules

| Object | Owner | Lifetime |
|--------|-------|----------|
| `gcomp_registry_t` | Caller (or library for default) | Until `destroy()` or program exit |
| `gcomp_encoder_t` | Caller | Until `destroy()` |
| `gcomp_decoder_t` | Caller | Until `destroy()` |
| `gcomp_options_t` | Caller | Until `destroy()` (can outlive encoder/decoder) |
| Input buffer data | Caller | Must remain valid during `update()` call |
| Output buffer data | Caller | Must remain valid during `update()` call |
| Method state | Method (via method_state pointer) | Managed by method's create/destroy |

### Allocator Hierarchy

```
┌─────────────────────────────────────────────────────────────────────────┐
│                      Registry Allocator                                 │
│  • Set via gcomp_registry_create(allocator, ...)                        │
│  • Default registry uses gcomp_allocator_default() (stdlib malloc)      │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      Encoder/Decoder Allocator                          │
│  • Inherits from registry that created it                               │
│  • All internal allocations use gcomp_malloc/calloc/free helpers        │
│  • Method implementations must use st->allocator (not raw malloc)       │
└─────────────────────────────────────────────────────────────────────────┘
```

### Memory Tracking

Methods track memory usage via `gcomp_memory_tracker_t`:

```c
// In decoder state initialization
gcomp_memory_track_alloc(&state->mem_tracker, sizeof(decode_state));
gcomp_memory_track_alloc(&state->mem_tracker, window_size);

// Check against limits
if (gcomp_limits_check_memory(state->mem_tracker.current_bytes, max_memory)
    != GCOMP_OK) {
    return GCOMP_ERR_LIMIT;
}

// On cleanup
gcomp_memory_track_free(&state->mem_tracker, window_size);
```

## Error Propagation Patterns

### Status Code Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     Method Implementation                               │
│                                                                         │
│  // Set error with context                                              │
│  return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,             │
│      "invalid block type %d at stage '%s'", block_type, stage_name);    │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     Core Stream Layer                                   │
│                                                                         │
│  // Pass through to caller                                              │
│  return encoder->update_fn(encoder, input, output);                     │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     Application                                         │
│                                                                         │
│  gcomp_status_t status = gcomp_decoder_update(dec, &in, &out);          │
│  if (status != GCOMP_OK) {                                              │
│      printf("Error: %s\n", gcomp_status_to_string(status));             │
│      printf("Detail: %s\n", gcomp_decoder_get_error_detail(dec));       │
│  }                                                                      │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Error Categories

| Status Code | Recoverable? | Typical Cause |
|-------------|--------------|---------------|
| `GCOMP_OK` | N/A | Success |
| `GCOMP_ERR_INVALID_ARG` | Yes | NULL pointer, invalid parameter |
| `GCOMP_ERR_MEMORY` | Maybe | Allocation failed |
| `GCOMP_ERR_LIMIT` | Yes | Resource limit exceeded |
| `GCOMP_ERR_CORRUPT` | No | Invalid compressed data |
| `GCOMP_ERR_UNSUPPORTED` | No | Method not found or feature unavailable |
| `GCOMP_ERR_INTERNAL` | No | Bug in library |
| `GCOMP_ERR_IO` | Maybe | I/O callback error |

### Error Detail Storage

Encoders and decoders store error details in a fixed-size buffer (256 bytes) to avoid allocation during error paths:

```c
struct gcomp_encoder_s {
    // ... other fields ...
    gcomp_status_t last_error;           // Error code
    char error_detail[256];              // Human-readable context
};
```

## Extension Points for New Methods

### Adding a New Compression Method

1. **Create method files** in `src/methods/<method_name>/`:

```
src/methods/mymethod/
├── mymethod_decode.c      # Decoder implementation
├── mymethod_encode.c      # Encoder implementation
├── mymethod_internal.h    # Shared internal definitions
└── mymethod_register.c    # Registration and vtable
```

2. **Define the method vtable** in `mymethod_register.c`:

```c
static const gcomp_method_t g_mymethod = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "mymethod",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = mymethod_create_encoder,
    .create_decoder = mymethod_create_decoder,
    .destroy_encoder = mymethod_destroy_encoder,
    .destroy_decoder = mymethod_destroy_decoder,
    .get_schema = mymethod_get_schema,
};
```

3. **Implement the encoder**:

```c
// Include internal header for stream structures
#include "core/stream_internal.h"

gcomp_status_t mymethod_create_encoder(gcomp_registry_t *registry,
    gcomp_options_t *options, gcomp_encoder_t **encoder_out) {
    
    gcomp_encoder_t *enc = *encoder_out;
    const gcomp_allocator_t *alloc = gcomp_registry_get_allocator(registry);
    
    // Allocate method-specific state
    mymethod_encode_state_t *state = gcomp_calloc(alloc, 1, sizeof(*state));
    if (!state) return GCOMP_ERR_MEMORY;
    
    state->allocator = alloc;
    // ... initialize state ...
    
    // Set up function pointers
    enc->method_state = state;
    enc->update_fn = mymethod_encode_update;
    enc->finish_fn = mymethod_encode_finish;
    enc->reset_fn = mymethod_encode_reset;  // Optional
    
    return GCOMP_OK;
}

static gcomp_status_t mymethod_encode_update(gcomp_encoder_t *encoder,
    gcomp_buffer_t *input, gcomp_buffer_t *output) {
    
    mymethod_encode_state_t *state = encoder->method_state;
    // ... compress input to output ...
    return GCOMP_OK;
}
```

4. **Add auto-registration** (optional):

```c
#include "autoreg/autoreg_platform.h"

gcomp_status_t gcomp_method_mymethod_register(gcomp_registry_t *registry) {
    if (!registry) return GCOMP_ERR_INVALID_ARG;
    return gcomp_registry_register(registry, &g_mymethod);
}

GCOMP_AUTOREG_METHOD(mymethod, gcomp_method_mymethod_register)
```

5. **Define option schema** for configuration:

```c
static const gcomp_option_schema_t g_mymethod_options[] = {
    {
        .key = "mymethod.level",
        .type = GCOMP_OPT_INT64,
        .has_default = 1,
        .default_value.i64 = 6,
        .has_min = 1,
        .has_max = 1,
        .min_int = 1,
        .max_int = 9,
        .help = "Compression level (1=fast, 9=best)"
    },
    // ... more options ...
};

static const gcomp_method_schema_t g_mymethod_schema = {
    .options = g_mymethod_options,
    .num_options = sizeof(g_mymethod_options) / sizeof(g_mymethod_options[0]),
    .unknown_key_policy = GCOMP_UNKNOWN_KEY_IGNORE,
};
```

### Implementing a Wrapper Method

Wrapper methods (like gzip) wrap another method (like deflate) and add headers/trailers. See [Wrapper Methods](wrapper-methods.md) for detailed documentation.

Key pattern:

```c
gcomp_status_t gzip_create_encoder(gcomp_registry_t *registry,
    gcomp_options_t *options, gcomp_encoder_t **encoder_out) {
    
    // Look up wrapped method
    const gcomp_method_t *deflate = gcomp_registry_find(registry, "deflate");
    if (!deflate) return GCOMP_ERR_UNSUPPORTED;
    
    // Create inner encoder
    gcomp_encoder_t *inner = NULL;
    gcomp_status_t status = deflate->create_encoder(registry, options, &inner);
    if (status != GCOMP_OK) return status;
    
    // Store inner encoder in wrapper state
    state->inner_encoder = inner;
    // ... set up gzip header/trailer handling ...
}
```

## Threading Model

| Component | Thread Safety |
|-----------|--------------|
| Default registry | Safe for concurrent reads after initialization |
| Custom registries | Not thread-safe; use one registry per thread |
| Encoders/decoders | Not thread-safe; use one instance per thread |
| Options (frozen) | Safe to share across threads |
| Options (mutable) | Not thread-safe |
| Allocator callbacks | Must be thread-safe if registry is shared |

## File Organization

```
compress/
├── include/ghoti.io/compress/    # Public headers
│   ├── compress.h                # Main header (convenience functions)
│   ├── stream.h                  # Streaming API
│   ├── registry.h                # Method registry
│   ├── method.h                  # Method vtable interface
│   ├── options.h                 # Configuration
│   ├── errors.h                  # Status codes
│   ├── allocator.h               # Memory allocation
│   ├── limits.h                  # Safety limits
│   ├── deflate.h                 # Deflate-specific API
│   └── macros.h                  # Cross-compiler utilities
│
├── src/
│   ├── compress.c                # Convenience functions
│   ├── core/                     # Core implementation
│   │   ├── stream.c              # Encoder/decoder management
│   │   ├── stream_cb.c           # Callback-based streaming
│   │   ├── registry.c            # Method registry
│   │   ├── options.c             # Options system
│   │   ├── allocator.c           # Default allocator
│   │   ├── limits.c              # Limit checking
│   │   ├── errors.c              # Error utilities
│   │   ├── buffer.c              # Buffer convenience functions
│   │   └── *_internal.h          # Internal headers
│   │
│   ├── autoreg/                  # Auto-registration support
│   │   └── autoreg_platform.h    # Platform-specific constructors
│   │
│   └── methods/                  # Compression method implementations
│       └── deflate/
│           ├── deflate_encode.c  # Encoder (LZ77 + Huffman)
│           ├── deflate_decode.c  # Decoder
│           ├── deflate_register.c# Vtable and registration
│           ├── huffman.c         # Huffman table building
│           ├── bitreader.c       # Bit-level input
│           └── bitwriter.c       # Bit-level output
│
├── tests/                        # Unit tests (Google Test)
├── examples/                     # Example programs
├── bench/                        # Benchmarks
├── fuzz/                         # Fuzz testing harnesses
└── documentation/                # Detailed documentation
```

## Related Documentation

- [Streaming API](api/streaming.md) - Detailed streaming usage patterns
- [Deflate Module](modules/deflate.md) - Deflate-specific options and usage
- [Auto-Registration](auto-registration.md) - How methods are automatically registered
- [Wrapper Methods](wrapper-methods.md) - Implementing wrapper methods (gzip, etc.)
- [Testing](testing/testing.md) - Testing infrastructure
- [Fuzzing](testing/fuzzing.md) - Fuzz testing documentation
