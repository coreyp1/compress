/**
 * @file fuzz_gzip_decoder.c
 *
 * AFL++ fuzz harness for the gzip decoder.
 *
 * This harness reads arbitrary bytes from stdin and attempts to decode them
 * as a gzip stream (RFC 1952). The goal is to find inputs that cause crashes,
 * hangs, or undefined behavior in the decoder.
 *
 * Key areas being tested:
 * - Header parsing (magic bytes, flags, optional fields)
 * - DEFLATE decompression via inner decoder
 * - Trailer validation (CRC32, ISIZE)
 * - Concatenated member handling
 * - Limit enforcement (expansion ratio, output size, header field sizes)
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_gzip_decoder fuzz/fuzz_gzip_decoder.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/gzip_decoder -o fuzz/findings/gzip_decoder \
 *       -- ./fuzz_gzip_decoder
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

// Output buffer size - large enough for most decompressed outputs
#define OUTPUT_BUFFER_SIZE (4 * 1024 * 1024) // 4 MB

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
 * @brief Fuzz the gzip decoder using the streaming API
 *
 * This exercises more code paths than the buffer API by processing
 * input in chunks, testing streaming header/body/trailer parsing.
 */
static void fuzz_decoder_streaming(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  gcomp_decoder_t * decoder = NULL;
  gcomp_status_t status;

  // Create decoder with concat enabled to test multi-member handling
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_bool(opts, "gzip.concat", 1);
    // Set reasonable limits to prevent fuzzer from timing out on bombs
    gcomp_options_set_uint64(
        opts, "limits.max_output_bytes", OUTPUT_BUFFER_SIZE);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);
    gcomp_options_set_uint64(opts, "gzip.max_name_bytes", 4096);
    gcomp_options_set_uint64(opts, "gzip.max_comment_bytes", 4096);
    gcomp_options_set_uint64(opts, "gzip.max_extra_bytes", 4096);
  }

  status = gcomp_decoder_create(NULL, "gzip", opts, &decoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  // Process input in variable-sized chunks to stress test the streaming
  size_t input_offset = 0;
  size_t output_offset = 0;
  size_t chunk_size = 1; // Start with 1-byte chunks, vary later

  while (input_offset < input_size) {
    // Vary chunk size to test different streaming patterns
    // This catches bugs where header fields cross chunk boundaries
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = input_size - input_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = (void *)(input + input_offset), .size = chunk_size, .used = 0};

    size_t out_remaining = OUTPUT_BUFFER_SIZE - output_offset;
    if (out_remaining == 0) {
      // Output buffer full, stop
      break;
    }

    gcomp_buffer_t out_buf = {
        .data = output + output_offset, .size = out_remaining, .used = 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);

    input_offset += in_buf.used;
    output_offset += out_buf.used;

    // Stop on error (but don't crash - errors are expected for fuzz input)
    if (status != GCOMP_OK && status != GCOMP_ERR_LIMIT) {
      break;
    }
  }

  // Try to finish (may fail for incomplete/corrupt streams - that's fine)
  gcomp_buffer_t out_buf = {.data = output + output_offset,
      .size = OUTPUT_BUFFER_SIZE - output_offset,
      .used = 0};
  gcomp_decoder_finish(decoder, &out_buf);

  gcomp_decoder_destroy(decoder);
}

/**
 * @brief Fuzz the gzip decoder using the simple buffer API
 */
static void fuzz_decoder_buffer(
    const uint8_t * input, size_t input_size, uint8_t * output) {
  size_t output_size = OUTPUT_BUFFER_SIZE;

  // Set reasonable limits
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_uint64(
        opts, "limits.max_output_bytes", OUTPUT_BUFFER_SIZE);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);
  }

  // This may fail for invalid input - that's expected and fine
  gcomp_decode_buffer(
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
    return 0; // Memory allocation failure - not a bug in the library
  }

  // Allocate output buffer
  uint8_t * output = malloc(OUTPUT_BUFFER_SIZE);
  if (!output) {
    free(input);
    return 0;
  }

  // Test both APIs to maximize coverage
  fuzz_decoder_streaming(input, input_size, output);
  fuzz_decoder_buffer(input, input_size, output);

  free(output);
  free(input);

  return 0;
}
