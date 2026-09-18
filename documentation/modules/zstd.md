# Zstd method (Zstandard Framed Streams)

Zstandard compression and decompression for the Ghoti.io Compress library. The method name is `"zstd"` and supports stream encode and decode. This implementation follows the Zstandard compression format specification, providing high compression ratios with excellent decompression speed.

**Reference:** [Zstandard compression format specification (RFC 8878)](https://www.rfc-editor.org/rfc/rfc8878.html)

## Registration

Zstd is **auto-registered** with the default registry when the library loads. No explicit initialization is required:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/zstd.h>

// zstd is already available - just use it
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "zstd", NULL, &enc);
```

For custom registries or when auto-registration is disabled, register explicitly:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);
gcomp_method_zstd_register(custom);  // Now zstd is available
```

See [Auto-Registration](../auto-registration.md) for details on disabling auto-registration.

## Architecture

The Zstd method is a **standalone** compression method (not a wrapper). It implements the complete Zstandard frame format, which provides:

- Frame header with configuration flags and optional content size
- Multiple data blocks (raw, RLE, or compressed)
- Block-level entropy coding using FSE (Finite State Entropy) and Huffman
- Sequence-based LZ compression with repeat offset optimization
- Optional content checksum (xxHash64, low 32 bits) in trailer

```
┌────────────────────────────────────────────────────────────────────┐
│                      Zstd Frame Structure                          │
│  ┌─────────────┐  ┌─────────────────────────┐  ┌─────────────────┐ │
│  │   Header    │  │        Data Blocks      │  │    Trailer      │ │
│  │  (6-14 B)   │  │                         │  │   (0-4 B)       │ │
│  └─────────────┘  └─────────────────────────┘  └─────────────────┘ │
│        │                    │                         │            │
│        v                    v                         v            │
│   Magic (4B)           Block 1:                 Content checksum   │
│   Frame Header          - Header (3B)            (if enabled)      │
│   Descriptor (1B)       - Data (var)                               │
│   [Window Desc]        Block 2...                                  │
│   [Dict ID]            Last Block (last=1)                         │
│   [Content Size]                                                   │
└────────────────────────────────────────────────────────────────────┘
```

### Block types

| Type | Value | Description |
|------|-------|-------------|
| Raw | 0 | Uncompressed data, copied directly |
| RLE | 1 | Run-length encoded: single byte repeated |
| Compressed | 2 | Entropy-coded literals + LZ sequences |
| Reserved | 3 | Invalid, returns `GCOMP_ERR_CORRUPT` |

### Compressed block structure

Compressed blocks contain two sections:

1. **Literals Section**: The literal bytes (non-matched data)
   - Can be raw, RLE, or Huffman-compressed
   - Huffman trees can be inline or reuse previous tree

2. **Sequences Section**: LZ match instructions
   - Each sequence: (literal_length, match_offset, match_length)
   - Encoded using FSE (Finite State Entropy) with predefined or custom tables
   - Supports repeat offsets for improved compression

