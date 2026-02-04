/**
 * @file fuzz_lzw_encoder.c
 *
 * AFL++ fuzz harness for the LZW encoder.
 *
 * This harness reads arbitrary plaintext bytes from stdin and attempts to
 * encode them using both supported LZW profiles (GIF and TIFF), using both
 * streaming and buffer APIs.
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

#define MAX_INPUT_SIZE (256 * 1024) // 256 KB

// Worst-case LZW output can grow; keep headroom but bounded.
#define OUTPUT_BUFFER_SIZE (MAX_INPUT_SIZE + 1024 * 1024)

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

static void make_encoder_opts(
    gcomp_options_t ** opts_out, const char * format, uint64_t max_code_bits) {
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_string(opts, "lzw.format", format);
    gcomp_options_set_uint64(opts, "lzw.max_code_bits", max_code_bits);
  }
  *opts_out = opts;
}

static void fuzz_encoder_buffer(const uint8_t * input, size_t input_size,
    uint8_t * output, const char * format, uint64_t max_code_bits) {
  size_t output_size = OUTPUT_BUFFER_SIZE;

  gcomp_options_t * opts = NULL;
  make_encoder_opts(&opts, format, max_code_bits);

  gcomp_encode_buffer(
      NULL, "lzw", opts, input, input_size, output, output_size, &output_size);

  if (opts) {
    gcomp_options_destroy(opts);
  }
}

static void fuzz_encoder_streaming(const uint8_t * input, size_t input_size,
    uint8_t * output, const char * format, uint64_t max_code_bits) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_status_t status;

  gcomp_options_t * opts = NULL;
  make_encoder_opts(&opts, format, max_code_bits);

  status = gcomp_encoder_create(NULL, "lzw", opts, &encoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  size_t in_offset = 0;
  size_t out_offset = 0;
  size_t chunk_size = 1;

  while (in_offset < input_size) {
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = input_size - in_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = (void *)(input + in_offset), .size = chunk_size, .used = 0};
    gcomp_buffer_t out_buf = {.data = output + out_offset,
        .size = OUTPUT_BUFFER_SIZE - out_offset,
        .used = 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    in_offset += in_buf.used;
    out_offset += out_buf.used;
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return;
    }
  }

  gcomp_buffer_t finish_buf = {.data = output + out_offset,
      .size = OUTPUT_BUFFER_SIZE - out_offset,
      .used = 0};
  gcomp_encoder_finish(encoder, &finish_buf);
  gcomp_encoder_destroy(encoder);
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

  // Choose some deterministic option variations from input.
  uint64_t max_code_bits = 12;
  if (input_size > 0) {
    // Map to [9, 12] (inclusive). Lower widths stress early dictionary limits.
    max_code_bits = 9 + (uint64_t)(input[0] % 4);
  }

  fuzz_encoder_streaming(input, input_size, output, "gif", max_code_bits);
  fuzz_encoder_buffer(input, input_size, output, "gif", max_code_bits);
  fuzz_encoder_streaming(input, input_size, output, "tiff", max_code_bits);
  fuzz_encoder_buffer(input, input_size, output, "tiff", max_code_bits);

  free(output);
  free(input);
  return 0;
}
