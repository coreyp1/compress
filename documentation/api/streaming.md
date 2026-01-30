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

### Callback Semantics

#### Read Callback

The read callback is called when the library needs more input data.

**Contract:**
- `ctx`: Your user-provided context pointer (e.g., file handle, socket, buffer position)
- `dst`: Buffer to read data into (library-owned, valid for the duration of the call)
- `cap`: Maximum bytes to read (always > 0)
- `out_n`: Must be set to the number of bytes actually read

**Return values:**
- `GCOMP_OK`: Read succeeded. Check `*out_n` for bytes read.
- `GCOMP_ERR_IO`: I/O error occurred (e.g., file read error, network error)
- Other error codes: Treated as fatal errors

**EOF handling:**
- Return `GCOMP_OK` with `*out_n = 0` to indicate end-of-file
- Once EOF is signaled, the callback will not be called again

**Partial reads:**
- Returning fewer bytes than `cap` is always valid (the library will call again if needed)
- This allows for non-blocking I/O or reading from slow sources

#### Write Callback

The write callback is called when the library has output data to write.

**Contract:**
- `ctx`: Your user-provided context pointer
- `src`: Data to write (library-owned, valid for the duration of the call)
- `n`: Number of bytes to write (always > 0)
- `out_n`: Must be set to the number of bytes actually written

**Return values:**
- `GCOMP_OK`: Write succeeded. Check `*out_n` for bytes written.
- `GCOMP_ERR_IO`: I/O error occurred
- Other error codes: Treated as fatal errors

**Partial writes:**
- Returning fewer bytes than `n` is valid (the library retries with remaining data)
- Returning `*out_n = 0` with `GCOMP_OK` is treated as `GCOMP_ERR_IO` (no progress)

**Backpressure:**
- The library automatically handles partial writes by retrying
- All output is eventually written before returning to the caller

### Error Propagation

Errors from callbacks are propagated directly to the caller:

```c
gcomp_status_t my_read(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    // Return GCOMP_ERR_IO on failure
    if (read_failed) {
        return GCOMP_ERR_IO;
    }
    // ...
}

// This error propagates to the caller
gcomp_status_t status = gcomp_encode_stream_cb(..., my_read, ...);
// status == GCOMP_ERR_IO if my_read returned GCOMP_ERR_IO
```

**Cleanup on error:**
- The library always cleans up internal resources (encoder/decoder, buffers) on error
- You are responsible for cleaning up your callback context

### Example: File-Based Compression

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
    return (*out_n < n && ferror(fc->file)) ? GCOMP_ERR_IO : GCOMP_OK;
}

// Usage
file_ctx_t in_ctx = { fopen("input.txt", "rb") };
file_ctx_t out_ctx = { fopen("output.deflate", "wb") };

gcomp_status_t status = gcomp_encode_stream_cb(NULL, "deflate", NULL,
    read_file, &in_ctx, write_file, &out_ctx);

fclose(in_ctx.file);
fclose(out_ctx.file);

if (status != GCOMP_OK) {
    fprintf(stderr, "Compression failed: %s\n", gcomp_status_to_string(status));
}
```

### Example: Memory Buffer with Partial Reads

```c
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    size_t chunk_size;  // Simulate slow reads
} chunked_reader_t;

gcomp_status_t read_chunked(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    chunked_reader_t *r = (chunked_reader_t *)ctx;
    
    if (r->pos >= r->size) {
        *out_n = 0;  // EOF
        return GCOMP_OK;
    }
    
    // Read at most chunk_size bytes at a time
    size_t remaining = r->size - r->pos;
    size_t to_read = (remaining < r->chunk_size) ? remaining : r->chunk_size;
    if (to_read > cap) to_read = cap;
    
    memcpy(dst, r->data + r->pos, to_read);
    r->pos += to_read;
    *out_n = to_read;
    return GCOMP_OK;
}
```

### Internal Buffer Size

The callback API uses internal 64 KiB buffers for reading and writing. This means:
- Your read callback will be asked to read up to 64 KiB at a time
- Your write callback will receive up to 64 KiB at a time

This is an implementation detail and may change in future versions.

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

## Resource Limits

The library provides several safety limits to protect against resource exhaustion from malicious or malformed inputs:

| Option | Default | Description |
|--------|---------|-------------|
| `limits.max_output_bytes` | 512 MiB | Maximum decompressed output size |
| `limits.max_memory_bytes` | 256 MiB | Maximum working memory for decoder |
| `limits.max_expansion_ratio` | 1000 | Maximum output/input byte ratio |

### Expansion ratio protection

The `limits.max_expansion_ratio` option protects against "decompression bombs" - maliciously crafted archives that decompress to massive outputs from tiny inputs (e.g., a 42 KB file that expands to 4.5 PB).

The ratio is calculated as `output_bytes / input_bytes`. With the default limit of 1000:
- 1 KB input → max 1 MB output
- 1 MB input → max 1 GB output

**Example: Setting limits for untrusted input**

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Strict limits for untrusted data
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);  // 10 MB
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);            // 100x max

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "deflate", opts, &dec);
// ... use decoder ...
gcomp_decoder_destroy(dec);
gcomp_options_destroy(opts);
```

When a limit is exceeded, the decoder returns `GCOMP_ERR_LIMIT`. Call `gcomp_decoder_get_error_detail()` to get diagnostic information about which limit was hit.

## Best Practices

1. **Check all return values** - Every API call can fail
2. **Use error details for debugging** - Call `gcomp_*_get_error_detail()` after failures
3. **Pre-allocate output buffers** - For encoding, output is typically slightly larger than input; for decoding, you need to know or estimate the decompressed size
4. **Set limits for untrusted input** - Use `limits.max_output_bytes` and `limits.max_expansion_ratio` to prevent decompression bombs
5. **Reuse encoders/decoders** - Call `gcomp_*_reset()` between streams instead of destroy/create for better performance
6. **Destroy after use** - Always call `gcomp_*_destroy()` to free resources
