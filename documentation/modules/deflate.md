# Deflate method (RFC 1951)

Raw DEFLATE compression and decompression for the Ghoti.io Compress library. The method name is `"deflate"` and supports stream encode and decode.

## Registration

Deflate is **auto-registered** with the default registry when the library loads. No explicit initialization is required:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>

// deflate is already available - just use it
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "deflate", NULL, &enc);
```

For custom registries or when auto-registration is disabled, register explicitly:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);
gcomp_method_deflate_register(custom);  // Now deflate is available
```

See [Auto-Registration](../auto-registration.md) for details on disabling auto-registration.

## Options

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `deflate.level` | int64 | 6 | 0..9 | Compression level (0 = none, 9 = best). Encoder only. |
| `deflate.strategy` | string | "default" | see below | Compression strategy. Encoder only. |
| `deflate.window_bits` | uint64 | 15 | 8..15 | LZ77 window size in bits (max 32 KiB). |
| `limits.max_output_bytes` | uint64 | 512 MiB | 0 = unlimited | Max decompressed size (decoder). |
| `limits.max_memory_bytes` | uint64 | 256 MiB | 0 = unlimited | Max working memory (decoder). |
| `limits.max_window_bytes` | uint64 | window size | — | Max window (format-constrained). |
| `limits.max_expansion_ratio` | uint64 | 1000 | 0 = unlimited | Max output/input ratio (decoder). |

Limits are enforced by the core; the decoder returns `GCOMP_ERR_LIMIT` when any limit is exceeded.

### Strategy option values

| Strategy | Description |
|----------|-------------|
| `"default"` | Standard LZ77 + Huffman compression. Best for most data types. |
| `"filtered"` | Optimized for pre-filtered data (e.g., PNG filter output). Uses longer hash chains and more aggressive lazy matching to find better matches in data with specific statistical properties. |
| `"huffman_only"` | Skip LZ77 matching entirely; emit all bytes as literals. Very fast encoding, minimal compression. Useful for already-compressed or high-entropy data where LZ77 would find few matches. |
| `"rle"` | Run-length encoding mode: only find matches at distance 1. Very fast, limited compression. Best for data with long runs of repeated bytes. |
| `"fixed"` | Always use fixed Huffman tables (skip dynamic tree building). Faster encoding at the cost of compression ratio. Equivalent to low compression levels but can be combined with any level. |

**Example: Using strategies**

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// For PNG image data (pre-filtered)
gcomp_options_set_string(opts, "deflate.strategy", "filtered");
gcomp_options_set_int64(opts, "deflate.level", 9);

// For already-compressed data (JPEG inside a container)
gcomp_options_set_string(opts, "deflate.strategy", "huffman_only");

// For data with many byte runs (simple graphics, sparse data)
gcomp_options_set_string(opts, "deflate.strategy", "rle");

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "deflate", opts, &enc);
```

**Strategy selection guidelines:**

- **General data**: Use `"default"` (or omit the option).
- **PNG images**: Use `"filtered"` with high compression level for best results.
- **Pre-compressed data**: Use `"huffman_only"` to avoid wasting CPU on futile LZ77 searches.
- **Simple patterns**: Use `"rle"` for data dominated by repeated byte runs.
- **Speed-critical**: Use `"fixed"` or `"huffman_only"` to minimize encoding time.

## Error Handling

The deflate decoder provides detailed error information when decoding fails. This is useful for debugging corrupt or truncated streams.

### Error codes

| Code | Meaning |
|------|---------|
| `GCOMP_ERR_CORRUPT` | Invalid block type, NLEN mismatch, invalid Huffman tree, distance beyond window, etc. |
| `GCOMP_ERR_LIMIT` | `limits.max_output_bytes` or `limits.max_memory_bytes` exceeded |
| `GCOMP_ERR_MEMORY` | Failed to allocate decoder state or Huffman tables |

### Error details

When decoding fails, call `gcomp_decoder_get_error_detail()` to get a human-readable message with context:

```c
gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
if (status != GCOMP_OK) {
    printf("Error: %s\n", gcomp_status_to_string(status));
    printf("Detail: %s\n", gcomp_decoder_get_error_detail(decoder));
}
```

**Example error details:**

- `"corrupt deflate stream at stage 'block_header' (output: 0 bytes)"` - Invalid block type
- `"corrupt deflate stream at stage 'stored_len' (output: 0 bytes)"` - NLEN mismatch in stored block
- `"corrupt deflate stream at stage 'huffman_data' (output: 1024 bytes)"` - Invalid distance or length code
- `"limit exceeded at stage 'huffman_data' (output: 1048576/1048576 bytes)"` - Max output limit reached
- `"incomplete deflate stream (stage 'huffman_data', expected final block)"` - Stream truncated

### Decoder stages

The error detail includes the decoder stage where the error occurred:

| Stage | Description |
|-------|-------------|
| `block_header` | Reading BFINAL and BTYPE bits |
| `stored_len` | Reading LEN/NLEN for stored block |
| `stored_copy` | Copying stored block data |
| `dynamic_header` | Reading HLIT/HDIST/HCLEN |
| `dynamic_codelen` | Reading code-length-lengths |
| `dynamic_lengths` | Decoding literal/distance code lengths |
| `huffman_data` | Decoding compressed symbols |
| `done` | Stream complete |

## Limits and security notes

### Decompression bomb protection

The deflate decoder implements multiple layers of protection against malicious inputs:

- **Max output:** Set `limits.max_output_bytes` to cap absolute decompressed size. Default: 512 MiB.
- **Expansion ratio:** Set `limits.max_expansion_ratio` to limit the ratio of output to input bytes. Default: 1000x (meaning 1 KB compressed → max 1 MB decompressed). This catches "zip bombs" where tiny inputs decompress to massive outputs.
- **Memory limits:** Set `limits.max_memory_bytes` to bound decoder working memory. Default: 256 MiB.

**Example: Strict limits for untrusted input**

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Max 10 MB output
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);

// Max 100x expansion ratio (stricter than default 1000x)
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);

// Max 1 MB working memory
gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1024 * 1024);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "deflate", opts, &dec);
```