```
┌─────────────────────────────────────────────────────────────┐
│                    Compressed Block                         │
│  ┌──────────────────────┐  ┌──────────────────────────────┐ │
│  │   Literals Section   │  │      Sequences Section       │ │
│  │  (header + data)     │  │  (header + FSE bitstream)    │ │
│  └──────────────────────┘  └──────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Options

### Zstd-specific options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `zstd.level` | int64 | 3 | Compression level (1-22). Higher = better compression, slower |
| `zstd.checksum` | bool | false | Enable content checksum (xxHash64, low 32 bits) |
| `zstd.window_log` | uint64 | 0 (auto) | Window log size (10-31). 0 = auto from level |
| `zstd.dictionary` | bytes | (none) | Optional dictionary (raw content or formatted per RFC 8878 §5) |
| `zstd.dictionary_id` | uint32 | (none) | Dictionary ID to write (encoder) or validate (decoder); used when dictionary provided |
| `zstd.content_size` | uint64 | (none) | Content size to write in header (optional) |
| `zstd.concat` | bool | false | Decoder: support concatenated frames |
| `zstd.job_size` | uint64 | 0 (auto) | Encoder: job size for parallel compression (64KB–16MB, 0=auto) |

### Threading options (encoder only)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `threads.count` | uint64 | 1 | Number of worker threads (0 or 1 = single-threaded) |

When `threads.count > 1`, the encoder uses parallel compression where input is split into independent jobs. See [Parallel Compression](#parallel-compression) below.

### Core limit options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 512 MiB | Maximum decompressed output (decoder) |
| `limits.max_window_bytes` | uint64 | (derived) | Maximum window size |
| `limits.max_memory_bytes` | uint64 | 256 MiB | Maximum working memory |
| `limits.max_expansion_ratio` | uint64 | 1000 | Maximum output/input ratio (decoder) |

Limits are enforced by the infrastructure; the decoder returns `GCOMP_ERR_LIMIT` when exceeded.

### Compression level guidelines

RFC 8878 says nothing about levels: it describes the frame, not how to choose
what goes in it. A level here is a promise about how much work the encoder will
spend, and it sets three things - what the match finder searches, how the parse
picks from what it finds, and how large a window it declares.

| Levels | Search | Parse | Window | Use case |
|--------|--------|-------|-------:|----------|
| 1 | hash chain, 4 candidates | greedy | 128 KB | real-time, logging |
| 2-3 | hash chain, 8-16 | deferred | 128 KB | general purpose (**default: 3**) |
| 4-6 | hash chain, 24-64 | deferred | 512 KB | |
| 7-8 | hash chain, 80-112 | deferred | 2 MB | the fast end of the chain |
| 9-10 | **binary tree**, 14-16 | deferred | 2 MB | where the tree takes over |
| 11-15 | binary tree, 18-26 | deferred | 8 MB | archival |
| 16-22 | binary tree, 28-64 | **shortest path** | 8 MB at 16, 32 MB from 17 | maximum compression |

**The candidate counts either side of level 9 are not comparable.** A hash-chain
step crosses off one position and learns nothing about the next, so searching
harder means walking further and the counts run to the hundreds. A binary-tree
step halves what is left, and a descent takes about six steps - so 14 candidates
at level 9 is *more* search than 112 at level 8, not less.

**Deferred** parsing holds a match back to see whether the next position starts a
longer one. **Shortest path** does not defer at all: it prices every candidate
match and every literal in 256ths of a bit, from statistics primed off the
predefined FSE distributions of RFC 8878 section 3.1.1.3.2.2 and the block's own
byte histogram, and walks the cheapest path through the block. Because a zstd
sequence carries a literal-length code, a path's cost is not exactly the sum of
its edges, so this is a very good approximation rather than a proof of optimality;
`src/methods/zstd/zstd_optimal.c` says where the approximation lies.

Levels 9 and 10 pay for the tree in memory: two slots per position where the
chain had one, 16 MiB against 8.5 over the 2 MB window they declare.

### What it costs and buys

Measured 2026-09-18 over 19,181,880 bytes of source, manuals, XML, CSV, binaries
and images, against **libzstd 1.5.7** at the same level, in one run of
`bench/bench_ratio` so that both sides saw the same machine. Negative means our
output is smaller. Speed is MiB of input per second; sizes are exact, speeds
good to about five percent.

| Level | Ours | libzstd | Delta | Encode | libzstd encode |
|-------|-----:|--------:|------:|-------:|---------------:|
| 1 | 5,029,655 | 5,223,057 | **-3.70%** | 108.6 MiB/s | 531.9 MiB/s |
| 3 | 4,693,228 | 4,831,204 | **-2.86%** | 55.5 MiB/s | 401.7 MiB/s |
| 9 | 4,334,525 | 4,341,823 | **-0.17%** | 11.2 MiB/s | 85.7 MiB/s |
| 16 | 3,907,753 | 3,959,522 | **-1.31%** | 5.4 MiB/s | 7.7 MiB/s |
| 19 | 3,881,087 | 3,836,814 | +1.15% | 5.1 MiB/s | 4.3 MiB/s |

A corpus total is an average. Per file these span a wider range than the totals
suggest - level 1 runs from -9.4% on prose to +7.2% on a skewed-alphabet file -
so measure on your own data before choosing a level on ratio alone.

### Window size

The window size controls how far back the compressor can reference previous
data. Larger windows compress better and cost more memory.

| Window Log | Window Size | Where it is used |
|------------|-------------|------------------|
| 17 | 128 KB | levels 1-3 |
| 19 | 512 KB | levels 4-6 |
| 21 | 2 MB | levels 7-10 |
| 23 | 8 MB | levels 11-16 |
| 25 | 32 MB | levels 17-22 |
| 31 | 2 GB | maximum the format allows; reachable only by setting `zstd.window_log` |

When `zstd.window_log=0` (the default) the window comes from the level, as
above. Setting it explicitly overrides the level, and the decoder enforces
`limits.max_window_bytes` against whatever the frame declares.

## Content checksum

When `zstd.checksum=true`, the encoder computes an xxHash64 checksum over all uncompressed content. The low 32 bits are written to the frame trailer.

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bool(opts, "zstd.checksum", 1);

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "zstd", opts, &enc);
// ... encode data ...
// Checksum is automatically computed and written to trailer
```

