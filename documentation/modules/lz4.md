# LZ4 method (LZ4 Frame Format)

LZ4 Frame Format compression and decompression for the Ghoti.io Compress library. The method name is `"lz4"` and supports stream encode and decode. This implementation follows the LZ4 Frame Format specification, which provides framing around LZ4 block compression with optional checksums and metadata.

**Reference:** [LZ4 Frame Format specification](https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md)

## Registration

LZ4 is **auto-registered** with the default registry when the library loads. No explicit initialization is required:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/lz4.h>

// lz4 is already available - just use it
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "lz4", NULL, &enc);
```

For custom registries or when auto-registration is disabled, register explicitly:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);
gcomp_method_lz4_register(custom);  // Now lz4 is available
```

See [Auto-Registration](../auto-registration.md) for details on disabling auto-registration.

## Architecture

The LZ4 method is a **standalone** compression method (not a wrapper). It implements the complete LZ4 Frame Format, which provides:

- Frame header with configuration flags
- Multiple data blocks with optional per-block checksums
- Optional content checksum in trailer
- Support for both independent and dependent blocks

```
┌────────────────────────────────────────────────────────────────────┐
│                        LZ4 Frame Structure                         │
│  ┌─────────────┐  ┌─────────────────────────┐  ┌─────────────────┐ │
│  │   Header    │  │       Data Blocks       │  │    Trailer      │ │
│  │  (7-19 B)   │  │                         │  │   (0-4 B)       │ │
│  └─────────────┘  └─────────────────────────┘  └─────────────────┘ │
│        │                    │                         │            │
│        v                    v                         v            │
│   Magic (4B)           Block 1:                 Content checksum   │
│   FLG byte              - Size (4B)              (if enabled)      │
│   BD byte               - Data (var)                               │
│   [Content Size]        - Checksum (4B opt)                        │
│   [Dict ID]            Block 2...                                  │
│   Header Checksum      End Mark (4B of 0x00)                       │
└────────────────────────────────────────────────────────────────────┘
```

### Header structure

| Field | Size | Description |
|-------|------|-------------|
| Magic Number | 4 bytes | `0x184D2204` (little-endian) |
| FLG | 1 byte | Flags: version, block independence, checksums |
| BD | 1 byte | Block descriptor: maximum block size |
| Content Size | 0 or 8 bytes | Original uncompressed size (if flag set) |
| Dictionary ID | 0 or 4 bytes | Dictionary identifier (if flag set) |
| Header Checksum | 1 byte | xxHash32 of descriptor bytes, masked |

### Block independence

LZ4 supports two block modes:

- **Independent blocks** (`lz4.independent_blocks=true`, default): Each block is compressed independently. Back-references cannot cross block boundaries. This mode enables parallel decompression and random access.

- **Dependent blocks** (`lz4.independent_blocks=false`): Blocks can reference data from previous blocks. Achieves better compression ratio but requires sequential processing.

## Options

### LZ4-specific options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `lz4.block_size` | uint64 | 4194304 | Block size in bytes. Must be one of: 65536 (64KB), 262144 (256KB), 1048576 (1MB), or 4194304 (4MB) |
| `lz4.block_checksum` | bool | false | Enable per-block xxHash32 checksum |
| `lz4.content_checksum` | bool | false | Enable content xxHash32 checksum in frame trailer |
| `lz4.independent_blocks` | bool | true | Use independent blocks (parallel-friendly) |
| `lz4.content_size` | uint64 | (none) | Content size to write in header (encoder); validated on decode if present |
| `lz4.dictionary_id` | uint64 | (none) | Dictionary ID to write in header (parsing only, dictionaries not yet supported) |
| `lz4.concat` | bool | false | Decoder: support concatenated LZ4 frames |

### Core limit options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 512 MiB | Maximum decompressed output (decoder) |
| `limits.max_block_bytes` | uint64 | 4 MiB | Maximum block size during decode |
| `limits.max_memory_bytes` | uint64 | 256 MiB | Maximum working memory |
| `limits.max_expansion_ratio` | uint64 | 1000 | Maximum output/input ratio (decoder) |

Limits are enforced by the infrastructure; the decoder returns `GCOMP_ERR_LIMIT` when exceeded.

### Block size selection

The block size affects compression ratio, memory usage, and parallelism:

