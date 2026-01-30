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
| `limits.max_memory_bytes` | uint64 | 256 MiB | Maximum working memory for decoder |
| `limits.max_expansion_ratio` | uint64 | 1000 | Maximum output/input byte ratio |
| `limits.max_window_bytes` | uint64 | method-specific | Maximum LZ77 window size |

Set any limit to `0` for unlimited (not recommended for untrusted input).

## Expansion Ratio Protection

The `limits.max_expansion_ratio` option specifically targets **decompression bombs** (also known as "zip bombs") - maliciously crafted archives designed to expand to massive sizes from tiny inputs.

### How it works

The decoder tracks:
- `total_input_bytes`: Compressed bytes consumed
- `total_output_bytes`: Decompressed bytes produced

On each output operation, the decoder checks:

```
output_bytes <= ratio_limit × input_bytes
```

With the default limit of 1000:
- 1 KB input → max 1 MB output
- 1 MB input → max 1 GB output
- 10 MB input → max 10 GB output

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
#define GCOMP_DEFAULT_MAX_EXPANSION_RATIO  1000ULL                  // 1000x
```

## Security Recommendations

1. **Always set limits for untrusted input** - Even with defaults, consider tighter limits for web endpoints or user uploads.

2. **Use expansion ratio limits** - The default 1000x catches most decompression bombs while allowing legitimate high-compression data.

3. **Monitor for limit errors** - Log `GCOMP_ERR_LIMIT` errors to detect potential attacks.

4. **Test with malicious inputs** - Verify your limits work by testing with known decompression bombs.

5. **Consider per-request limits** - For multi-tenant systems, use separate options objects with per-user limits.
