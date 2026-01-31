# Troubleshooting Guide

This guide covers common issues encountered when using the Ghoti.io Compress library and their solutions.

## Common Error Codes

### GCOMP_ERR_CORRUPT

**Symptom:** Decoder returns `GCOMP_ERR_CORRUPT` with a message about invalid data.

**Common causes and solutions:**

1. **Invalid magic bytes** (gzip)
   - Error: `"invalid gzip magic: 0xXX 0xXX (expected 0x1F 0x8B)"`
   - Cause: The input is not a gzip file or is truncated at the beginning
   - Solution: Verify the input is actually gzip-compressed data

2. **CRC32 mismatch**
   - Error: `"gzip CRC32 mismatch: expected 0xXXXXXXXX, computed 0xXXXXXXXX"`
   - Cause: Data corruption during transmission or storage
   - Solution: Re-download or recover the original file

3. **ISIZE mismatch**
   - Error: `"gzip ISIZE mismatch: expected N, computed M"`
   - Cause: File truncation or corruption in the trailer
   - Solution: Verify file integrity, check for partial downloads

4. **Invalid block type** (deflate)
   - Error: `"invalid deflate block type 3"`
   - Cause: Corrupted deflate stream
   - Solution: Verify the compressed data is valid

5. **Reserved flags set** (gzip)
   - Error: `"invalid gzip flags: reserved bits set"`
   - Cause: Non-standard gzip file or corruption
   - Solution: The file may use unsupported extensions

**Debugging corrupt streams:**

```c
gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "gzip", NULL, &dec);

gcomp_buffer_t in = { data, len, 0 };
gcomp_buffer_t out = { buffer, capacity, 0 };

gcomp_status_t s = gcomp_decoder_update(dec, &in, &out);
if (s != GCOMP_OK) {
    // Get detailed error information
    printf("Error: %s\n", gcomp_status_to_string(s));
    printf("Detail: %s\n", gcomp_decoder_get_error_detail(dec));
    printf("Bytes consumed: %zu\n", in.used);
    printf("Bytes produced: %zu\n", out.used);
}
```

### GCOMP_ERR_LIMIT

**Symptom:** Decoder returns `GCOMP_ERR_LIMIT`.

**Common causes and solutions:**

1. **Output size exceeded**
   - Error: `"output size exceeds limit"`
   - Cause: Decompressed size exceeds `limits.max_output_bytes`
   - Solution: Increase the limit or process the data in smaller chunks

   ```c
   gcomp_options_t *opts = NULL;
   gcomp_options_create(&opts);
   gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1024 * 1024 * 1024); // 1 GB
   ```

2. **Expansion ratio exceeded**
   - Error: `"expansion ratio exceeds limit"`
   - Cause: Possible decompression bomb or highly compressed data
   - Solution: For legitimate data, increase `limits.max_expansion_ratio`

   ```c
   gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 10000);
   ```

3. **Header field too large** (gzip)
   - Error: `"gzip FNAME exceeds limit"`
   - Cause: Very long filename in gzip header
   - Solution: Increase `gzip.max_name_bytes` limit

### GCOMP_ERR_MEMORY

**Symptom:** Operations fail with memory allocation errors.

**Common causes and solutions:**

1. **Insufficient system memory**
   - Solution: Reduce `limits.max_memory_bytes` to fail fast
   - Use streaming API to process data incrementally

2. **Memory limit exceeded**
   - Error: `"memory usage exceeds limit"`
   - Solution: Increase `limits.max_memory_bytes` if needed

### GCOMP_ERR_UNSUPPORTED

**Symptom:** Method or feature is not available.

**Common causes and solutions:**

1. **Method not registered**
   - Error: `"method 'xyz' not found"`
   - Solution: Register the method or use the default registry

   ```c
   gcomp_registry_t *reg = gcomp_registry_default();  // Has all methods
   ```

2. **Unsupported compression method** (gzip)
   - Error: `"unsupported compression method N"`
   - Cause: Gzip file uses a compression method other than deflate (8)
   - Solution: This library only supports deflate compression

## Streaming Issues

### Output buffer fills up

**Symptom:** `update()` returns `GCOMP_OK` but no progress is made.

**Solution:** Process the output buffer and call `update()` again:

