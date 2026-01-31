# Callback API

The callback API provides a convenient way to process compression streams using read/write callbacks instead of managing buffers directly. This is useful for file I/O, network streams, or any scenario where data sources/sinks are best represented as callbacks.

## Overview

The callback API wraps the lower-level streaming API (`gcomp_encoder_update`/`gcomp_decoder_update`) with automatic buffering and callback invocation. The library handles all buffer management internally.

## Callback Types

### Read Callback

```c
typedef gcomp_status_t (*gcomp_read_cb)(
    void *ctx, uint8_t *dst, size_t cap, size_t *out_n);
```

**Parameters:**
- `ctx`: User-provided context pointer (passed through from the API call)
- `dst`: Buffer to read data into
- `cap`: Capacity of the buffer in bytes
- `out_n`: Output parameter - set to the number of bytes actually read

**Return value:**
- `GCOMP_OK`: Success (check `out_n` for bytes read, 0 indicates EOF)
- `GCOMP_ERR_IO`: I/O error occurred
- Other error codes as appropriate

**Behavior:**
- Read up to `cap` bytes into `dst`
- Set `*out_n` to the number of bytes read
- Return 0 in `*out_n` to indicate end-of-file
- May read fewer than `cap` bytes (partial reads are OK)

### Write Callback

```c
typedef gcomp_status_t (*gcomp_write_cb)(
    void *ctx, const uint8_t *src, size_t n, size_t *out_n);
```

**Parameters:**
- `ctx`: User-provided context pointer (passed through from the API call)
- `src`: Buffer containing data to write
- `n`: Number of bytes to write
- `out_n`: Output parameter - set to the number of bytes actually written

**Return value:**
- `GCOMP_OK`: Success (check `out_n` for bytes written)
- `GCOMP_ERR_IO`: I/O error occurred
- Other error codes as appropriate

**Behavior:**
- Write up to `n` bytes from `src`
- Set `*out_n` to the number of bytes written
- Returning 0 in `*out_n` is treated as an error (returns `GCOMP_ERR_IO`)
- May write fewer than `n` bytes (partial writes are OK, library will retry)

## Functions

### gcomp_encode_stream_cb

```c
gcomp_status_t gcomp_encode_stream_cb(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    gcomp_read_cb read_cb,
    void *read_ctx,
    gcomp_write_cb write_cb,
    void *write_ctx);
```

Compresses data by reading from a callback and writing compressed output to another callback.

**Parameters:**
- `registry`: Method registry (NULL to use default)
- `method_name`: Compression method name (e.g., "gzip", "deflate")
- `options`: Compression options (NULL for defaults)
- `read_cb`: Callback to read uncompressed input
- `read_ctx`: Context passed to read callback
- `write_cb`: Callback to write compressed output
- `write_ctx`: Context passed to write callback

**Return value:**
- `GCOMP_OK`: Compression completed successfully
- `GCOMP_ERR_INVALID_ARG`: Invalid arguments
- `GCOMP_ERR_MEMORY`: Memory allocation failed
- Error codes from callbacks are propagated

### gcomp_decode_stream_cb

```c
gcomp_status_t gcomp_decode_stream_cb(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    gcomp_read_cb read_cb,
    void *read_ctx,
    gcomp_write_cb write_cb,
    void *write_ctx);
```

Decompresses data by reading from a callback and writing decompressed output to another callback.

**Parameters:**
- `registry`: Method registry (NULL to use default)
- `method_name`: Decompression method name (e.g., "gzip", "deflate")
- `options`: Decompression options (NULL for defaults)
- `read_cb`: Callback to read compressed input
- `read_ctx`: Context passed to read callback
- `write_cb`: Callback to write decompressed output
- `write_ctx`: Context passed to write callback

**Return value:**
- `GCOMP_OK`: Decompression completed successfully
- `GCOMP_ERR_INVALID_ARG`: Invalid arguments
- `GCOMP_ERR_CORRUPT`: Corrupted or invalid compressed data
- `GCOMP_ERR_LIMIT`: Safety limit exceeded
- Error codes from callbacks are propagated

## Usage Examples

### Compress a file

