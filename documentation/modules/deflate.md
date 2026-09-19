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
| `"lazy"` | Defers every match one byte to see whether the next position starts a longer one, at every level rather than from level 4 up. Good for PNG filter output, where a match one byte later is often longer, and worth 2.4% at level 1 over the 19 MB corpus measured below. Identical to `"default"` from level 4 up, shortest-path levels included. |
| `"huffman_only"` | Skip LZ77 matching entirely; emit all bytes as literals. Very fast encoding, minimal compression. Useful for already-compressed or high-entropy data where LZ77 would find few matches. |
| `"rle"` | Run-length encoding mode: only find matches at distance 1. Very fast, limited compression. Best for data with long runs of repeated bytes. |
| `"fixed"` | Always use fixed Huffman tables, skipping both the tree build and the pricing that would choose between them. Faster encoding at the cost of compression ratio. It is the only way to force fixed codes - no level does so - and it can be combined with any level. |

**Example: Using strategies**

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// For PNG filter output, or any data where deferring a match pays.
// This only changes anything at levels 1 to 3: from level 4 up the
// default parse already defers, and from 7 up both parse by shortest path.
gcomp_options_set_string(opts, "deflate.strategy", "lazy");
gcomp_options_set_int64(opts, "deflate.level", 3);

// For already-compressed data (JPEG inside a container)
gcomp_options_set_string(opts, "deflate.strategy", "huffman_only");

