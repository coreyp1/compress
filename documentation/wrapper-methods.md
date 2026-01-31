# Wrapper Methods

This document describes the pattern for implementing wrapper compression methods that wrap an underlying compression algorithm and add format-specific headers, trailers, and checksums.

## Overview

Wrapper methods extend a base compression method (like deflate) with additional format features:

| Wrapper | Wraps | Adds |
|---------|-------|------|
| gzip | deflate | 10-byte header, CRC32, file size |
| zlib | deflate | 2-byte header, Adler-32 checksum |
| zip | deflate | File metadata, directory structure |

The wrapper method pattern allows these formats to:
- Reuse the proven deflate encoder/decoder
- Add format-specific header/trailer handling
- Integrate checksums transparently
- Pass through compression options

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        gzip_encoder_t                                   │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    Wrapper State                                 │   │
│  │  • stage (HEADER, DATA, TRAILER)                                │   │
│  │  • crc32 accumulator                                            │   │
│  │  • original_size counter                                        │   │
│  │  • header buffer / header_pos                                   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                               │                                         │
│                               ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                 Inner deflate_encoder_t                         │   │
│  │  • LZ77 state                                                   │   │
│  │  • Huffman tables                                               │   │
│  │  • Bit buffer                                                   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

## Implementation Pattern

### 1. Wrapper State Structure

```c
typedef enum {
    GZIP_STAGE_HEADER,     // Writing gzip header
    GZIP_STAGE_DATA,       // Compressing data via inner encoder
    GZIP_STAGE_TRAILER,    // Writing CRC32 and size
    GZIP_STAGE_DONE        // Complete
} gzip_encode_stage_t;

typedef struct {
    gzip_encode_stage_t stage;
    
    // Inner encoder (wrapped method)
    gcomp_encoder_t *inner_encoder;
    
    // Checksum state
    uint32_t crc32;
    uint32_t original_size;  // Mod 2^32 per gzip spec
    
    // Header buffer (for partial writes)
    uint8_t header[10];
    size_t header_written;
    
    // Trailer buffer (for partial writes)
    uint8_t trailer[8];
    size_t trailer_written;
    
    // Allocator for cleanup
    const gcomp_allocator_t *allocator;
} gzip_encode_state_t;
```

### 2. Creating the Wrapper Encoder

```c
gcomp_status_t gzip_create_encoder(gcomp_registry_t *registry,
    gcomp_options_t *options, gcomp_encoder_t **encoder_out) {
    
    gcomp_encoder_t *enc = *encoder_out;
    const gcomp_allocator_t *alloc = gcomp_registry_get_allocator(registry);
    
    // 1. Look up the wrapped method
    const gcomp_method_t *deflate = gcomp_registry_find(registry, "deflate");
    if (!deflate) {
        return GCOMP_ERR_UNSUPPORTED;
    }
    
    // 2. Allocate wrapper state
    gzip_encode_state_t *state = gcomp_calloc(alloc, 1, sizeof(*state));
    if (!state) {
        return GCOMP_ERR_MEMORY;
    }
    state->allocator = alloc;
    state->stage = GZIP_STAGE_HEADER;
    state->crc32 = 0;
    state->original_size = 0;
    
    // 3. Create inner encoder with same options
    gcomp_encoder_t *inner = NULL;
    gcomp_status_t status = gcomp_encoder_create(registry, "deflate", 
                                                  options, &inner);
    if (status != GCOMP_OK) {
        gcomp_free(alloc, state);
        return status;
    }
    state->inner_encoder = inner;
    
    // 4. Build gzip header
    gzip_build_header(state->header, options);
    state->header_written = 0;
    
    // 5. Set up encoder
    enc->method_state = state;
    enc->update_fn = gzip_encode_update;
    enc->finish_fn = gzip_encode_finish;
    enc->reset_fn = gzip_encode_reset;
    
    return GCOMP_OK;
}
```

### 3. Header/Trailer Handling Pattern

The wrapper must handle partial writes for headers and trailers:

```c
// Helper: write buffered data with partial write support
static size_t write_buffer(const uint8_t *src, size_t src_size,
                           size_t *src_pos, gcomp_buffer_t *output) {
    size_t remaining = src_size - *src_pos;
    size_t available = output->size - output->used;
    size_t to_write = (remaining < available) ? remaining : available;
    
    if (to_write > 0) {
        memcpy((uint8_t *)output->data + output->used, src + *src_pos, to_write);
        *src_pos += to_write;
        output->used += to_write;
    }
    
    return to_write;
}

static gcomp_status_t gzip_encode_update(gcomp_encoder_t *encoder,
    gcomp_buffer_t *input, gcomp_buffer_t *output) {
    
    gzip_encode_state_t *state = encoder->method_state;
    
    // Stage 1: Write header
    if (state->stage == GZIP_STAGE_HEADER) {
        write_buffer(state->header, sizeof(state->header),
                     &state->header_written, output);
        
        if (state->header_written < sizeof(state->header)) {
            return GCOMP_OK;  // Header incomplete, need more output space
        }
        state->stage = GZIP_STAGE_DATA;
    }
    
    // Stage 2: Compress data
    if (state->stage == GZIP_STAGE_DATA) {
        // Update CRC32 before compression
        if (input->size > 0) {
            const uint8_t *data = (const uint8_t *)input->data;
            state->crc32 = gcomp_crc32_update(state->crc32, data, input->size);
            state->original_size += (uint32_t)input->size;  // Mod 2^32
        }
        
        // Pass to inner encoder
        return gcomp_encoder_update(state->inner_encoder, input, output);
    }
    
    return GCOMP_OK;
}

static gcomp_status_t gzip_encode_finish(gcomp_encoder_t *encoder,
    gcomp_buffer_t *output) {
    
    gzip_encode_state_t *state = encoder->method_state;
    gcomp_status_t status;
    
    // Finish inner encoder first (if not already done)
    if (state->stage == GZIP_STAGE_DATA) {
        status = gcomp_encoder_finish(state->inner_encoder, output);
        if (status != GCOMP_OK) {
            return status;
        }
        
        // Build trailer: CRC32 (4 bytes) + ISIZE (4 bytes), little-endian
        state->trailer[0] = (state->crc32 >> 0) & 0xFF;
        state->trailer[1] = (state->crc32 >> 8) & 0xFF;
        state->trailer[2] = (state->crc32 >> 16) & 0xFF;
        state->trailer[3] = (state->crc32 >> 24) & 0xFF;
        state->trailer[4] = (state->original_size >> 0) & 0xFF;
        state->trailer[5] = (state->original_size >> 8) & 0xFF;
        state->trailer[6] = (state->original_size >> 16) & 0xFF;
        state->trailer[7] = (state->original_size >> 24) & 0xFF;
        state->trailer_written = 0;
        state->stage = GZIP_STAGE_TRAILER;
    }
    
    // Write trailer
    if (state->stage == GZIP_STAGE_TRAILER) {
        write_buffer(state->trailer, sizeof(state->trailer),
                     &state->trailer_written, output);
        
        if (state->trailer_written < sizeof(state->trailer)) {
            return GCOMP_OK;  // Need more output space
        }
        state->stage = GZIP_STAGE_DONE;
    }
    
    return GCOMP_OK;
}
```

### 4. Decoder Pattern

Decoding follows the inverse pattern:

```c
typedef enum {
    GZIP_DEC_HEADER,       // Reading gzip header
    GZIP_DEC_HEADER_EXTRA, // Optional extra field
    GZIP_DEC_HEADER_NAME,  // Optional filename
    GZIP_DEC_HEADER_COMMENT,// Optional comment
    GZIP_DEC_HEADER_CRC,   // Optional header CRC
    GZIP_DEC_DATA,         // Decompressing via inner decoder
    GZIP_DEC_TRAILER,      // Reading CRC32 and size
    GZIP_DEC_DONE
} gzip_decode_stage_t;

typedef struct {
    gzip_decode_stage_t stage;
    gcomp_decoder_t *inner_decoder;
    
    // Header parsing state
    uint8_t header[10];
    size_t header_read;
    uint8_t flags;
    
    // Checksum verification
    uint32_t expected_crc32;
    uint32_t computed_crc32;
    uint32_t expected_size;
    uint32_t actual_size;
    
    // Trailer buffer
    uint8_t trailer[8];
    size_t trailer_read;
    
    const gcomp_allocator_t *allocator;
} gzip_decode_state_t;

static gcomp_status_t gzip_decode_update(gcomp_decoder_t *decoder,
    gcomp_buffer_t *input, gcomp_buffer_t *output) {
    
    gzip_decode_state_t *state = decoder->method_state;
    gcomp_status_t status;
    
    // Stage 1: Read and validate header
    if (state->stage == GZIP_DEC_HEADER) {
        // Read header bytes
        while (state->header_read < 10 && input->used < input->size) {
            state->header[state->header_read++] = 
                ((const uint8_t *)input->data)[input->used++];
        }
        
        if (state->header_read < 10) {
            return GCOMP_OK;  // Need more input
        }
        
        // Validate magic number
        if (state->header[0] != 0x1F || state->header[1] != 0x8B) {
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                "invalid gzip magic number: %02X %02X",
                state->header[0], state->header[1]);
        }
        
        // Check compression method (must be deflate = 8)
        if (state->header[2] != 8) {
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_UNSUPPORTED,
                "unsupported compression method: %d", state->header[2]);
        }
        
        state->flags = state->header[3];
        state->stage = GZIP_DEC_DATA;  // Skip optional fields for simplicity
        
        // Handle optional header fields based on flags...
        // FEXTRA (bit 2), FNAME (bit 3), FCOMMENT (bit 4), FHCRC (bit 1)
    }
    
    // Stage 2: Decompress data
    if (state->stage == GZIP_DEC_DATA) {
        size_t output_before = output->used;
        
        status = gcomp_decoder_update(state->inner_decoder, input, output);
        if (status != GCOMP_OK) {
            return status;
        }
        
        // Update CRC32 and size with decompressed output
        size_t produced = output->used - output_before;
        if (produced > 0) {
            const uint8_t *out_data = (const uint8_t *)output->data + output_before;
            state->computed_crc32 = gcomp_crc32_update(state->computed_crc32,
                                                        out_data, produced);
            state->actual_size += (uint32_t)produced;
        }
        
        // Check if inner decoder is done (signals end of deflate stream)
        // This would be indicated by the decoder reaching its DONE state
    }
    
    return GCOMP_OK;
}

static gcomp_status_t gzip_decode_finish(gcomp_decoder_t *decoder,
    gcomp_buffer_t *output) {
    
    gzip_decode_state_t *state = decoder->method_state;
    
    // Finish inner decoder
    gcomp_status_t status = gcomp_decoder_finish(state->inner_decoder, output);
    if (status != GCOMP_OK) {
        return status;
    }
    
    // Verify CRC32
    if (state->computed_crc32 != state->expected_crc32) {
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "CRC32 mismatch: expected %08X, got %08X",
            state->expected_crc32, state->computed_crc32);
    }
    
    // Verify size (mod 2^32)
    if (state->actual_size != state->expected_size) {
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "size mismatch: expected %u, got %u",
            state->expected_size, state->actual_size);
    }
    
    return GCOMP_OK;
}
```

### 5. CRC/Checksum Integration

The library provides a CRC32 implementation that wrapper methods should use:

```c
#include <ghoti.io/compress/crc32.h>

// Initialize CRC (or use 0 for default start value)
uint32_t crc = gcomp_crc32_init();

// Update CRC with data chunks
crc = gcomp_crc32_update(crc, data1, len1);
crc = gcomp_crc32_update(crc, data2, len2);

// Finalize CRC (applies final XOR)
crc = gcomp_crc32_finalize(crc);
```

For gzip specifically:
- CRC32 is computed over the **uncompressed** data
- CRC32 is stored in the trailer (little-endian)
- Original size is stored mod 2^32 (little-endian)

For zlib:
- Adler-32 checksum over the **uncompressed** data
- Stored in trailer (big-endian)

### 6. Options Passthrough

Wrapper methods should pass compression options through to the inner method:

```c
// Options like "deflate.level" pass through automatically
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_int64(opts, "deflate.level", 9);  // Max compression

// Creating gzip encoder uses these deflate options internally
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "gzip", opts, &enc);
```

Wrapper methods can also define their own options:

```c
static const gcomp_option_schema_t g_gzip_options[] = {
    // Passthrough deflate options
    {
        .key = "deflate.level",
        .type = GCOMP_OPT_INT64,
        .has_default = 1,
        .default_value.i64 = 6,
        .has_min = 1, .min_int = 0,
        .has_max = 1, .max_int = 9,
        .help = "Compression level (0-9)"
    },
    // Gzip-specific options
    {
        .key = "gzip.filename",
        .type = GCOMP_OPT_STRING,
        .has_default = 0,
        .help = "Original filename to store in header"
    },
    {
        .key = "gzip.mtime",
        .type = GCOMP_OPT_UINT64,
        .has_default = 1,
        .default_value.ui64 = 0,
        .help = "Modification time (Unix timestamp)"
    },
};
```