```c
#include <ghoti.io/compress/stream.h>
#include <stdio.h>

// Read callback for FILE*
gcomp_status_t file_read(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    FILE *f = (FILE *)ctx;
    size_t n = fread(dst, 1, cap, f);
    if (ferror(f)) {
        return GCOMP_ERR_IO;
    }
    *out_n = n;  // 0 if EOF
    return GCOMP_OK;
}

// Write callback for FILE*
gcomp_status_t file_write(void *ctx, const uint8_t *src, size_t n, size_t *out_n) {
    FILE *f = (FILE *)ctx;
    size_t written = fwrite(src, 1, n, f);
    if (written == 0 && n > 0) {
        return GCOMP_ERR_IO;
    }
    *out_n = written;
    return GCOMP_OK;
}

int compress_file(const char *input_path, const char *output_path) {
    FILE *in = fopen(input_path, "rb");
    FILE *out = fopen(output_path, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return -1;
    }

    gcomp_status_t status = gcomp_encode_stream_cb(
        NULL,           // Use default registry
        "gzip",         // Compression method
        NULL,           // Default options
        file_read, in,  // Read callback and context
        file_write, out // Write callback and context
    );

    fclose(in);
    fclose(out);

    return (status == GCOMP_OK) ? 0 : -1;
}
```

### Decompress a file with options

```c
int decompress_file_with_limits(const char *input_path, const char *output_path) {
    FILE *in = fopen(input_path, "rb");
    FILE *out = fopen(output_path, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return -1;
    }

    // Configure safety limits for untrusted input
    gcomp_options_t *opts = NULL;
    gcomp_options_create(&opts);
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 100 * 1024 * 1024);  // 100 MB
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);

    gcomp_status_t status = gcomp_decode_stream_cb(
        NULL, "gzip", opts,
        file_read, in,
        file_write, out
    );

    gcomp_options_destroy(opts);
    fclose(in);
    fclose(out);

    return (status == GCOMP_OK) ? 0 : -1;
}
```

### In-memory callbacks

```c
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} mem_reader_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
} mem_writer_t;

gcomp_status_t mem_read(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    mem_reader_t *r = (mem_reader_t *)ctx;
    size_t avail = r->size - r->pos;
    size_t n = (avail < cap) ? avail : cap;
    memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    *out_n = n;
    return GCOMP_OK;
}

gcomp_status_t mem_write(void *ctx, const uint8_t *src, size_t n, size_t *out_n) {
    mem_writer_t *w = (mem_writer_t *)ctx;
    size_t avail = w->capacity - w->size;
    size_t to_write = (avail < n) ? avail : n;
    if (to_write == 0 && n > 0) {
        return GCOMP_ERR_LIMIT;  // Buffer full
    }
    memcpy(w->data + w->size, src, to_write);
    w->size += to_write;
    *out_n = to_write;
    return GCOMP_OK;
}

// Usage
void compress_in_memory(const uint8_t *input, size_t input_len,
                        uint8_t *output, size_t output_cap, size_t *output_len) {
    mem_reader_t reader = { input, input_len, 0 };
    mem_writer_t writer = { output, output_cap, 0 };

    gcomp_status_t status = gcomp_encode_stream_cb(
        NULL, "gzip", NULL,
        mem_read, &reader,
        mem_write, &writer
    );

    if (status == GCOMP_OK) {
        *output_len = writer.size;
    }
}
```

## Implementation Notes

- The callback API uses 64 KiB internal buffers by default
- Partial reads and writes are handled automatically
- The library retries writes until all data is written (unless an error occurs)
- A write callback returning 0 bytes written is treated as an error
- All resources (encoder/decoder, internal buffers) are cleaned up on return

## Error Handling

Errors from callbacks are propagated directly to the caller. Common patterns:

```c
gcomp_status_t status = gcomp_decode_stream_cb(
    NULL, "gzip", NULL,
    read_cb, read_ctx,
    write_cb, write_ctx
);

switch (status) {
case GCOMP_OK:
    printf("Success!\n");
    break;
case GCOMP_ERR_CORRUPT:
    printf("Invalid or corrupt compressed data\n");
    break;
case GCOMP_ERR_IO:
    printf("I/O error in callback\n");
    break;
case GCOMP_ERR_LIMIT:
    printf("Safety limit exceeded\n");
    break;
default:
    printf("Error: %s\n", gcomp_status_to_string(status));
    break;
}
```

## See Also

- [Streaming API](streaming.md) - Lower-level buffer-based streaming
- [Error Codes](errors.md) - Complete error code reference
- [Options](../modules/gzip.md) - Gzip-specific options
