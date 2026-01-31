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
| 0 | FTEXT | Never (not used) |
| 1 | FHCRC | `gzip.header_crc` is true |
| 2 | FEXTRA | `gzip.extra` is provided |
| 3 | FNAME | `gzip.name` is provided |
| 4 | FCOMMENT | `gzip.comment` is provided |
| 5-7 | Reserved | Always 0 |

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