The decoder validates the checksum if present. A mismatch returns `GCOMP_ERR_CORRUPT`.

## Content size

The encoder can write the uncompressed content size in the frame header:

```c
// Encoder: write content size in header
gcomp_options_set_uint64(opts, "zstd.content_size", 1000);

// Decoder will validate: decompressed size must equal 1000 bytes
```

This enables:
- Progress reporting during decompression
- Pre-allocation of output buffers
- Validation that all data was received

## Dictionary compression

When compressing or decompressing many small files with similar structure (e.g. log lines, JSON records), a **dictionary** improves ratio by providing shared initial context. The encoder uses the dictionary as initial window history; the decoder preloads the same content so matches can be resolved.

### Options

| Option | Encoder | Decoder |
|--------|---------|---------|
| `zstd.dictionary` | bytes: raw or formatted dictionary (RFC 8878 §5) | Same: must match encoder’s dictionary |
| `zstd.dictionary_id` | uint32: ID written in frame header (optional) | Validated against frame header when present |

### Encoder usage

```c
// Build or load a dictionary (e.g. from samples)
uint8_t *dict_bytes = ...;
size_t dict_len = ...;

gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bytes(opts, "zstd.dictionary", dict_bytes, dict_len);
gcomp_options_set_uint32(opts, "zstd.dictionary_id", 1);  // optional

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "zstd", opts, &enc);
// ... encode data; frame header will include dictionary ID when set ...
```

### Decoder usage

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bytes(opts, "zstd.dictionary", dict_bytes, dict_len);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", opts, &dec);
// ... decode; window is preloaded with dictionary content ...
```

If the frame header specifies a dictionary ID and the decoder was created without a dictionary (or with a different ID for formatted dictionaries), the decoder returns `GCOMP_ERR_UNSUPPORTED` or `GCOMP_ERR_CORRUPT` with a message indicating the required dictionary ID. Raw dictionaries (no magic) are accepted for any frame dictionary ID to support external encoders that use content-derived IDs.

### Dictionary format

- **Raw**: Any bytes, no magic. `dict_id` is 0; encoder can still set `zstd.dictionary_id` in the header for identification.
- **Formatted** (RFC 8878 §5): Magic 0xEC30A437, 4-byte dictionary ID, entropy tables (Huffman, FSE), repeat offsets, then content. Encoder and decoder can use the entropy tables and repeat offsets from the dictionary for the first block.

## Parallel compression

The Zstd encoder supports parallel compression for improved throughput on multi-core systems. When enabled, input data is split into independent jobs that are compressed concurrently.

### Enabling parallel compression

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_uint64(opts, "threads.count", 4);  // Use 4 worker threads

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "zstd", opts, &enc);
```

### How it works

1. Input data is accumulated until `zstd.job_size` bytes are buffered (default: auto-calculated based on compression level)
2. Each full job is submitted to a thread pool for compression
3. Each job produces a complete, independent Zstd frame
4. Compressed frames are collected in order and written to the output
5. The final output is valid concatenated Zstd frames

```
┌───────────────────────────────────────────────────────────────────────┐
│                    Parallel Compression Flow                          │
│                                                                       │
│  Input Stream:    ────────────────────────────────────────────────►   │
│                   │  job_size  │  job_size  │  job_size  │ partial │  │
│                                                                       │
│  Worker Threads:  ┌────────┐   ┌────────┐   ┌────────┐                │
│                   │ Thread │   │ Thread │   │ Thread │   ...          │
│                   │   1    │   │   2    │   │   3    │                │
│                   └────────┘   └────────┘   └────────┘                │
│                        ↓           ↓           ↓                      │
│  Output:          [Frame 1]   [Frame 2]   [Frame 3]   [Frame 4]       │
│                   ◄───────────────────────────────────────────────    │
│                         (concatenated, ordered output)                │
└───────────────────────────────────────────────────────────────────────┘
```

