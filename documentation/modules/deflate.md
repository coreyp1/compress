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
- **Encode (fixed):** The encoder uses the same fixed code tables as the decoder for levels 1-9.
- **Encode (dynamic):** Not yet implemented; encoder currently falls back to fixed Huffman for all levels > 0.

## Compression levels

| Level | Strategy |
|-------|----------|
| 0 | Stored blocks (no compression, data copied verbatim) |
| 1-3 | Fixed Huffman with LZ77, shorter hash chains |
| 4-6 | Fixed Huffman with LZ77, medium hash chains |
| 7-9 | Fixed Huffman with LZ77, longer hash chains (best compression) |

Higher levels spend more effort searching for matches, improving compression ratio at the cost of speed.

## Streaming usage

### Decoding

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

### Encoding

```c
gcomp_encoder_t * enc = NULL;
gcomp_options_t * opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_int64(opts, "deflate.level", 6);  // 0-9
gcomp_encoder_create(registry, "deflate", opts, &enc);

gcomp_buffer_t in_buf = { input_data, input_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Feed input (can be called multiple times for streaming)
gcomp_status_t s = gcomp_encoder_update(enc, &in_buf, &out_buf);
if (s != GCOMP_OK) { /* handle error */ }

// Finalize (writes final block, flushes to byte boundary)
gcomp_buffer_t finish_buf = { output_array + out_buf.used,
                              output_capacity - out_buf.used, 0 };
s = gcomp_encoder_finish(enc, &finish_buf);
if (s != GCOMP_OK) { /* handle error */ }

size_t total_compressed = out_buf.used + finish_buf.used;

gcomp_encoder_destroy(enc);
gcomp_options_destroy(opts);
```