| Block Size | Memory per Block | Use Case |
|------------|------------------|----------|
| 64 KB | ~128 KB | Memory-constrained environments, small files |
| 256 KB | ~512 KB | Balanced memory/compression for moderate files |
| 1 MB | ~2 MB | Good compression for larger files |
| 4 MB | ~8 MB | Best compression ratio (default) |

**Note:** Memory usage includes the block buffer plus hash table for the encoder, and block buffer plus output buffer for the decoder.

## Checksums

### Block checksum

When `lz4.block_checksum=true`, each block includes a 4-byte xxHash32 checksum of the compressed block data. This allows detection of corruption at the block level.

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bool(opts, "lz4.block_checksum", 1);

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "lz4", opts, &enc);
```

### Content checksum

When `lz4.content_checksum=true`, the frame trailer includes a 4-byte xxHash32 checksum of the entire uncompressed content. This provides end-to-end integrity verification.

```c
gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
```

### Content size validation

When the encoder writes a content size in the header (via `lz4.content_size`), the decoder validates that the actual decompressed size matches. A mismatch returns `GCOMP_ERR_CORRUPT`.

```c
// Encoder: write content size in header
gcomp_options_set_uint64(opts, "lz4.content_size", 1000);

// Decoder will validate: decompressed size must equal 1000 bytes
```

## Concatenated frames

Multiple LZ4 frames can be concatenated into a single stream. By default, the decoder stops after the first frame. Enable `lz4.concat` to decode all frames:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bool(opts, "lz4.concat", 1);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "lz4", opts, &dec);

// Now the decoder will process all concatenated frames,
// writing their decompressed output contiguously.
```

**Important behaviors:**
- Each frame's checksums are validated independently
- Output is continuous across frames (no separation markers)
- Limits (`max_output_bytes`, `max_expansion_ratio`) apply to total output across all frames
- If any frame fails validation, the entire decode fails

## Error handling

### Error codes

| Code | Meaning |
|------|---------|
| `GCOMP_ERR_CORRUPT` | Invalid magic bytes, bad header checksum, invalid block data, checksum mismatch, content size mismatch |
| `GCOMP_ERR_LIMIT` | Output limit exceeded, block size exceeded, expansion ratio exceeded, memory limit exceeded |
| `GCOMP_ERR_MEMORY` | Failed to allocate encoder/decoder state or buffers |

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
- `"invalid magic number"` - Not an LZ4 frame
- `"invalid FLG version (expected 01)"` - Unsupported format version
- `"header checksum mismatch"` - Corrupted header
- `"invalid match offset"` - Block data corruption (offset 0 or out of bounds)
- `"lz4 block checksum mismatch: expected 0x..., computed 0x..."` - Block corruption
- `"lz4 content checksum mismatch: expected 0x..., computed 0x..."` - Content corruption
- `"lz4 content size mismatch: header specified N bytes, got M bytes"` - Size mismatch
- `"lz4 output size N exceeds limit M"` - Output limit reached
- `"lz4 expansion ratio N exceeds limit M"` - Decompression bomb detected

### Decoder stages

The decoder progresses through stages; errors include stage context:

| Stage | Description |
|-------|-------------|
| HEADER | Parsing frame header (magic, FLG, BD, optional fields, checksum) |
| BLOCK_SIZE | Reading 4-byte block size |
| BLOCK_DATA | Decompressing block content |
| BLOCK_CHECKSUM | Validating block checksum (if enabled) |
| CONTENT_CHECKSUM | Validating content checksum (if enabled) |
| DONE | Frame complete (or ready for next concatenated frame) |

## Security considerations

### Decompression bomb protection

The LZ4 decoder implements multiple layers of protection against malicious inputs:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Strict limits for untrusted input
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);  // 10 MB
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);  // 100x max
gcomp_options_set_uint64(opts, "limits.max_block_bytes", 65536);  // 64 KB blocks max

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "lz4", opts, &dec);
```

### Checksum validation

- **Block checksums**: Detected during block processing, before decompressed data is returned
- **Content checksum**: Validated after all data is decompressed; mismatch reported at frame end
- **Content size**: Validated at end of frame; mismatch returns `GCOMP_ERR_CORRUPT`

### Memory limits

The decoder tracks memory usage and enforces `limits.max_memory_bytes`. This includes:
- Decoder state structure
- Block input buffer
- Decompressed output buffer
- History buffer (for dependent blocks)

## Streaming usage

### Encoding

```c
gcomp_encoder_t *enc = NULL;
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Configure LZ4 options
gcomp_options_set_uint64(opts, "lz4.block_size", 262144);  // 256 KB blocks
gcomp_options_set_bool(opts, "lz4.content_checksum", 1);

