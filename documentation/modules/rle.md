# RLE method (Run-Length Encoding)

Run-Length Encoding (RLE) for the Ghoti.io Compress library. The method name is `"rle"` and supports stream encode and decode. RLE is a simple lossless scheme that replaces runs of identical bytes with a short control token plus the repeated byte. The library does not define a canonical container; stream interpretation is **profile-driven**. Two reference profiles are provided: **PackBits** (TIFF / Apple MacPaint) and **TGA** (Truevision Targa).

**References:** TIFF 6.0 (PackBits), Apple TN1023, Truevision Targa RLE, Utah RLE documentation.

## Registration

RLE is **auto-registered** with the default registry when the library loads:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/rle.h>

// rle is already available
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "rle", NULL, &enc);
```

For custom registries or when auto-registration is disabled, register explicitly:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);
gcomp_method_rle_register(custom);
```

See [Auto-Registration](../auto-registration.md) for details.

## Options

### RLE-specific options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `rle.format` | string | `"packbits"` | Token grammar: `"packbits"` (TIFF/Apple) or `"tga"` (Truevision Targa) |

### Core limit options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output (decoder) |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum working memory |
| `limits.max_expansion_ratio` | uint64 | 0 (unlimited) | Maximum output/input ratio; decompression bomb protection (decoder) |

Limits are enforced by the decoder; exceeding any limit returns `GCOMP_ERR_LIMIT`. Unknown option keys yield `GCOMP_ERR_INVALID_ARG` at create time (schema policy: `GCOMP_UNKNOWN_KEY_ERROR`).

## Format profiles

### PackBits (`rle.format` = `"packbits"`)

- **Control byte:** One byte per token.
  - **0–127:** Next (n+1) bytes are literal (max 128).
  - **128:** No-op; next byte is the next control byte.
  - **129–255:** Interpreted as signed −127…−1; next single byte is repeated (256−n) times (max run 128).
- Used in TIFF and Apple MacPaint.

### TGA (`rle.format` = `"tga"`)

- **Packet header:** One byte. Bit 7 selects packet type.
  - **Bit 7 = 0 (raw):** Bits 0–6 = (count−1); count 1–128; then count literal bytes.
  - **Bit 7 = 1 (run):** Bits 0–6 = (count−1); count 1–128; then one byte repeated count times.
- Used in Truevision Targa RLE.

Encoder and decoder must use the same `rle.format`; the stream has no format identifier.

## Error handling

### Error codes

| Code | Meaning |
|------|---------|
| `GCOMP_ERR_INVALID_ARG` | NULL registry/encoder/decoder; NULL buffer pointer with size > 0; unknown or invalid option |
| `GCOMP_ERR_LIMIT` | Output would exceed `max_output_bytes`; expansion ratio would exceed `max_expansion_ratio` |
| `GCOMP_ERR_CORRUPT` | Reserved for malformed token sequences (profile-dependent) |
| `GCOMP_ERR_MEMORY` | Failed to allocate encoder/decoder state |

### Error details

All error returns set detail via `gcomp_encoder_set_error()` / `gcomp_decoder_set_error()`. Use `gcomp_encoder_get_error_detail()` or `gcomp_decoder_get_error_detail()` after a failed call:

```c
gcomp_status_t s = gcomp_decoder_update(dec, &input, &output);
if (s != GCOMP_OK) {
    printf("Error: %s\n", gcomp_status_to_string(s));
    printf("Detail: %s\n", gcomp_decoder_get_error_detail(dec));
}
```

## Streaming usage

### Encoding

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_string(opts, "rle.format", "packbits");

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "rle", opts, &enc);

gcomp_buffer_t in_buf = { input_data, input_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

gcomp_encoder_update(enc, &in_buf, &out_buf);
gcomp_encoder_finish(enc, &out_buf);  // Flushes any pending literal/run token

size_t encoded_len = out_buf.used;
gcomp_encoder_destroy(enc);
gcomp_options_destroy(opts);
```

### Decoding

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_string(opts, "rle.format", "tga");

// Optional: set limits for untrusted input
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "rle", opts, &dec);

gcomp_buffer_t in_buf = { encoded_data, encoded_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

gcomp_decoder_update(dec, &in_buf, &out_buf);
gcomp_decoder_finish(dec, &out_buf);

gcomp_decoder_destroy(dec);
gcomp_options_destroy(opts);
```

## Reset

Encoder and decoder support `gcomp_encoder_reset()` and `gcomp_decoder_reset()`. Reset clears literal/run accumulation (encoder) or partial-token state (decoder) and error state; options are retained. Use reset to reuse the same instance for another stream without destroy/create.

## Security considerations

- For untrusted encoded input, set `limits.max_output_bytes` and `limits.max_expansion_ratio` to avoid decompression bombs.
- Buffer pointer validation: `update()` and `finish()` return `GCOMP_ERR_INVALID_ARG` if `input->data` or `output->data` is NULL when the corresponding size is greater than zero.

## See also

- [Streaming API](../api/streaming.md) - General streaming usage patterns
- [Limits](../api/limits.md) - Safety limit configuration
- [Errors](../api/errors.md) - Error handling patterns
- [Auto-Registration](../auto-registration.md) - How methods are registered
