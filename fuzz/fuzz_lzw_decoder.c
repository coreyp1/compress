/**
 * @file fuzz_lzw_decoder.c
 *
 * AFL++ fuzz harness for the LZW decoder.
 *
 * This harness reads arbitrary bytes from stdin and attempts to decode them
 * as LZW streams using both supported profiles (GIF LSB-first and TIFF
 * MSB-first).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/lzw.h"
#include "ghoti.io/compress/macros.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/stream.h"

// Keep input modest; some malformed LZW can be very slow without caps.
#define MAX_INPUT_SIZE (256 * 1024) // 256 KB

// Output buffer cap (also enforced via limits).
#define OUTPUT_BUFFER_SIZE (4 * 1024 * 1024) // 4 MB

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

static void make_decoder_opts(
    gcomp_options_t ** opts_out, const char * format) {
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_string(opts, "lzw.format", format);
    gcomp_options_set_uint64(
        opts, "limits.max_output_bytes", OUTPUT_BUFFER_SIZE);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000);
    gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 32 * 1024 * 1024);
  }
  *opts_out = opts;
}

static void fuzz_decoder_streaming(const uint8_t * input, size_t input_size,
    uint8_t * output, const char * format) {
  gcomp_decoder_t * decoder = NULL;
  gcomp_status_t status;

  gcomp_options_t * opts = NULL;
  make_decoder_opts(&opts, format);

  status = gcomp_decoder_create(NULL, "lzw", opts, &decoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  size_t input_offset = 0;
  size_t output_offset = 0;
  size_t chunk_size = 1;

  while (input_offset < input_size) {
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

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    input_offset += in_buf.used;
    output_offset += out_buf.used;

    if (status != GCOMP_OK && status != GCOMP_ERR_LIMIT) {
      break;
    }
  }

  gcomp_buffer_t out_buf = {.data = output + output_offset,
      .size = OUTPUT_BUFFER_SIZE - output_offset,
      .used = 0};
  gcomp_decoder_finish(decoder, &out_buf);
  gcomp_decoder_destroy(decoder);
}

static void fuzz_decoder_buffer(const uint8_t * input, size_t input_size,
    uint8_t * output, const char * format) {
  size_t output_size = OUTPUT_BUFFER_SIZE;

  gcomp_options_t * opts = NULL;
  make_decoder_opts(&opts, format);

  gcomp_decode_buffer(
      NULL, "lzw", opts, input, input_size, output, output_size, &output_size);

  if (opts) {
    gcomp_options_destroy(opts);
  }
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {
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

  // Try both supported LZW profiles to maximize coverage.
  fuzz_decoder_streaming(input, input_size, output, "gif");
  fuzz_decoder_buffer(input, input_size, output, "gif");
  fuzz_decoder_streaming(input, input_size, output, "tiff");
  fuzz_decoder_buffer(input, input_size, output, "tiff");

  free(output);
  free(input);
  return 0;
}