**Example: Disable expansion ratio limit for known-good data**

```c
// For trusted data with extreme compression (e.g., all-zeros test data)
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0);  // 0 = unlimited
```

### Other security considerations

- **Window:** DEFLATE allows up to 32 KiB back-reference distance; `deflate.window_bits` and `limits.max_window_bytes` constrain the decoder.
- **Malformed input:** Invalid block type, stored-block NLEN mismatch, or invalid distance/length can produce `GCOMP_ERR_CORRUPT`.
- **Error details:** Call `gcomp_decoder_get_error_detail()` for diagnostic information when errors occur.

## Huffman tables

- **Decode (fixed):** Block type 1 uses the fixed literal/length and distance code lengths defined in RFC 1951 §3.2.6.
- **Decode (dynamic):** Block type 2 sends HLIT, HDIST, HCLEN, then code-length alphabet and literal/length and distance lengths; the decoder builds canonical decode tables from these.
- **Encode (fixed):** Levels 1-3 use the fixed Huffman code tables defined in RFC 1951.
- **Encode (dynamic):** Levels 4-9 build optimal Huffman codes from symbol frequency histograms collected during LZ77 matching. The encoder generates length-limited (15-bit max) codes and transmits them using the code-length alphabet with run-length encoding.

## Compression levels

| Level | Huffman Mode | LZ77 Effort | Use Case |
|-------|--------------|-------------|----------|
| 0 | Stored blocks | None | No compression (data copied verbatim) |
| 1-3 | Fixed | Short hash chains | Fast compression, reasonable ratio |
| 4-6 | Dynamic | Medium hash chains | Balanced speed and compression |
| 7-9 | Dynamic | Long hash chains | Best compression, slower |

Higher levels spend more effort searching for matches and build optimal Huffman codes from actual symbol frequencies, improving compression ratio at the cost of speed.

**Level and strategy interaction:** The `deflate.strategy` option modifies how matching works at each level:

- `"default"`: Uses the standard LZ77 algorithm with hash chain length determined by level.
- `"filtered"`: Uses longer hash chains than default at each level (16/128/256 vs 8/32/64).
- `"huffman_only"`: Ignores level for matching (no LZ77), but level still affects Huffman mode.
- `"rle"`: Ignores hash chains entirely; only checks distance-1 matches.
- `"fixed"`: Forces fixed Huffman codes regardless of level (skips dynamic tree building).

### Dynamic Huffman encoding (levels 4-9)

At compression levels 4 and above, the encoder:

1. **Collects frequency histograms** during LZ77 matching for:
   - Literal bytes (0-255) and length codes (257-285)
   - Distance codes (0-29)

2. **Builds optimal Huffman trees** using a heap-based algorithm:
   - Creates length-limited codes (max 15 bits per RFC 1951)
   - Uses Kraft inequality validation for code validity
   - Falls back to uniform 8-bit codes on memory allocation failure

3. **Encodes the Huffman trees** in the block header using:
   - Run-length encoding with symbols 16 (repeat previous), 17 (short zero run), 18 (long zero run)
   - A secondary Huffman tree for the code-length alphabet itself

4. **Writes compressed data** using the dynamic codes, which typically achieve better compression than fixed Huffman for varied input data.

The encoder automatically falls back to fixed Huffman blocks if the dynamic tree would be larger than the savings.

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
