/**
 * @file fuzz_zstd_encoder.c
 *
 * AFL++ fuzz harness for the Zstd encoder.
 *
 * This harness reads arbitrary bytes from stdin and compresses them using
 * the Zstd encoder. The goal is to find inputs that cause crashes, hangs,
 * or undefined behavior in the encoder.
 *
 * Key areas being tested:
 * - Match finding (hash chains, repeat offsets)
 * - Block selection (raw vs RLE vs compressed)
 * - Entropy encoding (FSE tables, sequence encoding)
 * - Literals encoding (raw literals)
 * - Frame header/trailer writing
 * - Content checksum computation
 * - Memory management under various input patterns
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_zstd_encoder fuzz/fuzz_zstd_encoder.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/zstd_encoder -o fuzz/findings/zstd_encoder \
 *       -- ./fuzz_zstd_encoder
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
#define MAX_INPUT_SIZE (1024 * 1024) // 1 MB

// Output buffer size - compressed output plus overhead
#define OUTPUT_BUFFER_SIZE (2 * 1024 * 1024) // 2 MB

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
      // Limit input size to prevent excessive memory/time usage
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
 * @brief Fuzz the Zstd encoder using the streaming API
 *
 * This exercises more code paths by processing input in chunks.
 */
static void fuzz_encoder_streaming(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_status_t status;

  // Create encoder with checksum to test xxHash computation
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_bool(opts, "zstd.checksum", 1);
    gcomp_options_set_int64(opts, "zstd.level", 3);
    // Use smaller window for faster fuzzing
    gcomp_options_set_uint64(opts, "zstd.window_log", 14);
    gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 16 * 1024 * 1024);
  }

  status = gcomp_encoder_create(NULL, "zstd", opts, &encoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  // Process input in variable-sized chunks
  size_t input_offset = 0;
  size_t output_offset = 0;
  size_t chunk_size = 1;

  while (input_offset < input_size) {
    // Vary chunk size to test different streaming patterns
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = input_size - input_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = (void *)(input + input_offset), .size = chunk_size, .used = 0};

    size_t out_remaining = OUTPUT_BUFFER_SIZE - output_offset;
    if (out_remaining == 0) {
      break;
    }

    gcomp_buffer_t out_buf = {
        .data = output + output_offset, .size = out_remaining, .used = 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);

    input_offset += in_buf.used;
    output_offset += out_buf.used;

    if (status != GCOMP_OK) {
      break;
    }
  }

  // Finish the stream
  gcomp_buffer_t out_buf = {.data = output + output_offset,
      .size = OUTPUT_BUFFER_SIZE - output_offset,
      .used = 0};
  gcomp_encoder_finish(encoder, &out_buf);

  gcomp_encoder_destroy(encoder);
}

/**
 * @brief Fuzz the Zstd encoder using the simple buffer API
 */
static void fuzz_encoder_buffer(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  size_t output_size = OUTPUT_BUFFER_SIZE;

  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_int64(opts, "zstd.level", 3);
    gcomp_options_set_uint64(opts, "zstd.window_log", 14);
  }

  gcomp_encode_buffer(
      NULL, "zstd", opts, input, input_size, output, output_size, &output_size);

  if (opts) {
    gcomp_options_destroy(opts);
  }
}

/**
 * @brief Fuzz encoder with different compression levels
 */
static void fuzz_encoder_levels(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  // Test different compression levels to exercise different code paths
  // Level affects match finder search depth and strategy selection
  static const int levels[] = {1, 3, 9};
  size_t num_levels = sizeof(levels) / sizeof(levels[0]);

  for (size_t i = 0; i < num_levels; i++) {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    if (opts) {
      gcomp_options_set_int64(opts, "zstd.level", levels[i]);
      gcomp_options_set_uint64(opts, "zstd.window_log", 14);
    }

    size_t output_size = OUTPUT_BUFFER_SIZE;
    gcomp_encode_buffer(NULL, "zstd", opts, input, input_size, output,
        output_size, &output_size);

    if (opts) {
      gcomp_options_destroy(opts);
    }
  }
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {

  // Read input from stdin
  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0;
  }

  // Allocate output buffer
  uint8_t * output = malloc(OUTPUT_BUFFER_SIZE);
  if (!output) {
    free(input);
    return 0;
  }

  // Test various encoder paths
  fuzz_encoder_streaming(input, input_size, output);
  fuzz_encoder_buffer(input, input_size, output);
  fuzz_encoder_levels(input, input_size, output);

  free(output);
  free(input);

  return 0;
}
