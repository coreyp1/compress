# Gzip method (RFC 1952)

GZIP compression and decompression for the Ghoti.io Compress library. The method name is `"gzip"` and supports stream encode and decode. Gzip is a wrapper format around the DEFLATE algorithm (RFC 1951), adding a header with metadata and a trailer with CRC32 checksum and original file size.

## Registration

Gzip is **auto-registered** with the default registry when the library loads. No explicit initialization is required:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/gzip.h>

// gzip is already available - just use it
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "gzip", NULL, &enc);
```

For custom registries or when auto-registration is disabled, register explicitly:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);
gcomp_method_deflate_register(custom);  // Must register deflate first
gcomp_method_gzip_register(custom);     // Now gzip is available
```

**Note:** The gzip method requires the deflate method to be registered first. With auto-registration, this is handled automatically.

See [Auto-Registration](../auto-registration.md) for details on disabling auto-registration.

## Architecture

The gzip method is implemented as a **wrapper** around the deflate method. It does not duplicate deflate's compression logic; instead, it delegates all compression and decompression to an inner deflate encoder/decoder while handling the gzip-specific framing.

```
┌────────────────────────────────────────────────────────────────────┐
│                        Gzip Encoder                                │
│  ┌───────────────┐  ┌──────────────────────┐  ┌─────────────────┐  │
│  │  Write Header │──│    Inner Deflate     │──│  Write Trailer  │  │
│  │  (10+ bytes)  │  │      Encoder         │  │    (8 bytes)    │  │
│  └───────────────┘  │                      │  └─────────────────┘  │
│         │           │  - Compresses data   │          │            │
│         │           │  - All compression   │          │            │
│         v           │    logic is here     │          v            │
│   [MTIME, OS,       └──────────────────────┘    [CRC32, ISIZE]     │
│    FNAME, etc.]                                                    │
└────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────┐
│                        Gzip Decoder                                │
│  ┌───────────────┐  ┌──────────────────────┐  ┌─────────────────┐  │
│  │  Parse Header │──│    Inner Deflate     │──│ Validate Trailer│  │
│  │  (streaming)  │  │      Decoder         │  │  CRC32 + ISIZE  │  │
│  └───────────────┘  │                      │  └─────────────────┘  │
│         │           │  - Decompresses data │          │            │
│         │           │  - CRC32 tracked on  │          │            │
│         v           │    decompressed output│          v           │
│   Validates magic,  └──────────────────────┘    Mismatch →         │
│   CM, reserved bits                             GCOMP_ERR_CORRUPT  │
└────────────────────────────────────────────────────────────────────┘
```

### Data flow

**Encoding:**
1. Input data flows to gzip encoder
2. Gzip writes header to output, then passes input to inner deflate encoder
3. CRC32 is computed incrementally on **uncompressed** input
4. Deflate compresses data and writes to output
5. On finish, gzip writes trailer with CRC32 and ISIZE

**Decoding:**
1. Compressed data flows to gzip decoder
2. Gzip parses header, validates magic/CM/flags
3. Remaining data passes to inner deflate decoder
4. CRC32 is computed incrementally on **decompressed** output
5. When deflate completes, gzip reads trailer and validates CRC32/ISIZE

### Option pass-through

Options prefixed with `deflate.*` or `limits.*` are automatically forwarded to the inner deflate encoder/decoder. This allows control over compression level, window size, and memory limits without gzip needing to understand every deflate option.

## Options

### Gzip-specific options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `gzip.mtime` | uint64 | 0 | Modification time as Unix timestamp (seconds since epoch) |
| `gzip.os` | uint64 | 255 | Operating system code (255 = unknown, per RFC 1952) |
| `gzip.name` | string | (none) | Original filename (written to FNAME header field) |
| `gzip.comment` | string | (none) | File comment (written to FCOMMENT header field) |
| `gzip.extra` | bytes | (none) | Extra field data (written to FEXTRA header field) |
| `gzip.header_crc` | bool | false | Include CRC16 of header (FHCRC field) |
| `gzip.xfl` | uint64 | (auto) | Extra flags byte; auto-calculated from compression level if not set |
| `gzip.header_flags` | uint64 | (auto) | Header FLG byte; OR'd with auto-calculated flags (0-255) |
| `gzip.concat` | bool | false | Decoder: support concatenated gzip members |

### Header field size limits (decoder safety)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `gzip.max_name_bytes` | uint64 | 1 MiB | Maximum FNAME length before `GCOMP_ERR_LIMIT` |
| `gzip.max_comment_bytes` | uint64 | 1 MiB | Maximum FCOMMENT length before `GCOMP_ERR_LIMIT` |
| `gzip.max_extra_bytes` | uint64 | 64 KiB | Maximum FEXTRA length before `GCOMP_ERR_LIMIT` |