// For data with many byte runs (simple graphics, sparse data)
gcomp_options_set_string(opts, "deflate.strategy", "rle");

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "deflate", opts, &enc);
```

**Strategy selection guidelines:**

- **General data**: Use `"default"` (or omit the option).
- **PNG images**: Use `"lazy"` at levels 1 to 3, where it buys deferral the default parse does not do; above that the level already defers or parses by shortest path.
- **Pre-compressed data**: Use `"huffman_only"` to avoid wasting CPU on futile LZ77 searches.
- **Simple patterns**: Use `"rle"` for data dominated by repeated byte runs.
- **Speed-critical**: Use a low level first - level 1 is the fastest parse - and `"fixed"` or `"huffman_only"` on top of it to drop the entropy-coding work as well.

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

### Empty distance tree edge case

RFC 1951 permits dynamic Huffman blocks where the distance tree is empty (all zero code lengths). This occurs when the encoder outputs only literals and no LZ77 matches, which can happen with:

- Very short inputs (not enough data to find matches)
- Incompressible high-entropy data
- `"huffman_only"` strategy (no LZ77 matching)

The decoder correctly handles this edge case: the distance tree is only accessed when decoding a length code (symbols 257-285), and if no such codes appear in the compressed data, an empty distance tree is valid.

**Required invariant:** The literal/length tree must always contain the end-of-block symbol (256). A stream missing this symbol is rejected as corrupt.

## Compression levels

A level sets two things that are independent of each other: how hard the match
finder looks, and how the parse decides which of the matches it found to
actually emit. The second is the larger difference between the bands.

| Level | Hash chain | Parse | Use case |
|-------|-----------:|-------|----------|
| 0 | - | none; blocks are stored | data copied verbatim |
| 1 | 4 | greedy - take the first match found | fastest |
| 2 | 8 | greedy | |
| 3 | 16 | greedy | |
| 4 | 16 | deferred - the first level to defer | |
| 5 | 32 | deferred | |
| 6 | 128 | deferred | **default** |
| 7 | 64 | **shortest path**, 32 shortenings per position | first level to parse by shortest path |
| 8 | 96 | shortest path, 64 shortenings | |
| 9 | 128 | shortest path, 128 shortenings | best ratio |

**Greedy** takes each match as it is found. **Deferred** (what zlib calls lazy
matching) holds a match back to see whether the next position starts a longer
one. **Shortest path** does neither: it treats the block as a graph, one vertex
per byte position and one edge per literal and per match, prices every edge in
256ths of a bit from the block's own symbol statistics, and emits the cheapest
path through it. Because DEFLATE has no literal-length code - a literal is just
a symbol in the same alphabet as the lengths, RFC 1951 section 3.2.5 - the cost
of a path is exactly the sum of its edges, so the sweep finds the true minimum
for the prices it is given. `src/methods/deflate/deflate_encode.c` carries the
derivation.

Note that the chain at level 7 is *shorter* than at level 6. The shortest-path
parse asks for a list of candidate matches at every position rather than the
single longest one, so it pays for the chain many more times; the levels were
re-tuned against that, not inherited from the deferring band.

### What it costs and buys

Measured 2026-09-18 over 19,181,880 bytes of source, manuals, XML, CSV,
binaries and images, against **zlib 1.3.1** at the same level, in one run of
`bench/bench_ratio` so that both sides saw the same machine. Negative means our
output is smaller. Encode speed is MiB of input per second; sizes are exact,
speeds good to about five percent.

| Level | Ours | zlib | Delta | Encode | zlib encode |
|-------|-----:|-----:|------:|-------:|------------:|
| 1 | 5,388,771 | 5,788,889 | **-6.91%** | 78.7 MiB/s | 143.5 MiB/s |
| 6 | 4,865,580 | 4,861,938 | +0.07% | 18.8 MiB/s | 36.6 MiB/s |
| 7 | 4,688,422 | 4,831,806 | **-2.97%** | 5.5 MiB/s | 26.3 MiB/s |
| 9 | 4,666,795 | 4,793,634 | **-2.65%** | 3.8 MiB/s | 9.4 MiB/s |

The shortest-path levels cost three to five times level 6's encode time - 5.5
MiB/s at level 7 and 3.8 at level 9, against 18.8 - and buy about 3%. Whether
that trade is worth making is the caller's decision, which is why it is a level
rather than the default.

**Level and strategy interaction:** The `deflate.strategy` option modifies how matching works at each level:

- `"default"`: the level's own parse - greedy at 1 to 3, deferred at 4 to 6, shortest path at 7 to 9.
- `"lazy"`: same hash chains as default; it differs only in deferring matches at levels 1 to 3, where default takes the first match it finds. At 4 and above it is default, shortest-path levels included.
- `"huffman_only"`: no LZ77 at all, so there is no parse for the level to choose; only the entropy coding applies.
- `"rle"`: ignores hash chains entirely; only checks distance-1 matches, and so has no parse to replace either.
- `"fixed"`: forces fixed Huffman codes. This is the only thing that does; see below. Matching still follows the level, shortest path included.

### Huffman coding: priced, not assumed

**Every level builds a code from its own block's frequencies.** Which coding a
block gets is decided by pricing both: the dynamic block's cost including its
table description, and the same symbols under the fixed code of RFC 1951
section 3.2.6, with the smaller one written. Extra bits are identical under
both, so they are left out of both sides.

No level makes this call, because no level can: a short block, or one whose
symbols are near uniform, pays more for the table than the table saves, and
that depends on the block rather than on the effort setting. Only
`deflate.strategy = "fixed"` forces the fixed code.

This is a change from earlier versions, where levels 1 to 3 always emitted
fixed blocks. Pricing them instead is worth 17.1% at level 1 over a 19 MB
corpus - the last 0.56% of that being the blocks flushed at the *end* of a
stream, which kept the old behaviour after the streaming loop had dropped it.

At every level the encoder:

1. **Collects frequency histograms** during LZ77 matching for:
   - Literal bytes (0-255) and length codes (257-285)
   - Distance codes (0-29)

2. **Builds optimal length-limited codes** by boundary package-merge
   (Larmore and Hirschberg, JACM 37(3), 1990), in `src/core/huffman_lengths.c`:
   - Creates codes no longer than the 15 bits RFC 1951 allows
   - Optimal *under* that cap, rather than a plain Huffman tree clamped to it
     and repaired: clamping cannot see which lengthening costs fewest bits, so
     it spends the budget in the wrong place
   - Falls back to uniform 8-bit codes on memory allocation failure

3. **Encodes the Huffman trees** in the block header using:
   - Run-length encoding with symbols 16 (repeat previous), 17 (short zero run), 18 (long zero run)
   - A secondary Huffman tree for the code-length alphabet itself

4. **Writes compressed data** using the dynamic codes, which typically achieve better compression than fixed Huffman for varied input data.

The fixed code is written whenever it prices smaller, which is the same rule stated above rather than a fallback.

## Flushing

`gcomp_encoder_flush()` emits every symbol for the input consumed so far
without ending the stream. See [Streaming API](../api/streaming.md#flushing)
for the general contract.

DEFLATE blocks do not end on byte boundaries, so a flush ends the current
block and then writes the empty stored block RFC 1951 §3.2.4 describes:
`BFINAL=0`, `BTYPE=00`, padding to the next byte, then `LEN=0x0000` and
`NLEN=0xFFFF`. That is the familiar `00 00 FF FF` tail, and it is what stops
the decoder reading the padding bits as the next block's header. Both sides
come out of it byte-aligned.

At level 0 no marker is needed: stored blocks are already byte-aligned and
self-terminating, so emitting the buffered data is the whole flush.

`GCOMP_FLUSH_FULL` additionally empties the match history — the hash chains,
and the window that the RLE strategy consults directly — so nothing written
afterwards refers to anything before the flush.

Anything still in the lookahead becomes literals, because there is no further
input to match it against. Those bytes are still entered into the hash chains,
so a later match can find them; only the flushed block itself pays.

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
