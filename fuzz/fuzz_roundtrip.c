/**
 * @file fuzz_roundtrip.c
 *
 * AFL++ fuzz harness for DEFLATE encode-then-decode roundtrip testing.
 *
 * This harness reads arbitrary bytes from stdin, compresses them, then
 * decompresses them, and verifies the output matches the input. This is
 * a powerful way to find bugs because any mismatch indicates a bug.
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_roundtrip fuzz/fuzz_roundtrip.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/roundtrip -o fuzz/findings/roundtrip --
 * ./fuzz_roundtrip
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/stream.h"

// Maximum input size - keep small for fast fuzzing
#define MAX_INPUT_SIZE (64 * 1024) // 64 KB

// Compressed output buffer (deflate overhead is small)
#define COMPRESSED_BUFFER_SIZE (MAX_INPUT_SIZE + 1024)

// Decompressed output buffer (should match input size)
#define DECOMPRESSED_BUFFER_SIZE (MAX_INPUT_SIZE + 1024)

/**
 * @brief Read all data from stdin into a buffer
 */
static uint8_t * read_stdin(size_t * size_out) {
  size_t capacity = 4096;
  size_t size = 0;
  uint8_t * buffer = malloc(capacity);
  if (!buffer) {
    return NULL;
  }

  int c;
  while ((c = getchar()) != EOF) {
    if (size >= MAX_INPUT_SIZE) {
      break;
    }
    if (size >= capacity) {
      capacity *= 2;
      if (capacity > MAX_INPUT_SIZE) {
        capacity = MAX_INPUT_SIZE;
      }
      uint8_t * new_buffer = realloc(buffer, capacity);
      if (!new_buffer) {
        free(buffer);
        return NULL;
      }
      buffer = new_buffer;
    }
    buffer[size++] = (uint8_t)c;
  }

  *size_out = size;
  return buffer;
}

/**
 * @brief Perform roundtrip test with buffer API
 *
 * Returns 0 if roundtrip succeeds and data matches, non-zero otherwise.
 * A non-zero return that's not from memory allocation indicates a bug.
 */
static int roundtrip_buffer(const uint8_t * input, size_t input_size,
    uint8_t * compressed, uint8_t * decompressed, int level) {
  gcomp_options_t * opts = NULL;
  gcomp_status_t status;

  // Create options
  status = gcomp_options_create(&opts);
  if (status != GCOMP_OK) {
    return 0; // Memory issue, not a library bug
  }
  gcomp_options_set_int64(opts, "deflate.level", level);

  // Compress
  size_t compressed_size = COMPRESSED_BUFFER_SIZE;
  status = gcomp_encode_buffer(NULL, "deflate", opts, input, input_size,
      compressed, compressed_size, &compressed_size);

  if (status != GCOMP_OK) {
    gcomp_options_destroy(opts);
    // Encoding failed - could be valid (e.g., buffer too small) or a bug
    // For fuzzing, we treat encoding failure as "not interesting"
    return 0;
  }

  // Decompress
  size_t decompressed_size = DECOMPRESSED_BUFFER_SIZE;
  status = gcomp_decode_buffer(NULL, "deflate", NULL, compressed,
      compressed_size, decompressed, decompressed_size, &decompressed_size);

  gcomp_options_destroy(opts);

  if (status != GCOMP_OK) {
    // Decoding our own encoded output failed - this is a BUG!
    // Abort to trigger AFL crash detection
    fprintf(stderr, "ROUNDTRIP BUG: decode failed after successful encode\n");
    abort();
  }

  // Verify sizes match
  if (decompressed_size != input_size) {
    fprintf(stderr, "ROUNDTRIP BUG: size mismatch (input=%zu, output=%zu)\n",
        input_size, decompressed_size);
    abort();
  }

  // Verify content matches
  if (memcmp(input, decompressed, input_size) != 0) {
    fprintf(stderr, "ROUNDTRIP BUG: content mismatch\n");
    // Find first differing byte for debugging
    for (size_t i = 0; i < input_size; i++) {
      if (input[i] != decompressed[i]) {
        fprintf(stderr,
            "  First diff at offset %zu: input=0x%02x, output=0x%02x\n", i,
            input[i], decompressed[i]);
        break;
      }
    }
    abort();
  }

  return 0;
}

/**
 * @brief Perform roundtrip test with streaming API
 */