### Pass-through options (forwarded to deflate)

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `deflate.level` | int64 | 6 | 0..9 | Compression level (0 = none, 9 = best) |
| `deflate.window_bits` | uint64 | 15 | 8..15 | LZ77 window size in bits |
| `deflate.strategy` | string | "default" | see deflate docs | Compression strategy |

### Core limit options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 512 MiB | Max decompressed output (decoder) |
| `limits.max_memory_bytes` | uint64 | 256 MiB | Max working memory |
| `limits.max_expansion_ratio` | uint64 | 1000 | Max output/input ratio (decoder) |

Limits are enforced by the infrastructure; the decoder returns `GCOMP_ERR_LIMIT` when exceeded.

## XFL (Extra Flags) auto-calculation

When `gzip.xfl` is not explicitly set, it is calculated from `deflate.level`:

| Compression Level | XFL Value | Meaning |
|-------------------|-----------|---------|
| 0-2 | 4 | Fastest compression |
| 3-5 | 0 | Default |
| 6-9 | 2 | Maximum compression |

## Header flags (FLG byte)

The FLG byte is auto-calculated based on which optional fields are provided:

| Bit | Flag | Set when |
|-----|------|----------|
| 0 | FTEXT | Never (not used by default) |
| 1 | FHCRC | `gzip.header_crc` is true |
| 2 | FEXTRA | `gzip.extra` is provided |
| 3 | FNAME | `gzip.name` is provided |
| 4 | FCOMMENT | `gzip.comment` is provided |
| 5-7 | Reserved | Always 0 |

### Explicit header flags

The `gzip.header_flags` option allows explicit control over the FLG byte. When set, the value is **OR'd** with the auto-calculated flags, ensuring consistency between the header byte and the actual optional fields present:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Set FTEXT flag (bit 0) to indicate text content
gcomp_options_set_uint64(opts, "gzip.header_flags", 0x01);

// The encoder will compute: FLG = auto_flags | 0x01
// If gzip.name is also set, FLG will have both FNAME (0x08) and FTEXT (0x01)
```

**Note:** Setting explicit flags does not disable auto-calculation. If you set `gzip.header_flags` to a value that conflicts with provided fields (e.g., clearing FNAME when `gzip.name` is provided), the auto-calculated flags will still be OR'd in, ensuring the header correctly reflects the data that follows.

## Concatenated members

Multiple gzip streams can be concatenated into a single file. By default, the decoder stops after the first member. Enable `gzip.concat` to decode all members:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bool(opts, "gzip.concat", 1);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "gzip", opts, &dec);

// Now the decoder will process all concatenated members,
// writing their decompressed output contiguously.
```

**Important behaviors:**
- Each member's CRC32 and ISIZE are validated independently
- Output is continuous across members (no separation markers)
- Limits (`max_output_bytes`, `max_expansion_ratio`) apply to total output across all members
- If any member fails validation, the entire decode fails

## Error handling

### Error codes

| Code | Meaning |
|------|---------|
| `GCOMP_ERR_CORRUPT` | Invalid magic bytes, wrong CM, reserved FLG bits set, CRC32 mismatch, ISIZE mismatch, truncated stream |
| `GCOMP_ERR_UNSUPPORTED` | Compression method not 8 (deflate), or deflate method not registered |
| `GCOMP_ERR_LIMIT` | Header field too large, output limit exceeded, expansion ratio exceeded |
| `GCOMP_ERR_MEMORY` | Failed to allocate decoder/encoder state |

### Error details

When decoding fails, call `gcomp_decoder_get_error_detail()` for diagnostic information:

```c
gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
if (status != GCOMP_OK) {
    printf("Error: %s\n", gcomp_status_to_string(status));
    printf("Detail: %s\n", gcomp_decoder_get_error_detail(decoder));
}
```

**Example error details:**
- `"gzip: invalid magic bytes (expected 1f 8b)"` - Not a gzip file
- `"gzip: unsupported compression method 12 (expected 8)"` - Unknown algorithm
- `"gzip: reserved flags set in FLG byte"` - Malformed header
- `"gzip: FNAME exceeds limit (1048577 > 1048576)"` - Filename too long
- `"gzip: CRC32 mismatch (computed=0x12345678, stored=0xdeadbeef)"` - Data corruption
- `"gzip: ISIZE mismatch (computed=1000, stored=999)"` - Size mismatch
- `"gzip: truncated at trailer"` - Incomplete file

### Decoder stages

The decoder progresses through stages; errors include stage information:

| Stage | Description |
|-------|-------------|
| HEADER | Parsing gzip header (magic, FLG, MTIME, etc.) |
| BODY | Decompressing via inner deflate decoder |
| TRAILER | Reading and validating CRC32 + ISIZE |
| DONE | Stream complete (or ready for next concatenated member) |