### 7. Reset Support

Wrapper reset must reset both wrapper state and inner encoder/decoder:

```c
static gcomp_status_t gzip_encode_reset(gcomp_encoder_t *encoder) {
    gzip_encode_state_t *state = encoder->method_state;
    
    // Reset wrapper state
    state->stage = GZIP_STAGE_HEADER;
    state->crc32 = 0;
    state->original_size = 0;
    state->header_written = 0;
    state->trailer_written = 0;
    
    // Reset inner encoder
    return gcomp_encoder_reset(state->inner_encoder);
}
```

### 8. Cleanup

```c
static void gzip_destroy_encoder(gcomp_encoder_t *encoder) {
    if (!encoder || !encoder->method_state) return;
    
    gzip_encode_state_t *state = encoder->method_state;
    
    // Destroy inner encoder first
    if (state->inner_encoder) {
        gcomp_encoder_destroy(state->inner_encoder);
    }
    
    // Free wrapper state (encoder struct is freed by core)
    gcomp_free(state->allocator, state);
    encoder->method_state = NULL;
}
```

## Registration

Register the wrapper method like any other method:

```c
static const gcomp_method_t g_gzip_method = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "gzip",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = gzip_create_encoder,
    .create_decoder = gzip_create_decoder,
    .destroy_encoder = gzip_destroy_encoder,
    .destroy_decoder = gzip_destroy_decoder,
    .get_schema = gzip_get_schema,
};

gcomp_status_t gcomp_method_gzip_register(gcomp_registry_t *registry) {
    return gcomp_registry_register(registry, &g_gzip_method);
}

// Auto-registration (optional)
GCOMP_AUTOREG_METHOD(gzip, gcomp_method_gzip_register)
```

## Testing Wrapper Methods

### Interoperability Tests

Wrapper methods should be tested against standard tools:

```c
// Test that our gzip output can be decompressed by system gzip
TEST(GzipInterop, DecompressWithSystemGzip) {
    // Compress with our library
    uint8_t compressed[4096];
    size_t compressed_size;
    gcomp_encode_buffer(NULL, "gzip", NULL, input, input_size,
                        compressed, sizeof(compressed), &compressed_size);
    
    // Write to temp file and decompress with system gzip
    write_file("/tmp/test.gz", compressed, compressed_size);
    system("gzip -d -c /tmp/test.gz > /tmp/test.out");
    
    // Verify output matches original
    uint8_t *output = read_file("/tmp/test.out", &output_size);
    ASSERT_EQ(output_size, input_size);
    ASSERT_EQ(memcmp(output, input, input_size), 0);
}

// Test that we can decompress system gzip output
TEST(GzipInterop, DecompressSystemGzip) {
    // Compress with system gzip
    write_file("/tmp/test.txt", input, input_size);
    system("gzip -c /tmp/test.txt > /tmp/test.gz");
    
    // Decompress with our library
    uint8_t *compressed = read_file("/tmp/test.gz", &compressed_size);
    uint8_t output[4096];
    size_t output_size;
    
    gcomp_status_t status = gcomp_decode_buffer(NULL, "gzip", NULL,
        compressed, compressed_size, output, sizeof(output), &output_size);
    
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_EQ(output_size, input_size);
    ASSERT_EQ(memcmp(output, input, input_size), 0);
}
```

### Checksum Verification Tests

```c
TEST(GzipChecksum, DetectsCorruption) {
    // Compress valid data
    uint8_t compressed[4096];
    size_t compressed_size;
    gcomp_encode_buffer(NULL, "gzip", NULL, input, input_size,
                        compressed, sizeof(compressed), &compressed_size);
    
    // Corrupt a byte in the compressed data
    compressed[compressed_size / 2] ^= 0x01;
    
    // Decompression should fail with CORRUPT error
    uint8_t output[4096];
    size_t output_size;
    gcomp_status_t status = gcomp_decode_buffer(NULL, "gzip", NULL,
        compressed, compressed_size, output, sizeof(output), &output_size);
    
    ASSERT_EQ(status, GCOMP_ERR_CORRUPT);
}
```

## Related Documentation

- [Architecture Overview](architecture.md) - Overall library architecture
- [Streaming API](api/streaming.md) - Streaming patterns for wrapper methods
- [Deflate Module](modules/deflate.md) - The wrapped compression method