### Job size configuration

| `zstd.job_size` | Behavior |
|-----------------|----------|
| 0 (default) | Auto-calculated: ~4× window size, minimum 64KB |
| 64KB - 16MB | Use specified size |
| < 64KB | Clamped to 64KB minimum |

Larger job sizes provide better compression (more context for the match finder) but reduce parallelism. Smaller job sizes enable more parallelism but may reduce compression ratio slightly.

### Output format

Parallel compression produces **concatenated Zstd frames**. Each frame is complete and valid:

- Standard tools (`zstd`, `unzstd`) can decompress the output directly
- When decoding with this library, enable `zstd.concat=true` to decode all frames

```c
// Decoding parallel-compressed output
gcomp_options_t *dec_opts = NULL;
gcomp_options_create(&dec_opts);
gcomp_options_set_bool(dec_opts, "zstd.concat", true);  // Required for multi-frame

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", dec_opts, &dec);
```

### Single-threaded fallback

When `threads.count` is 0 or 1, the encoder operates in single-threaded mode:

- No threading overhead
- Produces a single Zstd frame (not concatenated)
- Standard streaming compression behavior

### Memory usage

Parallel compression uses additional memory:
- Per-job input buffer (`job_size` bytes each)
- Per-job output buffer (up to `job_size + overhead` bytes)
- Match finder state per in-flight job

The encoder respects `limits.max_memory_bytes` by limiting the number of concurrent in-flight jobs.

### Error handling

If any worker thread encounters an error:
- The error is propagated to the main encoder
- Subsequent `update()` or `finish()` calls return the error
- All in-flight jobs are cleaned up

## Concatenated frames

Multiple Zstd frames can be concatenated into a single stream. By default, the decoder stops after the first frame. Enable `zstd.concat` to decode all frames:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bool(opts, "zstd.concat", 1);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", opts, &dec);

// Now the decoder will process all concatenated frames,
// writing their decompressed output contiguously.
```

**Important behaviors:**
- Each frame's checksum is validated independently
- Output is continuous across frames (no separation markers)
- Limits apply to total output across all frames
- If any frame fails validation, the entire decode fails

## Error handling

### Error codes

| Code | Meaning |
|------|---------|
| `GCOMP_ERR_CORRUPT` | Invalid magic, bad checksums, malformed blocks, invalid sequences |
| `GCOMP_ERR_LIMIT` | Output/window/memory/expansion limits exceeded |
| `GCOMP_ERR_MEMORY` | Failed to allocate encoder/decoder state or buffers |
| `GCOMP_ERR_UNSUPPORTED` | Unsupported feature (e.g., dictionary required) |

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
- `"invalid zstd magic number"` - Not a Zstd frame
- `"reserved bit set in frame header"` - Corrupted header
- `"reserved block type"` - Invalid block type
- `"invalid match offset"` - Sequence references before window start
- `"content checksum mismatch"` - Data corruption detected
- `"output size exceeds limit"` - Output limit reached
- `"expansion ratio exceeds limit"` - Decompression bomb detected

## Streaming usage

### Encoding

```c
gcomp_encoder_t *enc = NULL;
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Configure options
gcomp_options_set_int64(opts, "zstd.level", 5);
gcomp_options_set_bool(opts, "zstd.checksum", 1);

gcomp_encoder_create(registry, "zstd", opts, &enc);

