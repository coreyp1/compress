# Error Codes

This document describes all error codes returned by the Compress library, when they occur, and how to handle them.

## Error Code Reference

### `GCOMP_OK` (0)

**Meaning:** Operation completed successfully.

**When Returned:**
- All API functions return this on success
- Encoder/decoder operations when data was processed without errors

**Handling:** No action needed; proceed with the next operation.

---

### `GCOMP_ERR_INVALID_ARG` (1)

**Meaning:** One or more arguments passed to a function are invalid.

**When Returned:**
- Null pointer passed where a valid pointer is required
- Invalid buffer size (e.g., size is 0 when data is expected)
- Invalid option key or value type
- Method name not found in registry

**Common Causes:**
```c
// Null pointer
gcomp_encoder_update(NULL, &input, &output);  // encoder is NULL

// Invalid buffer
gcomp_buffer_t buf = {NULL, 100, 0};  // data is NULL but size is non-zero
```

**Handling:** Check all pointers and parameters before calling functions. This is a programming error that should be fixed in the calling code.

---

### `GCOMP_ERR_MEMORY` (2)

**Meaning:** Memory allocation failed.

**When Returned:**
- `malloc()` or similar allocation returned NULL
- Creating encoders/decoders when system is low on memory
- Processing data that requires dynamic allocations

**Common Causes:**
- System out of memory
- Allocating very large buffers
- Memory limit exceeded (see `limits.max_memory_bytes`)

**Handling:**
```c
gcomp_encoder_t *encoder = NULL;
gcomp_status_t status = gcomp_encoder_create(reg, "gzip", opts, &encoder);
if (status == GCOMP_ERR_MEMORY) {
    // Free other resources, reduce buffer sizes, or report error to user
    fprintf(stderr, "Out of memory\n");
    return;
}
```

---

### `GCOMP_ERR_LIMIT` (3)

**Meaning:** A resource limit was exceeded.

**When Returned:**
- Output size exceeds `limits.max_output_bytes`
- Expansion ratio exceeds `limits.max_expansion_ratio`
- Memory usage exceeds `limits.max_memory_bytes`
- Gzip header field exceeds limit (`gzip.max_name_bytes`, `gzip.max_comment_bytes`, `gzip.max_extra_bytes`)

**Common Causes:**
```c
// Decompression bomb protection triggered
// Input: 100 bytes, Output: 100 MB (ratio > default limit)

// Header field too large
// gzip.name = <very long filename>
```

**Handling:**
- For legitimate large data: increase the relevant limit via options
- For untrusted input: treat as potential attack, reject the data

```c
gcomp_options_t *opts;
gcomp_options_create(&opts);
// Allow larger output for trusted sources
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1024 * 1024 * 1024);
```

---

### `GCOMP_ERR_CORRUPT` (4)

**Meaning:** The input data is corrupted or malformed.

**When Returned:**
- Invalid gzip magic bytes (not 0x1F 0x8B)
- Invalid gzip flags (reserved bits set)
- CRC32 checksum mismatch
- ISIZE mismatch (uncompressed size doesn't match)
- Header CRC16 mismatch (when FHCRC is present)
- Invalid Huffman codes in deflate stream
- Truncated data (stream ends unexpectedly)
- Malformed deflate block headers

**Common Causes:**
- File truncation (incomplete download)
- Data transmission errors
- File corruption
- Invalid file format (not actually gzip/deflate)
- Intentionally malformed input (fuzzing, attacks)

**Handling:**
```c
gcomp_status_t status = gcomp_decoder_finish(decoder, &output);
if (status == GCOMP_ERR_CORRUPT) {
    const char *detail = gcomp_decoder_get_error_detail(decoder);
    fprintf(stderr, "Corrupt data: %s\n", detail);
    // Reject the data, do not use partial output
}
```

**Error Detail Examples:**
- `"invalid gzip magic: 0x50 0x4B (expected 0x1F 0x8B)"` - Wrong file format (this is a ZIP file)
- `"gzip CRC32 mismatch: expected 0x12345678, computed 0xABCDEF01"` - Data corrupted after compression
- `"gzip stream truncated in trailer (4 of 8 bytes)"` - Incomplete file
- `"invalid gzip flags: reserved bits set (0xE0)"` - Malformed header

---

### `GCOMP_ERR_UNSUPPORTED` (5)

**Meaning:** The operation or format is not supported.

**When Returned:**
- Gzip compression method is not deflate (CM != 8)
- Requested compression method not registered
- Feature not yet implemented

**Common Causes:**
```c
// Gzip with non-deflate compression
// CM field set to something other than 8

// Using a method that isn't registered
gcomp_encoder_create(registry, "lzma", opts, &encoder);  // LZMA not registered
```

**Handling:** Check if the requested method is registered. For gzip files with unsupported compression methods, the file cannot be processed by this library.

---

### `GCOMP_ERR_INTERNAL` (6)

**Meaning:** An internal library error occurred.

**When Returned:**
- Unexpected state machine state
- Internal consistency check failed
- Bug in the library

**Handling:** This indicates a bug in the library. Please report it with:
- Library version
- Code that reproduces the issue
- Input data (if possible)

---

### `GCOMP_ERR_IO` (7)

**Meaning:** An I/O error occurred.

**When Returned:**
- Callback-based API when read/write callback fails
- Future use for file-based APIs

**Handling:** Check the underlying I/O system for errors (disk full, permission denied, etc.).

---

## Getting Error Details

All encoder and decoder objects maintain detailed error information:

```c
gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
if (status != GCOMP_OK) {
    // Get the error code
    gcomp_status_t error = gcomp_decoder_get_last_error(decoder);
    
    // Get human-readable error name
    const char *name = gcomp_status_to_string(error);
    
    // Get detailed error message
    const char *detail = gcomp_decoder_get_error_detail(decoder);
    
    fprintf(stderr, "Error %s: %s\n", name, detail);
}
```

## Error Recovery

### Encoder Errors

Encoder errors are generally non-recoverable within a single stream. To recover:

1. Destroy the encoder
2. Create a new encoder
3. Start compression from the beginning

### Decoder Errors

Decoder errors from corrupt data are non-recoverable for that stream. Options:

1. **Reject the data:** Safest approach for untrusted input
2. **Reset and retry:** `gcomp_decoder_reset()` clears state for a fresh stream
3. **Skip to next member:** For concatenated gzip, skip the corrupt member

### Memory Errors

Memory errors may be recoverable:

1. Free other resources
2. Reduce buffer sizes
3. Retry with smaller allocations
4. Use streaming with smaller chunks

## Best Practices

1. **Always check return values:** Every API function returns a status code
2. **Use error detail:** Call `gcomp_*_get_error_detail()` for diagnostic information
3. **Don't ignore errors:** Continuing after an error leads to undefined behavior
4. **Set appropriate limits:** Configure limits based on expected data sizes
5. **Validate untrusted input:** Use limits to protect against malicious data
