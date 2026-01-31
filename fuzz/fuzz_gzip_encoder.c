/**
 * @file fuzz_gzip_encoder.c
 *
 * AFL++ fuzz harness for the gzip encoder.
 *
 * This harness reads arbitrary bytes from stdin and compresses them
 * as a gzip stream (RFC 1952). The goal is to find inputs that cause crashes,
 * hangs, or undefined behavior in the encoder.
 *
 * Key areas being tested:
 * - Header generation with various options
 * - DEFLATE compression via inner encoder
 * - CRC32 computation
 * - Trailer generation
 * - Streaming with various buffer sizes
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_gzip_encoder fuzz/fuzz_gzip_encoder.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/gzip_encoder -o fuzz/findings/gzip_encoder \
 *       -- ./fuzz_gzip_encoder
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/gzip.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/stream.h"

// Maximum input size to prevent excessive memory usage
#define MAX_INPUT_SIZE (1024 * 1024) // 1 MB

// Output buffer size - slightly larger than input for incompressible data
#define OUTPUT_BUFFER_SIZE (MAX_INPUT_SIZE + MAX_INPUT_SIZE / 10 + 1024)

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
 * @brief Fuzz the gzip encoder using the streaming API with various options
 */
static void fuzz_encoder_streaming(
    const uint8_t * input, size_t input_size, uint8_t * output, int level) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_status_t status;

  // Create encoder with various options based on input hash
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_int64(opts, "deflate.level", level);

    // Use first byte of input to decide which optional fields to add
    if (input_size > 0) {
      uint8_t flags = input[0];
      if (flags & 0x01) {
        gcomp_options_set_string(opts, "gzip.name", "fuzz_test.bin");
      }
      if (flags & 0x02) {
        gcomp_options_set_string(opts, "gzip.comment", "AFL fuzzer test");
      }
      if (flags & 0x04) {
        gcomp_options_set_bool(opts, "gzip.header_crc", 1);
      }
      if (flags & 0x08) {
        // Add extra field
        uint8_t extra[4] = {0xAB, 0xCD, 0x02, 0x00};
        gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
      }
    }
  }

  status = gcomp_encoder_create(NULL, "gzip", opts, &encoder);
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

  // Finish the stream
  gcomp_buffer_t out_buf = {.data = output + output_offset,
      .size = OUTPUT_BUFFER_SIZE - output_offset,
      .used = 0};
  gcomp_encoder_finish(encoder, &out_buf);

  gcomp_encoder_destroy(encoder);
}

/**
 * @brief Fuzz the gzip encoder using the simple buffer API
 */
static void fuzz_encoder_buffer(
    const uint8_t * input, size_t input_size, uint8_t * output, int level) {
  size_t output_size = OUTPUT_BUFFER_SIZE;

  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_int64(opts, "deflate.level", level);
  }

  gcomp_encode_buffer(
      NULL, "gzip", opts, input, input_size, output, output_size, &output_size);

  if (opts) {
    gcomp_options_destroy(opts);
  }
}

int main(int argc, char ** argv) {
  (void)argc;
  (void)argv;

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

  // Test multiple compression levels
  fuzz_encoder_streaming(input, input_size, output, 0); // Stored
  fuzz_encoder_streaming(input, input_size, output, 1); // Fast
  fuzz_encoder_streaming(input, input_size, output, 6); // Default
  fuzz_encoder_streaming(input, input_size, output, 9); // Maximum

  // Also test buffer API
  fuzz_encoder_buffer(input, input_size, output, 6);

  free(output);
  free(input);

  return 0;
}
