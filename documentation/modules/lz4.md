# LZ4

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

- **Independent blocks** (`lz4.independent_blocks=true`, default): Each block is compressed independently. Back-references cannot cross block boundaries. This is what makes [parallel encoding](#parallel-compression) possible, and it would permit parallel decoding and random access too, though this decoder does neither.

- **Dependent blocks** (`lz4.independent_blocks=false`): Blocks can reference data from previous blocks. Achieves better compression ratio but requires sequential processing.

The encoder keeps the last 64 KB of the frame in front of the block it is
filling, so matches reach back across the block boundary. The match offset
field is two bytes, which bounds the reach at 65535 bytes however long the
frame is — so the benefit falls away as the block size grows, because only the
first 64 KB of any block can reach back at all. Measured on 2 MB inputs,
dependent against independent:

| data | 64 KB blocks | 256 KB | 1 MB |
|---|---|---|---|
| mixed text and random bytes | +3.68% | +0.55% | +0.13% |
| all zeros | +0.54% | +0.08% | +0.01% |
| English-like prose | +0.08% | +0.02% | +0.00% |

Dependent blocks cost 64 KB of encoder memory and rule out the
[parallel encoder](#parallel-compression), which needs each block to stand
alone; `threads.count` is ignored in that mode.

## Options

### LZ4-specific options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `lz4.block_size` | uint64 | 4194304 | Block size in bytes. Must be one of: 65536 (64KB), 262144 (256KB), 1048576 (1MB), or 4194304 (4MB) |
| `lz4.block_checksum` | bool | false | Enable per-block xxHash32 checksum |
| `lz4.content_checksum` | bool | false | Enable content xxHash32 checksum in frame trailer |
| `lz4.independent_blocks` | bool | true | Use independent blocks (parallel-friendly) |
| `lz4.dictionary` | bytes | none | Dictionary content; only the last 64 KB is used |
| `lz4.content_size` | uint64 | (none) | Content size to write in header (encoder); validated on decode if present |
| `lz4.dictionary_id` | uint64 | (none) | Dictionary ID to write in header (parsing only, dictionaries not yet supported) |
| `lz4.concat` | bool | false | Decoder: support concatenated LZ4 frames |

### Threading options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `threads.count` | uint64 | 1 | Number of worker threads (0 or 1 = single-threaded). Applies to decoding as well as encoding: with `gcomp_decode_buffer()` and a frame whose blocks are independent, every block is a job. A frame with linked blocks, one block, or a dictionary is decoded single-threaded — see [Threading](../threading.md). |

See [Parallel compression](#parallel-compression) below.

### Core limit options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 512 MiB | Maximum decompressed output (decoder) |
| `limits.max_block_bytes` | uint64 | 4 MiB | Maximum block size during decode |
| `limits.max_memory_bytes` | uint64 | 256 MiB | Maximum working memory |
| `limits.max_expansion_ratio` | uint64 | 255 | Maximum output/input ratio (decoder). The default is the format's own ceiling: a sequence buys 255 bytes of match per extension byte. |

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

### Streaming and incremental output

Encoder output is **incremental** even at block boundaries. When a block is compressed and the caller’s output buffer is too small to hold the entire block, the encoder buffers the remainder and returns `GCOMP_OK`; the next `update()` (or `finish()`) call will emit the buffered bytes before consuming more input. Callers may use arbitrarily small output buffers (e.g. 1–8 bytes) and still stream-encode correctly by repeatedly calling `update()` until input is consumed, then `finish()` until completion.

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

## Inspecting a frame before decoding it

`gcomp_lz4_peek_frame_info()` reads a frame's descriptor without decoding
anything — how large the content will be, which checksums are present, and
above all **which dictionary the frame needs**, which is otherwise unknowable:
the decoder needs the dictionary to start, and the only place its identity
appears is the header.

```c
gcomp_lz4_frame_info_t info;
size_t needed = 0;
gcomp_status_t st = gcomp_lz4_peek_frame_info(buf, len, &info, &needed);
if (st == GCOMP_ERR_LIMIT) {
  // Read `needed` bytes in total and ask again; it takes a few rounds --
  // four bytes to recognise the magic, six to learn which optional fields
  // are present, then the whole header.
}
else if (st == GCOMP_OK && info.dict_id_present) {
  // Look up info.dict_id and pass that dictionary as lz4.dictionary.
}
```

Nothing is allocated. Skippable frames are reported rather than refused, with
`frame_size` set, so a reader can step over one and ask again — that is how you
walk a stream of mixed frames.

The validation is the decoder's own: `gcomp_lz4_peek_frame_info()` and the
decoder share a single parser, so a header one accepts the other accepts, and a
header one refuses the other refuses.

> **`lz4.content_size` carries the size, it is not a flag.** It is a `uint64`
> whose value is the uncompressed length; set it with
> `gcomp_options_set_uint64()`. Setting it with `gcomp_options_set_bool()`
> stores a differently-typed entry that the encoder declines, and the field
> simply does not appear in the frame — silently. `gcomp_lz4_peek_frame_info()`
> is the quickest way to confirm an option reached the header.

## Dictionaries

A dictionary is a block of bytes both sides know in advance. The encoder can
match against it as though it preceded the data, which is what makes small
payloads compress at all — a 500-byte record has almost nothing of its own to
match against, but plenty in common with the thousand records before it.

Pass the content through `lz4.dictionary` on the encoder, the decoder, or both:

```c
gcomp_options_set_bytes(opts, "lz4.dictionary", dict, dict_len);
```

Only the **last 64 KB** is used. The match offset is two bytes, so nothing
earlier is reachable, and a longer dictionary is truncated to its tail — pass
all of it or pass the tail, the result is identical.

**The block mode changes what a dictionary does**, which the specification's
wording does not settle and which was established by reading liblz4's own
output:

- **Dependent blocks**: the dictionary precedes the first block, and the
  frame's own output takes over from there.
- **Independent blocks**: *every* block starts from the dictionary again — a
  block may not reference the blocks before it, but it may reference the
  dictionary.

So with independent blocks the benefit persists across a large frame, while
with dependent blocks it fades as the frame's own history takes over. Measured
on text resembling the dictionary, against the same data compressed without
one:

| data | dependent | independent |
|---|---|---|
| 500 B | 77 → 41 (**46.8%** smaller) | 77 → 41 (**46.8%**) |
| 5 KB | 95 → 59 (37.9%) | 95 → 59 (37.9%) |
| 70 KB | 363 → 348 (4.1%) | 408 → 335 (17.9%) |
| 200 KB | 942 → 927 (1.6%) | 1032 → 878 (14.9%) |

A dictionary with nothing in common with the data costs nothing to speak of —
the encoder simply finds no matches in it — so the risk of a badly chosen one
is wasted memory, not a larger frame.

> **A wrong dictionary yields wrong bytes, silently.** Nothing in the block
> format can detect it: the matches resolve, they just resolve to the wrong
> content. This is how LZ4 works rather than a limitation here, and the
> frame's own defence is the content checksum, which does catch it. **Enable
> `lz4.content_checksum` on any frame meant to travel with a dictionary**, and
> consider `lz4.dictionary_id` so the reader can tell which one it needs.
> Decoding without the dictionary at all is safe: the offsets reach outside
> anything the decoder holds and the frame is refused.

With independent blocks the dictionary is re-indexed for each block, a scan of
up to 64 KB per block. If that shows up in a profile, dependent blocks avoid
it, as does a larger block size.

## Skippable frames

A skippable frame carries bytes of your choosing through an LZ4 stream. Every
conforming decoder steps over it, so it is where an application puts its own
metadata without disturbing the data. The frame is a magic number of
`0x184D2A50` through `0x184D2A5F`, a 4-byte little-endian size, and that many
bytes of payload — none of it compressed.

The low nibble of the magic number is yours to choose (the *magic variant*, 0
through 15), so an application can tell its own kinds of embedded data apart.
Decoders skip the frame whatever it says.

**Decoding.** Skippable frames are stepped over wherever they appear — before,
between or after data frames, or alone — and this is not gated on
`lz4.concat`. A skippable frame in front of the data is a preamble, not a
concatenation. `lz4.concat` keeps its own meaning: whether to carry on past a
finished *data* frame, so a skippable frame trailing one is governed by it like
any other trailing frame. A stream consisting only of skippable frames decodes
to nothing, and is not an error.

**Writing and reading.** Because the decoder discards skippable frames, as the
format requires, it can never hand the payload back. Use the parser to read
what you embedded:

```c
#include <ghoti.io/compress/lz4.h>

// Write metadata ahead of the compressed data.
const char *note = "recorded 2026-09-17";
size_t note_len = strlen(note);

uint8_t frame[GCOMP_LZ4_SKIPPABLE_OVERHEAD + 64];
size_t written = 0;
gcomp_lz4_write_skippable_frame(3, note, note_len, frame, sizeof(frame),
    &written);
// Prepend `written` bytes of `frame` to an ordinary lz4 stream.

// Later, reading it back from your own buffer:
unsigned variant = 0;
size_t offset = 0, payload_size = 0, frame_size = 0;
if (gcomp_lz4_read_skippable_frame(stream, stream_len, &variant, &offset,
        &payload_size, &frame_size) == GCOMP_OK) {
  // stream + offset holds payload_size bytes; nothing was copied.
  // stream + frame_size is whatever follows.
}
```

**Reading while decoding.** The parser needs a buffer to point at, which a
caller reading a pipe or a socket does not have. Register a callback on the
decoder instead and the payload is reported while it is still in hand:

```c
static gcomp_status_t on_skippable(void *ctx, unsigned variant,
    uint64_t payload_size, uint64_t payload_offset,
    const uint8_t *chunk, size_t chunk_size) {
  struct my_state *st = ctx;
  if (payload_offset == 0) {
    // First piece of a new frame: decide whether you want it at all.
    if (payload_size > MY_METADATA_CEILING) {
      return GCOMP_ERR_LIMIT;   // stops the decode with this status
    }
    st->buf = malloc((size_t)payload_size);
    st->len = 0;
  }
  memcpy(st->buf + st->len, chunk, chunk_size);
  st->len += chunk_size;
  return GCOMP_OK;
}

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "lz4", opts, &dec);
gcomp_lz4_decoder_on_skippable_frame(dec, on_skippable, &my_state);
```

The payload is **not** buffered by the library. A skippable frame may declare
up to 4 GB and the size comes from the stream, so holding one whole would let
the input choose an allocation. It arrives in pieces instead — in order,
covering `[0, payload_size)` exactly — each carrying the variant, the whole
size, and this piece's offset. That is why the example above checks
`payload_size` against its own ceiling on the first piece: **the decision about
how much memory to spend is yours, and it is the only place the stream cannot
make it for you.**

A frame with an empty payload is still reported, once, with `chunk_size` 0 —
its variant may be the whole message.

`chunk` points into the buffer you passed to `gcomp_decoder_update()` and is
valid only during the call. Whatever the callback returns is what
`gcomp_decoder_update()` returns, and the decoder stops there. The registration
survives `gcomp_decoder_reset()`; pass NULL to clear it. Skippable frames are
skipped either way, so the callback changes what you are told, never what the
stream decodes to.

The convenience wrappers (`gcomp_decode_buffer()`, `gcomp_decode_stream_cb()`)
create and destroy a decoder internally, so there is none to register on — use
`gcomp_decoder_create()` when you need this.

Neither the writer nor the parser allocates, and the parser reports the payload
as an offset into your buffer rather than copying it. A buffer of
`GCOMP_LZ4_SKIPPABLE_OVERHEAD + payload_size` always holds the frame; the
payload may be at most `GCOMP_LZ4_SKIPPABLE_MAX_PAYLOAD` bytes, the size field
being 32 bits.

The parser returns `GCOMP_ERR_CORRUPT` for a magic number outside the skippable
range and for a frame cut short of the size it declares, so it is safe to point
at untrusted bytes.

## Flushing

`gcomp_encoder_flush()` closes the block being filled without ending the frame.
See [Streaming API](../api/streaming.md#flushing) for the general contract.

A flushed block is an ordinary LZ4 block, so nothing about the frame changes
except where the block boundaries fall — and a flush costs only the short block
it forces. The frame header goes out first if it has not already.

With independent blocks (the default) a block already carries no history, so
`GCOMP_FLUSH_SYNC` and `GCOMP_FLUSH_FULL` do the same thing. With
`lz4.independent_blocks=false` a full flush additionally drops the 64 KB
window, so nothing after the flush can match into anything before it. It resets
to an empty window rather than back to the dictionary: with linked blocks the
window slides and the frame's own output displaces the dictionary as it goes,
so by then there is generally nothing of it left to restore.

In [parallel mode](#parallel-compression) a flush submits the block being
filled and then waits for every block still out with the workers — which is the
point: nothing the caller handed over is left in flight.

## Parallel compression

Set `threads.count` above 1 and the encoder compresses blocks on a pool of
worker threads instead of in the calling thread.

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_uint64(opts, "threads.count", 4);

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "lz4", opts, &enc);
```

### What comes out

**One ordinary LZ4 frame, byte-for-byte identical to what one thread would
have produced.** This is not an approximation: the same input, block size,
checksum settings and dictionary give the same output bytes at any thread
count. Nothing downstream can tell which encoder produced a frame, and
nothing needs to.

That is possible because the format already has the seam. With the Block
Independence flag set, each block is compressed knowing nothing about the
blocks before it, so a worker can be handed exactly the window the serial
encoder would have handed itself. (Zstd's parallel mode, by contrast,
produces *concatenated frames*, because a zstd frame cannot be split.)

### What divides the work

One job is one block, so **`lz4.block_size` is also the parallel
granularity**. There is no LZ4 equivalent of `zstd.job_size`; the frame
format already names the unit.

That has a practical consequence: an input smaller than a few blocks has
little to parallelise. 13 MB with the default 4 MB blocks is three blocks,
so no thread count beats about 3x however many cores are free. The same
input at 256 KB blocks is 53 blocks and scales further, at a small cost in
ratio.

Measured on a 12-core machine over 13.8 MB of mixed text (best of seven,
interleaved):

| Block size | 1 thread | 2 threads | 4 threads | 8 threads |
|------------|----------|-----------|-----------|-----------|
| 4 MB   | 547 MB/s | 803 MB/s (1.5x) | 1175 MB/s (2.2x) | 1241 MB/s (2.3x) |
| 1 MB   | 521 MB/s | 804 MB/s (1.5x) | 1222 MB/s (2.3x) | 1364 MB/s (2.6x) |
| 256 KB | 532 MB/s | 850 MB/s (1.6x) | 1472 MB/s (2.8x) | 1915 MB/s (3.6x) |

Output size was identical at every thread count in every row.

### When it does not apply

`threads.count > 1` is honoured only with independent blocks. Linked blocks
(`lz4.independent_blocks=false`) are the format saying block N may reference
block N-1, and that dependency is exactly what rules out compressing the two
at once. The combination is not an error and does not change the output; the
encoder simply compresses in the calling thread.

Because that is easy to arrange by accident — two options that each make
sense — the encoder reports what it actually did rather than leaving the
caller to assume:

```c
uint32_t workers = gcomp_lz4_encoder_worker_count(enc);  /* 1 when inline */
```

### Memory

Each in-flight block holds its own window, output buffer and match-finder
table:

```
  dictionary_size + block_size    (window)
+ block_size + 24                 (framed output)
+ 256 KB                          (hash table)
```

Up to `threads.count * 2` blocks are in flight at once, and that bound is
lowered to fit `limits.max_memory_bytes` — so raising the block size costs
memory rather than breaching the limit. At the default 4 MB block size,
four threads want roughly 66 MB of in-flight buffers; `limits.max_memory_bytes`
defaults to 256 MiB, and an encoder that cannot fit even one block in what
is left of the budget fails at creation with `GCOMP_ERR_MEMORY`.

### Decompression

Decoding is single-threaded regardless of `threads.count`. Independent
blocks would permit parallel decode, and the flag is preserved in the frame
so a future decoder could use it, but this one does not.


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
| SKIPPABLE_SIZE | Reading a skippable frame's 4-byte size |
| SKIPPABLE_DATA | Discarding a skippable frame's payload |
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

This is checked rather than asserted: the test suite binds to the installed
`liblz4` at runtime and puts our frames through its decoder — across every
frame option, all four block sizes, multi-block and dependent-block frames, and
skippable frames of every variant — as well as putting its frames through ours.

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