gcomp_encoder_create(registry, "lz4", opts, &enc);

gcomp_buffer_t in_buf = { input_data, input_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Feed input (can be called multiple times)
gcomp_status_t s = gcomp_encoder_update(enc, &in_buf, &out_buf);
if (s != GCOMP_OK) { /* handle error */ }

// Finalize (writes end mark and trailer)
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
gcomp_options_set_bool(opts, "lz4.concat", 1);

gcomp_decoder_create(registry, "lz4", opts, &dec);

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
gcomp_encoder_create(registry, "lz4", opts, &enc);

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
gcomp_decoder_create(registry, "lz4", opts, &dec);

// First decompression
gcomp_buffer_t in1 = { compressed1, comp_len1, 0 };
gcomp_buffer_t out1 = { buf1, cap1, 0 };
gcomp_decoder_update(dec, &in1, &out1);
gcomp_decoder_finish(dec, &out1);

// Reset clears all state: checksums, counters, header info, error state
gcomp_decoder_reset(dec);

// Second decompression - completely independent
gcomp_buffer_t in2 = { compressed2, comp_len2, 0 };
gcomp_buffer_t out2 = { buf2, cap2, 0 };
gcomp_decoder_update(dec, &in2, &out2);
gcomp_decoder_finish(dec, &out2);

gcomp_decoder_destroy(dec);
```

### What reset clears

| State | Encoder | Decoder |
|-------|---------|---------|
| Content checksum | ✓ Reset to init | ✓ Reset to init |
| Stage | ✓ Back to HEADER | ✓ Back to HEADER |
| Header parser | ✓ Position reset | ✓ Cleared |
| Block buffer | ✓ Position reset | ✓ Position reset |
| Hash table | ✓ Cleared | - |
| History buffer | - | ✓ Cleared |
| Total bytes counters | ✓ Reset to 0 | ✓ Reset to 0 |
| Error state | ✓ Cleared | ✓ Cleared |
| Options/config | Retained | Retained |

## Buffer convenience helpers

For simple use cases where the entire input fits in memory, use the buffer convenience API:

```c
#include <ghoti.io/compress/buffer.h>

// Compress data in one call
uint8_t *compressed = NULL;
size_t compressed_len = 0;

gcomp_status_t s = gcomp_buffer_compress(
    registry, "lz4", opts,
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
    registry, "lz4", opts,
    compressed_data, compressed_len,
    &decompressed, &decompressed_len);

if (s == GCOMP_OK) {
    // Use decompressed data...
    free(decompressed);
}
```

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
FILE *output_file = fopen("output.lz4", "wb");

gcomp_encoder_cb_create(registry, "lz4", opts,
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

## Interoperability

The LZ4 Frame Format implementation is fully compatible with:
- Standard `lz4` / `unlz4` command-line tools
- Python's `lz4.frame` module
- liblz4 library
- Any LZ4 Frame Format-compliant implementation

Files created by this library can be decompressed by standard tools, and files created by standard tools can be decompressed by this library.

**Note:** This library implements the LZ4 **Frame Format**, not the raw LZ4 block format. Raw LZ4 blocks (without framing) are not directly supported.

## Comparison with other methods

| Feature | LZ4 | Gzip | Deflate |
|---------|-----|------|---------|
| Compression ratio | Lower | Higher | Higher |
| Compression speed | Fastest | Slower | Slower |
| Decompression speed | Fastest | Slower | Slower |
| Memory usage | Higher | Lower | Lower |
| Parallel-friendly | Yes (independent blocks) | No | No |
| Block checksums | Yes | No (whole-stream CRC) | No |
| Streaming | Yes | Yes | Yes |

**When to use LZ4:**
- Speed is more important than compression ratio
- Real-time compression/decompression is required
- Data will be processed in parallel
- Memory is not severely constrained

## See also

- [Streaming API](../api/streaming.md) - General streaming usage patterns
- [Limits](../api/limits.md) - Safety limit configuration
- [Threading](../threading.md) - Thread pool and parallel compression
- [Auto-Registration](../auto-registration.md) - How methods are registered
