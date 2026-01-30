/**
 * @file fuzz_deflate_encoder.c
 *
 * AFL++ fuzz harness for the DEFLATE encoder.
 *
 * This harness reads arbitrary bytes from stdin and compresses them using
 * the DEFLATE encoder at various compression levels. The goal is to find
 * inputs that cause crashes, hangs, or undefined behavior in the encoder.
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_encoder fuzz/fuzz_deflate_encoder.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/encoder -o fuzz/findings/encoder -- ./fuzz_encoder
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

// Maximum input size to prevent excessive memory usage
#define MAX_INPUT_SIZE                                                         \
  (256 * 1024) // 256 KB (smaller for encoder - it's slower)

// Output buffer size - deflate output is at most ~0.1% larger than input +
// overhead
#define OUTPUT_BUFFER_SIZE (MAX_INPUT_SIZE + 1024)

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
 * @brief Fuzz the encoder using the streaming API at a specific level
 */
static void fuzz_encoder_streaming(
    const uint8_t * input, size_t input_size, uint8_t * output, int level) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_options_t * opts = NULL;
  gcomp_status_t status;

  // Create options with compression level
  status = gcomp_options_create(&opts);
  if (status != GCOMP_OK) {
    return;
  }
  gcomp_options_set_int64(opts, "deflate.level", level);

  // Create encoder
  status = gcomp_encoder_create(NULL, "deflate", opts, &encoder);
  gcomp_options_destroy(opts);
  if (status != GCOMP_OK) {
    return;
  }

  // Process input in variable-sized chunks
  size_t input_offset = 0;
  size_t output_offset = 0;
  size_t chunk_size = 1;

  while (input_offset < input_size) {
    chunk_size = (chunk_size * 7 + 13) % 512 + 1;
    size_t remaining = input_size - input_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = input + input_offset, .size = chunk_size, .used = 0};

    size_t out_remaining = OUTPUT_BUFFER_SIZE - output_offset;
    if (out_remaining == 0) {
      break;
    }

    gcomp_buffer_t out_buf = {
        .data = output + output_offset, .size = out_remaining, .used = 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);

    input_offset += in_buf.used;
    output_offset += out_buf.used;

    if (status != GCOMP_OK && status != GCOMP_ERR_LIMIT) {
      break;
    }
  }

  // Finish encoding
  gcomp_buffer_t out_buf = {.data = output + output_offset,
      .size = OUTPUT_BUFFER_SIZE - output_offset,
      .used = 0};
  gcomp_encoder_finish(encoder, &out_buf);

  gcomp_encoder_destroy(encoder);
}

/**
 * @brief Fuzz the encoder using the simple buffer API
 */
static void fuzz_encoder_buffer(
    const uint8_t * input, size_t input_size, uint8_t * output, int level) {
  gcomp_options_t * opts = NULL;
  gcomp_status_t status;

  status = gcomp_options_create(&opts);
  if (status != GCOMP_OK) {
    return;
  }
  gcomp_options_set_int64(opts, "deflate.level", level);

  size_t output_size = OUTPUT_BUFFER_SIZE;
  gcomp_encode_buffer(NULL, "deflate", opts, input, input_size, output,
      output_size, &output_size);

  gcomp_options_destroy(opts);
}

/**
 * @brief Fuzz the encoder with different strategies
 */
static void fuzz_encoder_strategies(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  const char * strategies[] = {
      "default", "filtered", "huffman_only", "rle", "fixed"};
  const int num_strategies = sizeof(strategies) / sizeof(strategies[0]);

  // Use first byte of input to select strategy (deterministic based on input)
  int strategy_idx = 0;
  if (input_size > 0) {
    strategy_idx = input[0] % num_strategies;
  }

  gcomp_options_t * opts = NULL;
  gcomp_status_t status;

  status = gcomp_options_create(&opts);
  if (status != GCOMP_OK) {
    return;
  }

  gcomp_options_set_string(opts, "deflate.strategy", strategies[strategy_idx]);
  gcomp_options_set_int64(opts, "deflate.level", 6);

  size_t output_size = OUTPUT_BUFFER_SIZE;
  gcomp_encode_buffer(NULL, "deflate", opts, input, input_size, output,
      output_size, &output_size);

  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  (void)argc;
  (void)argv;

  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0;
  }

  uint8_t * output = malloc(OUTPUT_BUFFER_SIZE);
  if (!output) {
    free(input);
    return 0;
  }

  // Test multiple compression levels
  // Use input to deterministically select level to avoid slow test cases
  int level = 6; // Default
  if (input_size > 0) {
    level = input[0] % 10; // 0-9
  }

  fuzz_encoder_streaming(input, input_size, output, level);
  fuzz_encoder_buffer(input, input_size, output, level);
  fuzz_encoder_strategies(input, input_size, output);

  free(output);
  free(input);

  return 0;
}
