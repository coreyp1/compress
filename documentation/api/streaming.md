# Streaming API

The Ghoti.io Compress library provides a streaming API for incremental compression and decompression. This allows processing data in chunks without loading entire files into memory.

## Overview

The streaming API consists of:

- **Encoders** (`gcomp_encoder_t`) - compress data incrementally
- **Decoders** (`gcomp_decoder_t`) - decompress data incrementally
- **Buffers** (`gcomp_buffer_t`) - track input/output progress

## Buffer Structure

```c
typedef struct {
    const void *data;   // Pointer to data
    size_t size;        // Size of data in bytes
    size_t used;        // Number of bytes consumed/produced
} gcomp_buffer_t;
```

The `used` field is updated by `update()` calls to track how many bytes were consumed (input) or produced (output).

## Encoder Lifecycle

### Creation

```c
gcomp_encoder_t *encoder = NULL;
gcomp_status_t status = gcomp_encoder_create(
    registry,           // Registry (or NULL for default)
    "deflate",          // Method name
    options,            // Options (or NULL for defaults)
    &encoder            // Output pointer
);
```

### Processing

```c
gcomp_buffer_t input = { data, data_len, 0 };
gcomp_buffer_t output = { out_buf, out_capacity, 0 };

// Call update() one or more times
status = gcomp_encoder_update(encoder, &input, &output);
// input.used indicates bytes consumed
// output.used indicates bytes produced
```

### Finishing

```c
gcomp_buffer_t final_output = { out_buf + offset, remaining_capacity, 0 };
status = gcomp_encoder_finish(encoder, &final_output);
// Writes final block, flushes to byte boundary
```

### Reset (Optional)

For compressing multiple independent streams with the same options, reuse the encoder instead of destroying and recreating:

```c
// First compression...
gcomp_encoder_finish(encoder, &output);

// Reset for next stream (more efficient than destroy/create)
status = gcomp_encoder_reset(encoder);
if (status == GCOMP_ERR_UNSUPPORTED) {
    // Method doesn't support reset - must destroy and recreate
}

// Second compression with same options...
```

### Cleanup

```c
gcomp_encoder_destroy(encoder);
```

## Decoder Lifecycle

Decoders follow the same pattern:

```c
gcomp_decoder_t *decoder = NULL;
gcomp_decoder_create(registry, "deflate", options, &decoder);

gcomp_buffer_t input = { compressed, compressed_len, 0 };
gcomp_buffer_t output = { out_buf, out_capacity, 0 };

while (/* more data */) {
    status = gcomp_decoder_update(decoder, &input, &output);
    if (status != GCOMP_OK) break;
    // Process output.used bytes, advance buffers
}

status = gcomp_decoder_finish(decoder, &output);

// Optional: reset for another stream
status = gcomp_decoder_reset(decoder);

gcomp_decoder_destroy(decoder);
```

## Error Handling

### Status Codes

All streaming functions return `gcomp_status_t`:

| Code | Meaning |
|------|---------|
| `GCOMP_OK` | Operation succeeded |
| `GCOMP_ERR_INVALID_ARG` | NULL pointer or invalid argument |
| `GCOMP_ERR_MEMORY` | Memory allocation failed |
| `GCOMP_ERR_LIMIT` | Resource limit exceeded (e.g., max output bytes) |
| `GCOMP_ERR_CORRUPT` | Corrupted or invalid input data |
| `GCOMP_ERR_UNSUPPORTED` | Method not found or operation not supported |
| `GCOMP_ERR_INTERNAL` | Internal library error (bug) |
| `GCOMP_ERR_IO` | I/O error (callback API only) |

### Error Detail API

When an error occurs, encoders and decoders store detailed error information that can be queried for debugging:

```c
// Get the last error status
gcomp_status_t gcomp_encoder_get_error(const gcomp_encoder_t *encoder);
gcomp_status_t gcomp_decoder_get_error(const gcomp_decoder_t *decoder);

// Get detailed error message (human-readable)
const char *gcomp_encoder_get_error_detail(const gcomp_encoder_t *encoder);
const char *gcomp_decoder_get_error_detail(const gcomp_decoder_t *decoder);
```

**Example: Checking error details**

```c
gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
if (status != GCOMP_OK) {
    fprintf(stderr, "Decode failed: %s\n", gcomp_status_to_string(status));
    fprintf(stderr, "Details: %s\n", gcomp_decoder_get_error_detail(decoder));
}
```