static int roundtrip_streaming(const uint8_t * input, size_t input_size,
    uint8_t * compressed, uint8_t * decompressed, int level) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_decoder_t * decoder = NULL;
  gcomp_options_t * opts = NULL;
  gcomp_status_t status;

  // Create options
  status = gcomp_options_create(&opts);
  if (status != GCOMP_OK) {
    return 0;
  }
  gcomp_options_set_int64(opts, "deflate.level", level);

  // Create encoder
  status = gcomp_encoder_create(NULL, "deflate", opts, &encoder);
  gcomp_options_destroy(opts);
  if (status != GCOMP_OK) {
    return 0;
  }

  // Encode in chunks
  size_t input_offset = 0;
  size_t compressed_offset = 0;
  size_t chunk_size = 1;

  while (input_offset < input_size) {
    chunk_size = (chunk_size * 7 + 13) % 256 + 1;
    size_t remaining = input_size - input_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = input + input_offset, .size = chunk_size, .used = 0};
    gcomp_buffer_t out_buf = {.data = compressed + compressed_offset,
        .size = COMPRESSED_BUFFER_SIZE - compressed_offset,
        .used = 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    input_offset += in_buf.used;
    compressed_offset += out_buf.used;

    if (status != GCOMP_OK && status != GCOMP_ERR_LIMIT) {
      gcomp_encoder_destroy(encoder);
      return 0;
    }
  }

  // Finish encoding
  gcomp_buffer_t out_buf = {.data = compressed + compressed_offset,
      .size = COMPRESSED_BUFFER_SIZE - compressed_offset,
      .used = 0};
  status = gcomp_encoder_finish(encoder, &out_buf);
  compressed_offset += out_buf.used;
  gcomp_encoder_destroy(encoder);

  if (status != GCOMP_OK) {
    return 0;
  }

  // Create decoder
  status = gcomp_decoder_create(NULL, "deflate", NULL, &decoder);
  if (status != GCOMP_OK) {
    return 0;
  }

  // Decode in chunks
  size_t comp_offset = 0;
  size_t decomp_offset = 0;
  chunk_size = 1;

  while (comp_offset < compressed_offset) {
    chunk_size = (chunk_size * 11 + 7) % 256 + 1;
    size_t remaining = compressed_offset - comp_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = compressed + comp_offset, .size = chunk_size, .used = 0};
    gcomp_buffer_t dec_out = {.data = decompressed + decomp_offset,
        .size = DECOMPRESSED_BUFFER_SIZE - decomp_offset,
        .used = 0};

    status = gcomp_decoder_update(decoder, &in_buf, &dec_out);
    comp_offset += in_buf.used;
    decomp_offset += dec_out.used;

    if (status != GCOMP_OK && status != GCOMP_ERR_LIMIT) {
      fprintf(stderr, "ROUNDTRIP BUG: streaming decode failed\n");
      gcomp_decoder_destroy(decoder);
      abort();
    }
  }

  // Finish decoding
  gcomp_buffer_t dec_out = {.data = decompressed + decomp_offset,
      .size = DECOMPRESSED_BUFFER_SIZE - decomp_offset,
      .used = 0};
  status = gcomp_decoder_finish(decoder, &dec_out);
  decomp_offset += dec_out.used;
  gcomp_decoder_destroy(decoder);

  if (status != GCOMP_OK) {
    fprintf(stderr, "ROUNDTRIP BUG: streaming decode finish failed\n");
    abort();
  }

  // Verify
  if (decomp_offset != input_size) {
    fprintf(stderr, "ROUNDTRIP BUG (streaming): size mismatch\n");
    abort();
  }

  if (memcmp(input, decompressed, input_size) != 0) {
    fprintf(stderr, "ROUNDTRIP BUG (streaming): content mismatch\n");
    abort();
  }

  return 0;
}

int main(int argc, char ** argv) {
  (void)argc;
  (void)argv;

  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0;
  }

  uint8_t * compressed = malloc(COMPRESSED_BUFFER_SIZE);
  uint8_t * decompressed = malloc(DECOMPRESSED_BUFFER_SIZE);
  if (!compressed || !decompressed) {
    free(input);
    free(compressed);
    free(decompressed);
    return 0;
  }

  // Select compression level based on input (deterministic)
  int level = 6;
  if (input_size > 0) {
    level = input[0] % 10;
  }

  // Test both APIs
  roundtrip_buffer(input, input_size, compressed, decompressed, level);
  roundtrip_streaming(input, input_size, compressed, decompressed, level);

  free(decompressed);
  free(compressed);
  free(input);

  return 0;
}
