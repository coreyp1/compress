/**
 * @file fuzz_rle_roundtrip.c
 *
 * AFL++ fuzz harness for RLE encode-then-decode roundtrip testing.
 *
 * This harness compresses arbitrary input and then decompresses it using the
 * same profile, verifying that the original data is recovered exactly.
 *
 * It tests both RLE profiles (PackBits and TGA) and both buffer and streaming
 * APIs.
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

// RLE output can be slightly larger than input; leave headroom.
#define COMPRESS_BUFFER_SIZE (MAX_INPUT_SIZE + 1024 * 1024)
#define DECOMPRESS_BUFFER_SIZE (MAX_INPUT_SIZE + 1024)

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

static void test_roundtrip_buffer(const uint8_t * original,
    size_t original_size, uint8_t * compress_buf, uint8_t * decompress_buf,
    const char * format) {
  gcomp_status_t status;
  size_t compressed_size = COMPRESS_BUFFER_SIZE;
  size_t decompressed_size = DECOMPRESS_BUFFER_SIZE;

  gcomp_options_t * enc_opts = NULL;
  gcomp_options_create(&enc_opts);
  if (enc_opts) {
    gcomp_options_set_string(enc_opts, "rle.format", format);
  }

  status = gcomp_encode_buffer(NULL, "rle", enc_opts, original, original_size,
      compress_buf, compressed_size, &compressed_size);

  if (enc_opts) {
    gcomp_options_destroy(enc_opts);
  }

  if (status != GCOMP_OK) {
    return;
  }

  gcomp_options_t * dec_opts = NULL;
  gcomp_options_create(&dec_opts);
  if (dec_opts) {
    gcomp_options_set_string(dec_opts, "rle.format", format);
    gcomp_options_set_uint64(
        dec_opts, "limits.max_output_bytes", (uint64_t)DECOMPRESS_BUFFER_SIZE);
    gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0);
  }

  status = gcomp_decode_buffer(NULL, "rle", dec_opts, compress_buf,
      compressed_size, decompress_buf, decompressed_size, &decompressed_size);

  if (dec_opts) {
    gcomp_options_destroy(dec_opts);
  }

  if (status != GCOMP_OK) {
    abort();
  }

  if (decompressed_size != original_size) {
    abort();
  }
  if (original_size > 0 &&
      memcmp(original, decompress_buf, original_size) != 0) {
    abort();
  }
}

static void test_roundtrip_streaming(const uint8_t * original,
    size_t original_size, uint8_t * compress_buf, uint8_t * decompress_buf,
    const char * format) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_decoder_t * decoder = NULL;
  gcomp_status_t status;

  gcomp_options_t * enc_opts = NULL;
  gcomp_options_create(&enc_opts);
  if (enc_opts) {
    gcomp_options_set_string(enc_opts, "rle.format", format);
  }

  status = gcomp_encoder_create(NULL, "rle", enc_opts, &encoder);
  if (enc_opts) {
    gcomp_options_destroy(enc_opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  size_t in_offset = 0;
  size_t out_offset = 0;
  size_t chunk_size = 1;

  while (in_offset < original_size) {
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = original_size - in_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = (void *)(original + in_offset), .size = chunk_size, .used = 0};
    gcomp_buffer_t out_buf = {.data = compress_buf + out_offset,
        .size = COMPRESS_BUFFER_SIZE - out_offset,
        .used = 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    in_offset += in_buf.used;
    out_offset += out_buf.used;
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return;
    }
  }

  gcomp_buffer_t finish_buf = {.data = compress_buf + out_offset,
      .size = COMPRESS_BUFFER_SIZE - out_offset,
      .used = 0};
  status = gcomp_encoder_finish(encoder, &finish_buf);
  size_t compressed_size = out_offset + finish_buf.used;
  gcomp_encoder_destroy(encoder);
  if (status != GCOMP_OK) {
    return;
  }

  gcomp_options_t * dec_opts = NULL;
  gcomp_options_create(&dec_opts);
  if (dec_opts) {
    gcomp_options_set_string(dec_opts, "rle.format", format);
    gcomp_options_set_uint64(
        dec_opts, "limits.max_output_bytes", (uint64_t)DECOMPRESS_BUFFER_SIZE);
    gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0);
  }

  status = gcomp_decoder_create(NULL, "rle", dec_opts, &decoder);
  if (dec_opts) {
    gcomp_options_destroy(dec_opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  in_offset = 0;
  out_offset = 0;
  chunk_size = 1;

  while (in_offset < compressed_size) {
    chunk_size = (chunk_size * 11 + 17) % 1024 + 1;
    size_t remaining = compressed_size - in_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {.data = (void *)(compress_buf + in_offset),
        .size = chunk_size,
        .used = 0};
    gcomp_buffer_t out_buf = {.data = decompress_buf + out_offset,
        .size = DECOMPRESS_BUFFER_SIZE - out_offset,
        .used = 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    in_offset += in_buf.used;
    out_offset += out_buf.used;
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      abort();
    }
  }

  gcomp_buffer_t dec_finish_buf = {.data = decompress_buf + out_offset,
      .size = DECOMPRESS_BUFFER_SIZE - out_offset,
      .used = 0};
  status = gcomp_decoder_finish(decoder, &dec_finish_buf);
  size_t decompressed_size = out_offset + dec_finish_buf.used;
  gcomp_decoder_destroy(decoder);

  if (status != GCOMP_OK) {
    abort();
  }
  if (decompressed_size != original_size) {
    abort();
  }
  if (original_size > 0 &&
      memcmp(original, decompress_buf, original_size) != 0) {
    abort();
  }
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {
  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0;
  }

  uint8_t * compress_buf = malloc(COMPRESS_BUFFER_SIZE);
  uint8_t * decompress_buf = malloc(DECOMPRESS_BUFFER_SIZE);
  if (!compress_buf || !decompress_buf) {
    free(input);
    free(compress_buf);
    free(decompress_buf);
    return 0;
  }

  test_roundtrip_buffer(
      input, input_size, compress_buf, decompress_buf, "packbits");
  test_roundtrip_buffer(input, input_size, compress_buf, decompress_buf, "tga");
  test_roundtrip_streaming(
      input, input_size, compress_buf, decompress_buf, "packbits");
  test_roundtrip_streaming(
      input, input_size, compress_buf, decompress_buf, "tga");

  free(decompress_buf);
  free(compress_buf);
  free(input);
  return 0;
}
