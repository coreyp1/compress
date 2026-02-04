/**
 * @file fuzz_rle_encoder.c
 *
 * AFL++ fuzz harness for the RLE encoder.
 *
 * This harness reads arbitrary plaintext bytes from stdin and attempts to
 * encode them using both supported RLE profiles (PackBits and TGA), using
 * both streaming and buffer APIs.
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
#include "ghoti.io/compress/rle.h"
#include "ghoti.io/compress/stream.h"

#define MAX_INPUT_SIZE (256 * 1024) // 256 KB

// RLE overhead is small; allocate extra slack for profile headers.
#define OUTPUT_BUFFER_SIZE (MAX_INPUT_SIZE + 1024 * 1024) // +1 MB

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

static void fuzz_encoder_buffer(const uint8_t * input, size_t input_size,
    uint8_t * output, const char * format) {
  size_t output_size = OUTPUT_BUFFER_SIZE;
  gcomp_status_t status;

  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_string(opts, "rle.format", format);
  }

  status = gcomp_encode_buffer(
      NULL, "rle", opts, input, input_size, output, output_size, &output_size);

  if (opts) {
    gcomp_options_destroy(opts);
  }

  (void)status; // errors are expected for fuzz input
}

static void fuzz_encoder_streaming(const uint8_t * input, size_t input_size,
    uint8_t * output, const char * format) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_status_t status;

  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_string(opts, "rle.format", format);
  }

  status = gcomp_encoder_create(NULL, "rle", opts, &encoder);
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

  fuzz_encoder_streaming(input, input_size, output, "packbits");
  fuzz_encoder_buffer(input, input_size, output, "packbits");
  fuzz_encoder_streaming(input, input_size, output, "tga");
  fuzz_encoder_buffer(input, input_size, output, "tga");

  free(output);
  free(input);
  return 0;
}
