# Safety Limits API

The Ghoti.io Compress library provides configurable safety limits to protect against resource exhaustion from malicious or malformed compressed data.

## Overview

When processing untrusted input, it's critical to limit:

1. **Output size** - Prevent unbounded memory allocation
2. **Memory usage** - Prevent working memory exhaustion
3. **Expansion ratio** - Prevent decompression bombs

All limits default to sensible values and can be customized via options.

## Limit Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 512 MiB | Maximum decompressed output size |
| `limits.max_memory_bytes` | uint64 | 256 MiB | Maximum working memory (encoder and decoder where the method tracks allocations) |
| `limits.max_expansion_ratio` | uint64 | the format's own ceiling (see below) | Maximum output/input byte ratio |
| `limits.max_window_bytes` | uint64 | method-specific | Maximum LZ77 window size |

Set any limit to `0` for unlimited (not recommended for untrusted input).

## Expansion Ratio Protection

The `limits.max_expansion_ratio` option bounds how much output a decoder will
produce per byte of input.

### How it works

The decoder tracks:
- `total_input_bytes`: Compressed bytes consumed **so far**
- `total_output_bytes`: Decompressed bytes produced **so far**

On each output operation, the decoder checks:

```
output_bytes <= ratio_limit × input_bytes
```

Both figures are running totals, not whole-stream totals - a streaming decoder
cannot know the whole stream. That matters: a file whose front compresses
harder than its body reaches a far higher ratio part-way through than it ends
at, so a limit chosen from whole-stream figures stops legitimate streams in the
middle.

### The default is the format's own ceiling

Every format has a maximum expansion it can possibly encode, and they differ by
a factor of five hundred:

| Method | Default | Where the number comes from |
|--------|---------|-----------------------------|
| `rle` | 64 : 1 | TIFF 6.0 §9: a two-byte run token carries at most 128 bytes |
| `lz4` | 255 : 1 | one extension byte per 255 further bytes of match |
| `deflate`, `zlib`, `gzip` | 1032 : 1 | RFC 1951 §3.2.5: a 258-byte match in as little as two bits |
| `lzw` | 2560 : 1 | TIFF 6.0 §13: a 3839-byte string in a twelve-bit code |
| `zstd` | 32768 : 1 | RFC 8878 §3.1.1.2.2: a four-byte RLE\_Block carries 128 KB |

Each is exposed as a macro - `GCOMP_DEFLATE_MAX_EXPANSION_RATIO` and so on - in
that method's header.

A limit set **below** a format's ceiling refuses streams that format may
legitimately produce. Set **at or above** it, the check never fires. So at the
default the check is not a policy guess but an impossibility test: it fires
only on output the format could not have produced, which means corruption or a
decoder bug.

This replaced a single 1000:1 default shared by every method, which was below
the ceiling for two formats and above it for two others. At that default this
library refused its own output: 32 MiB of zeros, compressed by these encoders
and handed back to these decoders, failed for five of the seven methods -
including `deflate` at a whole-stream ratio of 993:1, *below* the 1000 limit it
tripped, because the uniform prefix ran ahead of the average.

### What actually bounds a decompression bomb

`limits.max_output_bytes`. It is exact, it is format-independent, and it is the
limit to set when the question is "how much output am I willing to hold". The
ratio is the cheaper, earlier signal that a stream is heading somewhere
impossible; it is not the bound.

A caller who does want a ratio as policy - "nothing I accept should expand more
than 20x" - sets one explicitly, and should expect to refuse some legitimate
files.

### Example: Strict limits for untrusted input

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>

gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Strict limits for web uploads, API endpoints, etc.
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);  // 10 MB max
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);            // 100x max
gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1024 * 1024);       // 1 MB working memory

gcomp_decoder_t *dec = NULL;
gcomp_status_t status = gcomp_decoder_create(registry, "deflate", opts, &dec);
if (status != GCOMP_OK) {
    // Handle error (e.g., memory limit too low for decoder initialization)
}

// ... decode data ...

gcomp_decoder_destroy(dec);
gcomp_options_destroy(opts);
```

### Example: Disable limits for trusted data

```c
// For known-good data (e.g., test data, locally generated archives)
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0);  // 0 = unlimited
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 0);     // 0 = unlimited
```

## Error Handling

When a limit is exceeded, the decoder returns `GCOMP_ERR_LIMIT`. Use the error detail API to get diagnostic information:

```c
gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
if (status == GCOMP_ERR_LIMIT) {
    printf("Limit exceeded: %s\n", gcomp_decoder_get_error_detail(decoder));
    // Example output:
    // "limit exceeded at stage 'huffman_data' (output: 10485760/10485760 bytes)"
}
```

## API Reference

### Reading Limits

```c
// Read limit from options (or return default if not set)
uint64_t gcomp_limits_read_output_max(const gcomp_options_t *opts, uint64_t default_val);
uint64_t gcomp_limits_read_memory_max(const gcomp_options_t *opts, uint64_t default_val);
uint64_t gcomp_limits_read_window_max(const gcomp_options_t *opts, uint64_t default_val);
uint64_t gcomp_limits_read_expansion_ratio_max(const gcomp_options_t *opts, uint64_t default_val);
```

### Checking Limits

```c
// Returns GCOMP_OK if within limit, GCOMP_ERR_LIMIT if exceeded
gcomp_status_t gcomp_limits_check_output(size_t current, uint64_t limit);
gcomp_status_t gcomp_limits_check_memory(size_t current, uint64_t limit);
gcomp_status_t gcomp_limits_check_expansion_ratio(
    uint64_t input_bytes, uint64_t output_bytes, uint64_t ratio_limit);
```

### Memory Tracking

For methods that want to track memory usage:

```c
typedef struct {
    size_t current_bytes;
} gcomp_memory_tracker_t;

void gcomp_memory_track_alloc(gcomp_memory_tracker_t *tracker, size_t size);
void gcomp_memory_track_free(gcomp_memory_tracker_t *tracker, size_t size);
gcomp_status_t gcomp_memory_check_limit(const gcomp_memory_tracker_t *tracker, uint64_t limit);
```

## Default Values

Default values are defined in `<ghoti.io/compress/limits.h>`:

```c
#define GCOMP_DEFAULT_MAX_OUTPUT_BYTES     (512ULL * 1024 * 1024)  // 512 MiB
#define GCOMP_DEFAULT_MAX_MEMORY_BYTES     (256ULL * 1024 * 1024)  // 256 MiB
#define GCOMP_DEFAULT_MAX_EXPANSION_RATIO  1000ULL   // fallback only; no
                                                     // built-in method uses it
```

## Security Recommendations

1. **Always set limits for untrusted input** - Even with defaults, consider tighter limits for web endpoints or user uploads.

2. **Set `limits.max_output_bytes` deliberately** - it is the real bound. The expansion ratio defaults to each format's own ceiling, so it will not refuse anything by itself; lower it only if a ratio cap is the policy you want, and expect false positives when you do.

3. **Monitor for limit errors** - Log `GCOMP_ERR_LIMIT` errors to detect potential attacks.

4. **Test with malicious inputs** - Verify your limits work by testing with known decompression bombs.

5. **Consider per-request limits** - For multi-tenant systems, use separate options objects with per-user limits.
