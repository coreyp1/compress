# Deflate method (RFC 1951)

Raw DEFLATE compression and decompression for the Ghoti.io Compress library. The method name is `"deflate"` and supports stream encode and decode.

## Options

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `deflate.level` | int64 | 6 | 0..9 | Compression level (0 = none, 9 = best). Encoder only. |
| `deflate.window_bits` | uint64 | 15 | 8..15 | LZ77 window size in bits (max 32 KiB). |
| `limits.max_output_bytes` | uint64 | core default | 0 = unlimited | Max decompressed size (decoder). |
| `limits.max_window_bytes` | uint64 | window size | — | Max window (format-constrained). |

Limits are enforced by the core; the decoder returns `GCOMP_ERR_LIMIT` when `max_output_bytes` is exceeded.

## Limits and security notes

- **Max output:** Set `limits.max_output_bytes` to avoid unbounded decompression (e.g. from untrusted input).
- **Window:** DEFLATE allows up to 32 KiB back-reference distance; `deflate.window_bits` and `limits.max_window_bytes` constrain the decoder.
- **Malformed input:** Invalid block type, stored-block NLEN mismatch, or invalid distance/length can produce `GCOMP_ERR_CORRUPT`.

## Huffman tables

- **Decode (fixed):** Block type 1 uses the fixed literal/length and distance code lengths defined in RFC 1951 §3.2.6.
- **Decode (dynamic):** Block type 2 sends HLIT, HDIST, HCLEN, then code-length alphabet and literal/length and distance lengths; the decoder builds canonical decode tables from these.
- **Encode:** Not yet implemented; encoder will derive code lengths from histograms (fixed or dynamic blocks).

## Streaming usage

```c
gcomp_decoder_t * dec = NULL;
gcomp_decoder_create(registry, "deflate", options, &dec);

gcomp_buffer_t in_buf = { compressed_data, compressed_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

while (/* more input or output space */) {
  gcomp_status_t s = gcomp_decoder_update(dec, &in_buf, &out_buf);
  if (s != GCOMP_OK) { /* handle error */ break; }
  /* consume in_buf.used, use out_buf.used */
}

gcomp_status_t s = gcomp_decoder_finish(dec, &out_buf);
if (s != GCOMP_OK) { /* stream incomplete or corrupt */ }

gcomp_decoder_destroy(dec);
```

Encoder API is the same (`gcomp_encoder_create`, `gcomp_encoder_update`, `gcomp_encoder_finish`, `gcomp_encoder_destroy`); encoder implementation is stubbed and returns `GCOMP_ERR_UNSUPPORTED`.