**Example output:**

```
Decode failed: GCOMP_ERR_CORRUPT
Details: corrupt deflate stream at stage 'huffman_data' (output: 1024 bytes)
```

The error detail string includes:
- The decode stage where the error occurred
- Bytes processed so far (for debugging truncated/corrupt streams)
- Specific error context when available

**Notes:**
- The returned string pointer is valid until the encoder/decoder is destroyed or another operation is performed
- Returns empty string `""` if no error has occurred or if the pointer is NULL
- Error details are set by method implementations (e.g., deflate decoder)

## Callback-Based Streaming

For scenarios where you want the library to drive I/O, use the callback API:

```c
// Callback function types
typedef gcomp_status_t (*gcomp_read_cb)(
    void *ctx,          // User context
    uint8_t *dst,       // Buffer to read into
    size_t cap,         // Buffer capacity
    size_t *out_n       // Output: bytes actually read (0 = EOF)
);

typedef gcomp_status_t (*gcomp_write_cb)(
    void *ctx,          // User context
    const uint8_t *src, // Data to write
    size_t n,           // Number of bytes
    size_t *out_n       // Output: bytes actually written
);
```

**Convenience functions:**

```c
// Encode using callbacks
gcomp_status_t gcomp_encode_stream_cb(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    gcomp_read_cb read_cb, void *read_ctx,
    gcomp_write_cb write_cb, void *write_ctx
);

// Decode using callbacks
gcomp_status_t gcomp_decode_stream_cb(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    gcomp_read_cb read_cb, void *read_ctx,
    gcomp_write_cb write_cb, void *write_ctx
);
```

**Example: File-based compression**

```c
typedef struct {
    FILE *file;
} file_ctx_t;

gcomp_status_t read_file(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    file_ctx_t *fc = (file_ctx_t *)ctx;
    *out_n = fread(dst, 1, cap, fc->file);
    return ferror(fc->file) ? GCOMP_ERR_IO : GCOMP_OK;
}

gcomp_status_t write_file(void *ctx, const uint8_t *src, size_t n, size_t *out_n) {
    file_ctx_t *fc = (file_ctx_t *)ctx;
    *out_n = fwrite(src, 1, n, fc->file);
    return (*out_n < n) ? GCOMP_ERR_IO : GCOMP_OK;
}

// Usage
file_ctx_t in_ctx = { fopen("input.txt", "rb") };
file_ctx_t out_ctx = { fopen("output.deflate", "wb") };

gcomp_encode_stream_cb(NULL, "deflate", NULL,
    read_file, &in_ctx, write_file, &out_ctx);

fclose(in_ctx.file);
fclose(out_ctx.file);
```

## Buffer Convenience Functions

For simple cases where you have all data in memory:

```c
// Encode entire buffer
gcomp_status_t gcomp_encode_buffer(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    const void *input_data, size_t input_size,
    void *output_data, size_t output_capacity,
    size_t *output_size_out
);

// Decode entire buffer
gcomp_status_t gcomp_decode_buffer(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    const void *input_data, size_t input_size,
    void *output_data, size_t output_capacity,
    size_t *output_size_out
);
```

**Example:**

```c
uint8_t compressed[4096];
size_t compressed_size;

gcomp_status_t status = gcomp_encode_buffer(
    NULL, "deflate", NULL,
    input_data, input_size,
    compressed, sizeof(compressed),
    &compressed_size
);

if (status == GCOMP_ERR_LIMIT) {
    // Output buffer too small
}
```

## Thread Safety

- **Encoders/decoders are NOT thread-safe**: Each encoder/decoder should only be used by one thread at a time.
- **Registries**: The default registry is thread-safe for lookups after initialization. Custom registries should not be modified while in use.
- **Options**: Once frozen with `gcomp_options_freeze()`, options objects are safe to share across threads.

## Best Practices

1. **Check all return values** - Every API call can fail
2. **Use error details for debugging** - Call `gcomp_*_get_error_detail()` after failures
3. **Pre-allocate output buffers** - For encoding, output is typically slightly larger than input; for decoding, you need to know or estimate the decompressed size
4. **Set limits for untrusted input** - Use `limits.max_output_bytes` to prevent decompression bombs
5. **Reuse encoders/decoders** - Call `gcomp_*_reset()` between streams instead of destroy/create for better performance
6. **Destroy after use** - Always call `gcomp_*_destroy()` to free resources