```c
while (input.used < input.size) {
    gcomp_buffer_t out = { buffer, capacity, 0 };
    status = gcomp_decoder_update(dec, &input, &out);
    if (status != GCOMP_OK) break;
    
    // Process out.used bytes
    process_output(buffer, out.used);
}
```

### Incomplete decompression

**Symptom:** `finish()` returns an error about truncated stream.

**Causes:**
- Input data is incomplete
- Not all input was consumed before calling `finish()`

**Solution:** Ensure all input is consumed before finishing:

```c
while (input.used < input.size || output_buffer_was_full) {
    status = gcomp_decoder_update(dec, &input, &out);
    if (status != GCOMP_OK) break;
    // ... process output
}

// Now finish
status = gcomp_decoder_finish(dec, &out);
```

### Encoder produces no output

**Symptom:** `update()` consumes input but produces no output.

**Cause:** Deflate compression buffers data internally.

**Solution:** This is normal. Output is produced when:
- Internal buffer fills
- `finish()` is called
- Sufficient input accumulates for efficient compression

## Performance Issues

### Slow compression

**Possible causes and solutions:**

1. **High compression level**
   - Reduce `deflate.level` for faster compression (levels 1-3 are fastest)

   ```c
   gcomp_options_set_int64(opts, "deflate.level", 1);  // Fastest
   ```

2. **Small buffer sizes**
   - Use larger input/output buffers to reduce call overhead

   ```c
   // Use at least 64KB buffers for best performance
   uint8_t buffer[65536];
   ```

3. **Window size too large**
   - For small data, reduce `deflate.window_bits`

### Memory usage too high

**Solutions:**

1. Set explicit memory limit:
   ```c
   gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 16 * 1024 * 1024);
   ```

2. Use streaming with small buffers for memory-constrained environments

3. Lower window size reduces dictionary memory:
   ```c
   gcomp_options_set_uint64(opts, "deflate.window_bits", 12);  // 4KB window
   ```

## Platform-Specific Issues

### Windows line endings

**Symptom:** Text files have different CRC32 after compression/decompression.

**Cause:** Line ending conversion elsewhere in the pipeline.

**Solution:** Treat compressed data as binary, not text:
- Use `"rb"` / `"wb"` modes for file I/O
- Don't let other tools modify compressed data

### Large file support

**Symptom:** Files larger than 4GB cause issues.

**Note:** ISIZE in gzip trailer is mod 2^32, so files >4GB have wrapped ISIZE values. This is normal per RFC 1952.

## Debugging Tips

### Enable detailed error messages

```c
gcomp_status_t s = gcomp_decoder_update(dec, &in, &out);
if (s != GCOMP_OK) {
    fprintf(stderr, "Error code: %d (%s)\n", s, gcomp_status_to_string(s));
    fprintf(stderr, "Detail: %s\n", gcomp_decoder_get_error_detail(dec));
}
```

### Verify compressed data with external tools

```bash
# Check gzip file validity
gzip -t file.gz

# Decompress with verbose output
gzip -dv file.gz

# Check deflate stream with Python
python3 -c "import zlib; print(zlib.decompress(open('file.deflate','rb').read(),-15))"
```

### Binary inspection

```bash
# View gzip header
xxd file.gz | head -5

# Expected gzip header:
# 1f 8b - magic
# 08    - compression method (deflate)
# XX    - flags
# XX XX XX XX - mtime
# XX    - XFL
# XX    - OS
```

### Validate round-trip

```c
// Compress and decompress, verify data matches
auto compressed = compress(original_data, len);
auto decompressed = decompress(compressed.data(), compressed.size());
assert(decompressed.size() == len);
assert(memcmp(decompressed.data(), original_data, len) == 0);
```

## Getting Help

If you encounter issues not covered here:

1. Check the [API documentation](api/) for correct usage
2. Review [error codes](api/errors.md) for detailed error descriptions
3. Examine the test suite for working examples
4. Run with sanitizers to detect memory issues:
   ```bash
   make test-asan
   ```

## See Also

- [Error Codes Reference](api/errors.md)
- [Building Guide](building.md)
- [Testing Guide](testing/testing.md)
- [Fuzzing Documentation](testing/fuzzing.md)