gcomp_buffer_t in_buf = { input_data, input_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Feed input (can be called multiple times)
gcomp_status_t s = gcomp_encoder_update(enc, &in_buf, &out_buf);
if (s != GCOMP_OK) { /* handle error */ }

// Finalize (writes last block and optional checksum)
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

// Enable concatenated frame support if needed
gcomp_options_set_bool(opts, "zstd.concat", 1);

gcomp_decoder_create(registry, "zstd", opts, &dec);

gcomp_buffer_t in_buf = { compressed_data, compressed_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Process all input
while (in_buf.used < in_buf.size) {
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

## Security considerations

### Decompression bomb protection

The Zstd decoder implements multiple layers of protection against malicious inputs:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Strict limits for untrusted input
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);  // 10 MB
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);  // 100x max
gcomp_options_set_uint64(opts, "limits.max_window_bytes", 4 * 1024 * 1024);  // 4 MB

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", opts, &dec);
```

### Checksum validation

- **Content checksum**: Validated after all data is decompressed
- **Content size**: Validated at end of frame if present in header

### Memory limits

The decoder tracks memory usage and enforces `limits.max_memory_bytes`. This includes:
- Decoder state structure
- Window buffer (history)
- FSE decoding tables
- Huffman decoding table
- Block buffers

## Entropy coding

Zstd uses two entropy coding methods for efficient data representation:

### FSE (Finite State Entropy)

FSE is an asymmetric numeral system (ANS) variant used for encoding sequences (literal lengths, match lengths, and offsets). It provides near-optimal compression with fast decoding.

**Predefined tables**: For common data patterns, Zstd uses predefined FSE tables for:
- Literal lengths (36 symbols, accuracy log 6)
- Match lengths (53 symbols, accuracy log 6)
- Match offsets (29 symbols, accuracy log 5)

**Custom tables**: For better compression, the encoder can include custom FSE tables in the bitstream.

### Huffman coding

Huffman coding is used for literals (the non-matched bytes in compressed blocks). The encoder automatically selects the best encoding:

| Literals Encoding | When Used |
|-------------------|-----------|
| Raw | Small inputs (<32 bytes), or when compression provides <10% savings |
| Huffman compressed | Skewed byte distribution where compression is beneficial |

**Encoder behavior:**
1. Count symbol frequencies in the literal bytes
2. Build optimal Huffman tree from frequencies
3. Estimate compressed size (weights + bitstream)
4. Use Huffman only if savings exceed 10%; otherwise use raw literals

**Format requirements (RFC 8878 Section 4.2.1.2):**
- Maximum code length: 11 bits
- **Weight description**: One header byte then weight data.
  - **Header byte ≥ 128**: Direct representation. Number_Of_Symbols = header_byte − 127 (1–128 symbols). Weights follow as 4-bit values packed high nibble first.
  - **Header byte < 128**: FSE-compressed weights. header_byte = compressed size in bytes. Followed by FSE table header and backward FSE bitstream encoding the weight sequence (used when the Huffman table has more than 127 symbols).
- Bitstream written backwards with marker bit for decoder synchronization

**Four-stream mode (RFC 8878 Section 4.2.1.4):**
- When the literals section has **regenerated_size ≥ 1024**, the encoder uses four parallel Huffman streams: a 3×2-byte jump table (little-endian) followed by the concatenated streams. The decoder supports both single-stream and four-stream formats.

**Implementation notes:**
> The 32-byte minimum and 10% savings threshold are encoder heuristics in this implementation, not format requirements. The Zstd specification allows encoders complete freedom to choose raw, RLE, or Huffman for any literals section. Other Zstd encoders (e.g., the reference `libzstd`) may use different decision criteria.

**Decoding:**
- Single-stream mode: Sequential decoding (literals section < 1024 bytes)
- Four-stream mode: Parallel decoding with jump table (literals section ≥ 1024 bytes); encoder and decoder both support this format

## Interoperability

This implementation is fully compatible with:
- Standard `zstd` / `unzstd` command-line tools
- Python's `zstandard` module
- libzstd library
- Any RFC 8878-compliant implementation

Files created by this library can be decompressed by standard tools, and files created by standard tools can be decompressed by this library.

**Features not yet implemented:**
- Dictionary compression (frame header dictionary ID field is parsed but dictionaries are not used)

## Comparison with other methods

| Feature | Zstd | LZ4 | Gzip |
|---------|------|-----|------|
| Compression ratio | High | Low | Medium |
| Compression speed | Fast | Fastest | Slow |
| Decompression speed | Very Fast | Fastest | Medium |
| Memory usage | Moderate | Higher | Lower |
| Streaming | Yes | Yes | Yes |
| Content checksum | xxHash64 | xxHash32 | CRC32 |
| Compression levels | 1-22 | N/A | 1-9 |

**When to use Zstd:**
- Need high compression ratio with fast decompression
- Variable compression/speed tradeoff is desired
- Modern format with good tooling support is preferred
- Content integrity verification is needed

## See also

- [Streaming API](../api/streaming.md) - General streaming usage patterns
- [Limits](../api/limits.md) - Safety limit configuration
- [Errors](../api/errors.md) - Error handling patterns
- [Auto-Registration](../auto-registration.md) - How methods are registered
