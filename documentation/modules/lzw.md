# LZW method (Lempel–Ziv–Welch)

LZW compression and decompression for the Ghoti.io Compress library. The method name is `"lzw"` and supports stream encode and decode.

The library does not define a canonical container; stream interpretation is **profile-driven**. Two reference profiles are provided:

- **GIF LZW** (`"gif"`): LSB-first bit packing (GIF 89a style)
- **TIFF LZW** (`"tiff"`): MSB-first bit packing (TIFF 6.0 style)

**References:** GIF 89a (LZW image data), TIFF 6.0 (LZW compression).

## Registration

LZW is **auto-registered** with the default registry when the library loads:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/lzw.h>

// lzw is already available
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "lzw", NULL, &enc);
```

For custom registries or when auto-registration is disabled, register explicitly:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);
gcomp_method_lzw_register(custom);
```

See [Auto-Registration](../auto-registration.md) for details.

## Options

### LZW-specific options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `lzw.format` | string | `"gif"` | Profile: `"gif"` (LSB) or `"tiff"` (MSB) |
| `lzw.lit_width` | uint64 | (format default) | Literal width in bits. If unset: 8 for `"gif"`, 9 for `"tiff"` |
| `lzw.max_code_bits` | uint64 | 12 | Maximum code width in bits (max 12 → 4096 entries) |

### Core limit options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output (decoder) |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum working memory |
| `limits.max_expansion_ratio` | uint64 | 0 (unlimited) | Maximum output/input ratio; decompression bomb protection (decoder) |

Limits are enforced by the decoder; exceeding any limit returns `GCOMP_ERR_LIMIT`. Unknown option keys yield `GCOMP_ERR_INVALID_ARG` at create time (schema policy: `GCOMP_UNKNOWN_KEY_ERROR`).

## Format profiles

Both profiles share the same high-level LZW mechanics (dictionary growth, CLEAR reset, EOI termination), but differ in **bit packing order** and **code-width growth rule**.

### Shared semantics (both profiles)

- **Base dictionary:** codes 0–255 represent single-byte literals.
- **CLEAR code:** resets dictionary to literals-only and resets the current code width.
- **EOI code:** marks end-of-information; the decoder requires EOI to complete successfully.
- **Code widths:** variable-width codes from the initial width up to `lzw.max_code_bits` (default 12).
- **KwKwK case:** the decoder implements the standard KwKwK edge case (code equals next unassigned code).

The stream does **not** embed the format identifier; encoder and decoder must use the same `lzw.format`.

### Streaming note

Code packing is bit-level. The encoder/decoder may retain partial-byte state between `update()` calls, so you can stream with arbitrarily sized buffers. The stream is byte-oriented externally (your buffers are bytes), but not necessarily byte-aligned internally.

### GIF (`lzw.format` = `"gif"`)

- **Bit order:** LSB-first (least-significant bit first within the byte stream).
- **Default literal width:** 8.
- **Initial code width:** 9 bits (literals + CLEAR + EOI).
- **Code-width increment rule:** increment when `next_code == 2^current_bits` (GIF rule).

### TIFF (`lzw.format` = `"tiff"`)

- **Bit order:** MSB-first (most-significant bit first within the byte stream).
- **Default literal width:** 9.
- **Initial code width:** 9 bits.
- **Code-width increment rule:** increment when `next_code == 2^current_bits - 1` (TIFF rule).

## Error handling

### Error codes

| Code | Meaning |
|------|---------|
| `GCOMP_ERR_INVALID_ARG` | NULL registry/encoder/decoder; NULL buffer pointer with size > 0; unknown or invalid option |
| `GCOMP_ERR_LIMIT` | Output would exceed `max_output_bytes`; expansion ratio would exceed `max_expansion_ratio`; output buffer too small |
| `GCOMP_ERR_CORRUPT` | Truncated stream, invalid code sequence, missing EOI, etc. |
| `GCOMP_ERR_MEMORY` | Failed to allocate encoder/decoder state |

### Error details

All error returns set detail via `gcomp_encoder_set_error()` / `gcomp_decoder_set_error()`. Use `gcomp_encoder_get_error_detail()` or `gcomp_decoder_get_error_detail()` after a failed call.

## Streaming usage

### Encoding

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_string(opts, "lzw.format", "gif");

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "lzw", opts, &enc);

gcomp_buffer_t in_buf = { input_data, input_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

gcomp_encoder_update(enc, &in_buf, &out_buf);
gcomp_encoder_finish(enc, &out_buf);  // Emits EOI and flushes bit writer

size_t encoded_len = out_buf.used;
gcomp_encoder_destroy(enc);
gcomp_options_destroy(opts);
```

### Decoding

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_string(opts, "lzw.format", "tiff");

// Optional: set limits for untrusted input
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "lzw", opts, &dec);

gcomp_buffer_t in_buf = { encoded_data, encoded_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

gcomp_decoder_update(dec, &in_buf, &out_buf);
gcomp_decoder_finish(dec, &out_buf);  // Requires EOI

gcomp_decoder_destroy(dec);
gcomp_options_destroy(opts);
```

## Reset

Encoder and decoder support `gcomp_encoder_reset()` and `gcomp_decoder_reset()`. Reset clears dictionary state and partial bit state; options are retained. Use reset to reuse the same instance for another stream without destroy/create.

## Security considerations

- For untrusted encoded input, set `limits.max_output_bytes` and `limits.max_expansion_ratio` to avoid decompression bombs.
- Buffer pointer validation: `update()` and `finish()` return `GCOMP_ERR_INVALID_ARG` if `input->data` or `output->data` is NULL when the corresponding size is greater than zero.

## See also

- [Streaming API](../api/streaming.md) - General streaming usage patterns
- [Limits](../api/limits.md) - Safety limit configuration
- [Errors](../api/errors.md) - Error handling patterns
- [Auto-Registration](../auto-registration.md) - How methods are registered

