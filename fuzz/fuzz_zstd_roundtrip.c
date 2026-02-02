/**
 * @file fuzz_zstd_roundtrip.c
 *
 * AFL++ fuzz harness for Zstd encode/decode roundtrip testing.
 *
 * This harness reads arbitrary bytes from stdin, compresses them, then
 * decompresses and verifies the output matches the original input.
 * This is effective at finding encoder/decoder mismatches and data corruption.
 *
 * Key areas being tested:
 * - Encoder produces valid output that decoder can process
 * - Decoded output exactly matches original input
 * - Various input patterns (random, repetitive, structured)
 * - Block boundary handling
 * - Repeat offset handling
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_zstd_roundtrip fuzz/fuzz_zstd_roundtrip.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/zstd_roundtrip -o fuzz/findings/zstd_roundtrip \
 *       -- ./fuzz_zstd_roundtrip
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/macros.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/stream.h"
#include "ghoti.io/compress/zstd.h"

// Maximum input size to prevent excessive memory usage
#define MAX_INPUT_SIZE (256 * 1024) // 256 KB

// Buffer sizes
#define COMPRESSED_BUFFER_SIZE (512 * 1024)   // 512 KB
#define DECOMPRESSED_BUFFER_SIZE (512 * 1024) // 512 KB

/**
 * @brief Read all data from stdin into a buffer
 *
 * @param size_out Output parameter for the number of bytes read
 * @return Pointer to allocated buffer (caller must free), or NULL on error
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
 * @brief Compress data using Zstd
 *
 * @param input Input data to compress
 * @param input_size Size of input data
 * @param output Output buffer for compressed data
 * @param output_cap Capacity of output buffer
 * @param compressed_size_out Output: actual compressed size
 * @param opts Options (can be NULL)
 * @return true if compression succeeded, false otherwise
 */
static int compress_data(const uint8_t * input, size_t input_size,
    uint8_t * output, size_t output_cap, size_t * compressed_size_out,
    gcomp_options_t * opts) {
  size_t compressed_size = output_cap;
  gcomp_status_t status = gcomp_encode_buffer(NULL, "zstd", opts, input,
      input_size, output, output_cap, &compressed_size);
  if (status != GCOMP_OK) {
    return 0;
  }
  *compressed_size_out = compressed_size;
  return 1;
}

/**
 * @brief Decompress data using Zstd
 *
 * @param compressed Compressed data
 * @param compressed_size Size of compressed data
 * @param output Output buffer for decompressed data
 * @param output_cap Capacity of output buffer
 * @param decompressed_size_out Output: actual decompressed size
 * @param opts Options (can be NULL)
 * @return true if decompression succeeded, false otherwise
 */
static int decompress_data(const uint8_t * compressed, size_t compressed_size,
    uint8_t * output, size_t output_cap, size_t * decompressed_size_out,
    gcomp_options_t * opts) {
  size_t decompressed_size = output_cap;
  gcomp_status_t status = gcomp_decode_buffer(NULL, "zstd", opts, compressed,
      compressed_size, output, output_cap, &decompressed_size);
  if (status != GCOMP_OK) {
    return 0;
  }
  *decompressed_size_out = decompressed_size;
  return 1;
}

/**
 * @brief Test roundtrip with specific options
 */
static void test_roundtrip(const uint8_t * input, size_t input_size,
    uint8_t * compressed, uint8_t * decompressed, gcomp_options_t * enc_opts,
    gcomp_options_t * dec_opts) {

  size_t compressed_size = 0;
  if (!compress_data(input, input_size, compressed, COMPRESSED_BUFFER_SIZE,
          &compressed_size, enc_opts)) {
    // Compression failed - that's unexpected but not a crash
    return;
  }

  size_t decompressed_size = 0;
  if (!decompress_data(compressed, compressed_size, decompressed,
          DECOMPRESSED_BUFFER_SIZE, &decompressed_size, dec_opts)) {
    // Decoder failed on valid encoder output - this is a bug!
    // AFL will detect this via the non-zero exit code or crash
    fprintf(stderr, "ROUNDTRIP FAIL: decoder failed on encoder output\n");
    abort();
  }

  // Verify sizes match
  if (decompressed_size != input_size) {
    fprintf(stderr,
        "ROUNDTRIP FAIL: size mismatch (input=%zu, decompressed=%zu)\n",
        input_size, decompressed_size);
    abort();
  }

  // Verify content matches
  if (memcmp(input, decompressed, input_size) != 0) {
    fprintf(stderr, "ROUNDTRIP FAIL: content mismatch\n");
    // Find first differing byte for debugging
    for (size_t i = 0; i < input_size; i++) {
      if (input[i] != decompressed[i]) {
        fprintf(stderr, "  First diff at offset %zu: input=%02x, got=%02x\n", i,
            input[i], decompressed[i]);
        break;
      }
    }
    abort();
  }
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {

  // Read input from stdin
  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0;
  }

  // Allocate buffers
  uint8_t * compressed = malloc(COMPRESSED_BUFFER_SIZE);
  uint8_t * decompressed = malloc(DECOMPRESSED_BUFFER_SIZE);
  if (!compressed || !decompressed) {
    free(input);
    free(compressed);
    free(decompressed);
    return 0;
  }

  // Test basic roundtrip (level 3, no checksum)
  {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    if (opts) {
      gcomp_options_set_int64(opts, "zstd.level", 3);
      gcomp_options_set_uint64(opts, "zstd.window_log", 14);
    }
    test_roundtrip(input, input_size, compressed, decompressed, opts, NULL);
    if (opts)
      gcomp_options_destroy(opts);
  }

  // Test with checksum enabled
  {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    if (opts) {
      gcomp_options_set_bool(opts, "zstd.checksum", 1);
      gcomp_options_set_int64(opts, "zstd.level", 3);
      gcomp_options_set_uint64(opts, "zstd.window_log", 14);
    }
    test_roundtrip(input, input_size, compressed, decompressed, opts, NULL);
    if (opts)
      gcomp_options_destroy(opts);
  }

  // Test with higher compression level
  {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    if (opts) {
      gcomp_options_set_int64(opts, "zstd.level", 9);
      gcomp_options_set_uint64(opts, "zstd.window_log", 16);
    }
    test_roundtrip(input, input_size, compressed, decompressed, opts, NULL);
    if (opts)
      gcomp_options_destroy(opts);
  }

  // Test with content size in header
  {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    if (opts) {
      gcomp_options_set_int64(opts, "zstd.level", 3);
      gcomp_options_set_uint64(opts, "zstd.window_log", 14);
      gcomp_options_set_uint64(opts, "zstd.content_size", input_size);
    }
    test_roundtrip(input, input_size, compressed, decompressed, opts, NULL);
    if (opts)
      gcomp_options_destroy(opts);
  }

  free(decompressed);
  free(compressed);
  free(input);

  return 0;
}