## Security considerations

### Decompression bomb protection

The gzip decoder inherits all deflate safety limits plus adds header field limits:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Strict limits for untrusted input
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);  // 10 MB
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);  // 100x max

// Strict header field limits
gcomp_options_set_uint64(opts, "gzip.max_name_bytes", 256);      // 256 byte filename
gcomp_options_set_uint64(opts, "gzip.max_comment_bytes", 1024);  // 1 KB comment
gcomp_options_set_uint64(opts, "gzip.max_extra_bytes", 1024);    // 1 KB extra

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "gzip", opts, &dec);
```

### CRC32 validation

The gzip format includes a CRC32 checksum of the original uncompressed data. The decoder computes this incrementally during decompression and validates against the stored value in the trailer. Any mismatch returns `GCOMP_ERR_CORRUPT`.

### ISIZE validation

The trailer also includes ISIZE (original file size mod 2^32). The decoder validates this to catch truncation or corruption.

## Streaming usage

### Encoding

```c
gcomp_encoder_t *enc = NULL;
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Set gzip-specific options
gcomp_options_set_string(opts, "gzip.name", "myfile.txt");
gcomp_options_set_uint64(opts, "gzip.mtime", (uint64_t)time(NULL));

// Set compression level (passed to deflate)
gcomp_options_set_int64(opts, "deflate.level", 9);

gcomp_encoder_create(registry, "gzip", opts, &enc);

gcomp_buffer_t in_buf = { input_data, input_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Feed input (can be called multiple times)
gcomp_status_t s = gcomp_encoder_update(enc, &in_buf, &out_buf);
if (s != GCOMP_OK) { /* handle error */ }

// Finalize (writes trailer)
gcomp_buffer_t finish_buf = { output_array + out_buf.used,
                              output_capacity - out_buf.used, 0 };
s = gcomp_encoder_finish(enc, &finish_buf);

size_t total_compressed = out_buf.used + finish_buf.used;

gcomp_encoder_destroy(enc);
gcomp_options_destroy(opts);
```

### Decoding

```c
gcomp_decoder_t *dec = NULL;
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Enable concatenated member support if needed
gcomp_options_set_bool(opts, "gzip.concat", 1);

gcomp_decoder_create(registry, "gzip", opts, &dec);

gcomp_buffer_t in_buf = { compressed_data, compressed_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Process all input
while (in_buf.used < in_buf.size || /* output buffer full */) {
    gcomp_status_t s = gcomp_decoder_update(dec, &in_buf, &out_buf);
    if (s != GCOMP_OK) {
        printf("Error: %s - %s\n", 
               gcomp_status_to_string(s),
               gcomp_decoder_get_error_detail(dec));
        break;
    }
    // Process out_buf.used bytes, reset out_buf for more
}

// Verify stream is complete
gcomp_status_t s = gcomp_decoder_finish(dec, &out_buf);
if (s != GCOMP_OK) { /* stream incomplete or corrupt */ }

gcomp_decoder_destroy(dec);
gcomp_options_destroy(opts);
```

## Reset method usage

Both encoder and decoder support a `reset()` method that allows reusing the same instance for multiple independent compression/decompression operations without the overhead of destroy/create cycles.

### Encoder reset

```c
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "gzip", opts, &enc);

// First compression
gcomp_buffer_t in1 = { data1, len1, 0 };
gcomp_buffer_t out1 = { buf1, cap1, 0 };
gcomp_encoder_update(enc, &in1, &out1);
gcomp_encoder_finish(enc, &out1);
// out1 now contains compressed data1

// Reset for reuse (same options retained)
gcomp_encoder_reset(enc);

// Second compression - completely independent
gcomp_buffer_t in2 = { data2, len2, 0 };
gcomp_buffer_t out2 = { buf2, cap2, 0 };
gcomp_encoder_update(enc, &in2, &out2);
gcomp_encoder_finish(enc, &out2);
// out2 now contains compressed data2

gcomp_encoder_destroy(enc);
```

### Decoder reset

```c
gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "gzip", opts, &dec);

// First decompression
gcomp_buffer_t in1 = { compressed1, comp_len1, 0 };
gcomp_buffer_t out1 = { buf1, cap1, 0 };
gcomp_decoder_update(dec, &in1, &out1);
gcomp_decoder_finish(dec, &out1);

// Reset clears all state: CRC32, ISIZE, header info, error state
gcomp_decoder_reset(dec);

// Second decompression - completely independent
gcomp_buffer_t in2 = { compressed2, comp_len2, 0 };
gcomp_buffer_t out2 = { buf2, cap2, 0 };
gcomp_decoder_update(dec, &in2, &out2);
gcomp_decoder_finish(dec, &out2);

