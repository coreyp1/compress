/**
 * @file fuzz_lz4_roundtrip.c
 *
 * AFL++ fuzz harness for LZ4 roundtrip testing.
 *
 * This harness compresses arbitrary input and then decompresses it,
 * verifying that the original data is recovered exactly. This catches
 * bugs where encode+decode produces incorrect output without crashing.
 *
 * Key areas being tested:
 * - Encoder/decoder consistency
 * - All compression paths produce decodable output
 * - Edge cases in both encoder and decoder
 * - Various option combinations
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_lz4_roundtrip fuzz/fuzz_lz4_roundtrip.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/lz4_roundtrip -o fuzz/findings/lz4_roundtrip \
 *       -- ./fuzz_lz4_roundtrip
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

// Output buffer sizes
#define COMPRESS_BUFFER_SIZE (MAX_INPUT_SIZE + 1024 * 1024)
#define DECOMPRESS_BUFFER_SIZE (MAX_INPUT_SIZE + 1024)

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
 * @brief Test roundtrip with specific options
 */
static void test_roundtrip(const uint8_t * original, size_t original_size,
    uint8_t * compress_buf, uint8_t * decompress_buf, int content_checksum,
    int block_checksum, uint64_t block_size) {
  gcomp_status_t status;
  size_t compressed_size = COMPRESS_BUFFER_SIZE;
  size_t decompressed_size = DECOMPRESS_BUFFER_SIZE;

  // Set up encoder options
  gcomp_options_t * enc_opts = NULL;
  gcomp_options_create(&enc_opts);
  if (enc_opts) {
    gcomp_options_set_bool(enc_opts, "lz4.content_checksum", content_checksum);
    gcomp_options_set_bool(enc_opts, "lz4.block_checksum", block_checksum);
    gcomp_options_set_uint64(enc_opts, "lz4.block_size", block_size);
    gcomp_options_set_bool(enc_opts, "lz4.independent_blocks", 1);
  }

  // Compress
  status = gcomp_encode_buffer(NULL, "lz4", enc_opts, original, original_size,
      compress_buf, compressed_size, &compressed_size);

  if (enc_opts) {
    gcomp_options_destroy(enc_opts);
  }

  if (status != GCOMP_OK) {
    // Compression failed - not necessarily a bug (could be OOM or limit)
    return;
  }

  // Set up decoder options
  gcomp_options_t * dec_opts = NULL;
  gcomp_options_create(&dec_opts);
  if (dec_opts) {
    // Disable expansion ratio limit for roundtrip tests
    gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0);
  }

  // Decompress
  status = gcomp_decode_buffer(NULL, "lz4", dec_opts, compress_buf,
      compressed_size, decompress_buf, decompressed_size, &decompressed_size);

  if (dec_opts) {
    gcomp_options_destroy(dec_opts);
  }

  if (status != GCOMP_OK) {
    // Decompression of our own output failed - this is a bug!
    abort();
  }

  // Verify size matches
  if (decompressed_size != original_size) {
    // Size mismatch - this is a bug!
    abort();
  }

  // Verify content matches
  if (original_size > 0 &&
      memcmp(original, decompress_buf, original_size) != 0) {
    // Content mismatch - this is a bug!
    abort();
  }
}

/**
 * @brief Test roundtrip using streaming API
 */
static void test_roundtrip_streaming(const uint8_t * original,
    size_t original_size, uint8_t * compress_buf, uint8_t * decompress_buf) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_decoder_t * decoder = NULL;
  gcomp_status_t status;

  // Create encoder
  gcomp_options_t * enc_opts = NULL;
  gcomp_options_create(&enc_opts);
  if (enc_opts) {
    gcomp_options_set_bool(enc_opts, "lz4.content_checksum", 1);
    gcomp_options_set_bool(enc_opts, "lz4.independent_blocks", 1);
  }

  status = gcomp_encoder_create(NULL, "lz4", enc_opts, &encoder);
  if (enc_opts) {
    gcomp_options_destroy(enc_opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  // Compress in chunks
  size_t in_offset = 0;
  size_t out_offset = 0;
  size_t chunk_size = 1;

  while (in_offset < original_size) {
    chunk_size = (chunk_size * 7 + 13) % 512 + 1;
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

  // Finish encoding
  gcomp_buffer_t finish_buf = {.data = compress_buf + out_offset,
      .size = COMPRESS_BUFFER_SIZE - out_offset,
      .used = 0};
  status = gcomp_encoder_finish(encoder, &finish_buf);
  size_t compressed_size = out_offset + finish_buf.used;
  gcomp_encoder_destroy(encoder);

  if (status != GCOMP_OK) {
    return;
  }

  // Create decoder
  gcomp_options_t * dec_opts = NULL;
  gcomp_options_create(&dec_opts);
  if (dec_opts) {
    gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0);
  }

  status = gcomp_decoder_create(NULL, "lz4", dec_opts, &decoder);
  if (dec_opts) {
    gcomp_options_destroy(dec_opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  // Decompress in chunks
  in_offset = 0;
  out_offset = 0;
  chunk_size = 1;

  while (in_offset < compressed_size) {
    chunk_size = (chunk_size * 11 + 17) % 512 + 1;
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
      abort(); // Our own output should always decompress
    }
  }

  // Finish decoding
  gcomp_buffer_t dec_finish_buf = {.data = decompress_buf + out_offset,
      .size = DECOMPRESS_BUFFER_SIZE - out_offset,
      .used = 0};
  status = gcomp_decoder_finish(decoder, &dec_finish_buf);
  size_t decompressed_size = out_offset + dec_finish_buf.used;
  gcomp_decoder_destroy(decoder);

  if (status != GCOMP_OK) {
    abort(); // Finish should succeed
  }

  // Verify
  if (decompressed_size != original_size) {
    abort();
  }
  if (original_size > 0 &&
      memcmp(original, decompress_buf, original_size) != 0) {
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
  uint8_t * compress_buf = malloc(COMPRESS_BUFFER_SIZE);
  uint8_t * decompress_buf = malloc(DECOMPRESS_BUFFER_SIZE);
  if (!compress_buf || !decompress_buf) {
    free(input);
    free(compress_buf);
    free(decompress_buf);
    return 0;
  }

  // Test roundtrip with various options using buffer API
  test_roundtrip(input, input_size, compress_buf, decompress_buf, 0, 0, 65536);
  test_roundtrip(input, input_size, compress_buf, decompress_buf, 1, 0, 262144);
  test_roundtrip(
      input, input_size, compress_buf, decompress_buf, 0, 1, 1048576);
  test_roundtrip(
      input, input_size, compress_buf, decompress_buf, 1, 1, 4194304);

  // Test roundtrip with streaming API
  test_roundtrip_streaming(input, input_size, compress_buf, decompress_buf);

  free(decompress_buf);
  free(compress_buf);
  free(input);

  return 0;
}
