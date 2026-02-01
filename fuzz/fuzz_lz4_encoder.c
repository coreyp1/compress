/**
 * @file fuzz_lz4_encoder.c
 *
 * AFL++ fuzz harness for the LZ4 encoder.
 *
 * This harness reads arbitrary bytes from stdin and compresses them using
 * the LZ4 frame format encoder. The goal is to find inputs that cause crashes,
 * hangs, or undefined behavior in the encoder.
 *
 * Key areas being tested:
 * - Input buffering and block splitting
 * - Match finding (hash table operations)
 * - Block compression (literal/match encoding)
 * - Header generation
 * - Checksum computation (xxHash32)
 * - Streaming with various chunk sizes
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_lz4_encoder fuzz/fuzz_lz4_encoder.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/lz4_encoder -o fuzz/findings/lz4_encoder \
 *       -- ./fuzz_lz4_encoder
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/lz4.h"
#include "ghoti.io/compress/macros.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/stream.h"

// Maximum input size to prevent excessive memory usage
#define MAX_INPUT_SIZE (256 * 1024) // 256 KB

// Output buffer size - LZ4 worst case is input + overhead
#define OUTPUT_BUFFER_SIZE                                                     \
  (MAX_INPUT_SIZE + 1024 * 1024) // Input + 1MB overhead

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
 * @brief Fuzz the LZ4 encoder with various option combinations
 */
static void fuzz_encoder_with_options(const uint8_t * input, size_t input_size,
    uint8_t * output, int content_checksum, int block_checksum,
    uint64_t block_size) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_status_t status;

  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_bool(opts, "lz4.content_checksum", content_checksum);
    gcomp_options_set_bool(opts, "lz4.block_checksum", block_checksum);
    gcomp_options_set_uint64(opts, "lz4.block_size", block_size);
    gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
  }

  status = gcomp_encoder_create(NULL, "lz4", opts, &encoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  // Process input in variable-sized chunks to stress test streaming
  size_t input_offset = 0;
  size_t output_offset = 0;
  size_t chunk_size = 1;

  while (input_offset < input_size) {
    // Vary chunk size
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
      gcomp_encoder_destroy(encoder);
      return;
    }
  }

  // Finish compression
  gcomp_buffer_t out_buf = {.data = output + output_offset,
      .size = OUTPUT_BUFFER_SIZE - output_offset,
      .used = 0};
  gcomp_encoder_finish(encoder, &out_buf);

  gcomp_encoder_destroy(encoder);
}

/**
 * @brief Fuzz the LZ4 encoder using the streaming API
 */
static void fuzz_encoder_streaming(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  // Test with default options
  fuzz_encoder_with_options(
      input, input_size, output, 0, 0, 65536); // 64KB blocks

  // Test with content checksum
  fuzz_encoder_with_options(
      input, input_size, output, 1, 0, 262144); // 256KB blocks

  // Test with block checksum
  fuzz_encoder_with_options(
      input, input_size, output, 0, 1, 1048576); // 1MB blocks

  // Test with both checksums
  fuzz_encoder_with_options(
      input, input_size, output, 1, 1, 4194304); // 4MB blocks
}

/**
 * @brief Fuzz the LZ4 encoder using the simple buffer API
 */
static void fuzz_encoder_buffer(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  size_t output_size = OUTPUT_BUFFER_SIZE;

  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
    gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
    gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  }

  gcomp_encode_buffer(
      NULL, "lz4", opts, input, input_size, output, output_size, &output_size);

  if (opts) {
    gcomp_options_destroy(opts);
  }
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {

  // Read input from stdin
  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0; // Memory allocation failure - not a bug in the library
  }

  // Allocate output buffer
  uint8_t * output = malloc(OUTPUT_BUFFER_SIZE);
  if (!output) {
    free(input);
    return 0;
  }

  // Test both APIs with various options
  fuzz_encoder_streaming(input, input_size, output);
  fuzz_encoder_buffer(input, input_size, output);

  free(output);
  free(input);

  return 0;
}