gcomp_decoder_destroy(dec);
```

### Reset after error

Reset can recover from error states, allowing continued use after handling a corrupted input:

```c
gcomp_status_t s = gcomp_decoder_update(dec, &bad_input, &out);
if (s == GCOMP_ERR_CORRUPT) {
    // Log error, report to user, etc.
    printf("Error: %s\n", gcomp_decoder_get_error_detail(dec));
    
    // Reset to try another input
    gcomp_decoder_reset(dec);
    
    // Now dec is ready for a new gzip stream
    s = gcomp_decoder_update(dec, &good_input, &out);
}
```

### What reset clears

| State | Encoder | Decoder |
|-------|---------|---------|
| CRC32 counter | ✓ Reset to init | ✓ Reset to init |
| ISIZE counter | ✓ Reset to 0 | ✓ Reset to 0 |
| Stage | ✓ Back to HEADER | ✓ Back to HEADER |
| Header parser | - | ✓ Cleared |
| Parsed header info | - | ✓ Freed and cleared |
| Trailer buffer | ✓ Position reset | ✓ Position reset |
| Total bytes counters | - | ✓ Reset to 0 |
| Error state | ✓ Cleared | ✓ Cleared |
| Inner deflate | ✓ Reset | ✓ Reset |
| Options/config | Retained | Retained |

## Buffer convenience helpers

For simple use cases where the entire input fits in memory, use the buffer convenience API:

```c
#include <ghoti.io/compress/buffer.h>

// Compress data in one call
uint8_t *compressed = NULL;
size_t compressed_len = 0;

gcomp_status_t s = gcomp_buffer_compress(
    registry, "gzip", opts,
    input_data, input_len,
    &compressed, &compressed_len);

if (s == GCOMP_OK) {
    // Use compressed data...
    free(compressed);  // Caller owns the buffer
}

// Decompress data in one call
uint8_t *decompressed = NULL;
size_t decompressed_len = 0;

s = gcomp_buffer_decompress(
    registry, "gzip", opts,
    compressed_data, compressed_len,
    &decompressed, &decompressed_len);

if (s == GCOMP_OK) {
    // Use decompressed data...
    free(decompressed);
}
```

**Note:** The buffer helpers allocate output memory automatically. For large data or streaming scenarios, use the streaming API directly for better memory control.

## Callback API usage

For scenarios where you want to process data incrementally without managing buffers, use the callback API:

```c
#include <ghoti.io/compress/stream_cb.h>

// Output callback - called whenever compressed data is ready
void on_compressed_data(const void *data, size_t len, void *user_data) {
    FILE *out = (FILE *)user_data;
    fwrite(data, 1, len, out);
}

// Create callback encoder
gcomp_encoder_cb_t *enc_cb = NULL;
FILE *output_file = fopen("output.gz", "wb");

gcomp_encoder_cb_create(registry, "gzip", opts,
                        on_compressed_data, output_file,
                        &enc_cb);

// Feed input incrementally - callbacks fire as output is ready
gcomp_encoder_cb_write(enc_cb, chunk1, chunk1_len);
gcomp_encoder_cb_write(enc_cb, chunk2, chunk2_len);
// ... more chunks ...

// Finalize
gcomp_encoder_cb_finish(enc_cb);
gcomp_encoder_cb_destroy(enc_cb);
fclose(output_file);
```

### Decoder callback example

```c
// Output callback - called whenever decompressed data is ready
void on_decompressed_data(const void *data, size_t len, void *user_data) {
    // Process decompressed data incrementally
    process_data(data, len);
}

gcomp_decoder_cb_t *dec_cb = NULL;
gcomp_decoder_cb_create(registry, "gzip", opts,
                        on_decompressed_data, NULL,
                        &dec_cb);

// Feed compressed data - decompressed output delivered via callback
while (has_more_input()) {
    size_t chunk_len;
    const void *chunk = get_next_chunk(&chunk_len);
    gcomp_decoder_cb_write(dec_cb, chunk, chunk_len);
}

gcomp_decoder_cb_finish(dec_cb);
gcomp_decoder_cb_destroy(dec_cb);
```

## Interoperability

The gzip implementation is fully compatible with:
- Standard `gzip` / `gunzip` command-line tools
- Python's `gzip` module
- zlib's gzip functions
- Any RFC 1952-compliant implementation

Files created by this library can be decompressed by standard tools, and files created by standard tools can be decompressed by this library.

## See also

- [Deflate method](deflate.md) - The underlying compression algorithm
- [Streaming API](../api/streaming.md) - General streaming usage patterns
- [Limits](../api/limits.md) - Safety limit configuration
- [Wrapper Methods](../wrapper-methods.md) - How wrapper methods work
