/**
 * @file fuzz_gzip_roundtrip.c
 *
 * AFL++ fuzz harness for gzip roundtrip testing.
 *
 * This harness reads arbitrary bytes from stdin, compresses them as gzip,
 * then decompresses the result and verifies the output matches the input.
 * This catches subtle encoding/decoding mismatches and data corruption.
 *
 * Key areas being tested:
 * - CRC32 computation consistency between encoder and decoder
 * - ISIZE tracking consistency
 * - Round-trip data integrity at all compression levels
 * - Various optional header field combinations
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_gzip_roundtrip fuzz/fuzz_gzip_roundtrip.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/gzip_roundtrip -o fuzz/findings/gzip_roundtrip \
 *       -- ./fuzz_gzip_roundtrip
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
#define MAX_INPUT_SIZE (256 * 1024) // 256 KB (smaller for roundtrip)

// Buffer sizes
#define COMPRESSED_BUFFER_SIZE (MAX_INPUT_SIZE + MAX_INPUT_SIZE / 10 + 1024)
#define DECOMPRESSED_BUFFER_SIZE (MAX_INPUT_SIZE + 1024)

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
 * @brief Test roundtrip with specific options
 *
 * @param input Original input data
 * @param input_size Size of input
 * @param compressed Buffer for compressed data
 * @param decompressed Buffer for decompressed data
 * @param level Compression level
 * @param use_name Include FNAME field
 * @param use_comment Include FCOMMENT field
 * @param use_hcrc Include FHCRC field
 */
static void test_roundtrip(const uint8_t * input, size_t input_size,
    uint8_t * compressed, uint8_t * decompressed, int level, int use_name,
    int use_comment, int use_hcrc) {

  gcomp_status_t status;
  size_t compressed_size = COMPRESSED_BUFFER_SIZE;
  size_t decompressed_size = DECOMPRESSED_BUFFER_SIZE;

  // Create encoder options
  gcomp_options_t * enc_opts = NULL;
  gcomp_options_create(&enc_opts);
  if (enc_opts) {
    gcomp_options_set_int64(enc_opts, "deflate.level", level);
    if (use_name) {
      gcomp_options_set_string(enc_opts, "gzip.name", "roundtrip.bin");
    }
    if (use_comment) {
      gcomp_options_set_string(enc_opts, "gzip.comment", "roundtrip test");
    }
    if (use_hcrc) {
      gcomp_options_set_bool(enc_opts, "gzip.header_crc", 1);
    }
  }

  // Compress
  status = gcomp_encode_buffer(NULL, "gzip", enc_opts, input, input_size,
      compressed, COMPRESSED_BUFFER_SIZE, &compressed_size);

  if (enc_opts) {
    gcomp_options_destroy(enc_opts);
  }

  if (status != GCOMP_OK) {
    // Compression failed - this shouldn't happen for valid input
    // but isn't necessarily a crash bug
    return;
  }

  // Decompress
  status = gcomp_decode_buffer(NULL, "gzip", NULL, compressed, compressed_size,
      decompressed, DECOMPRESSED_BUFFER_SIZE, &decompressed_size);

  if (status != GCOMP_OK) {
    // Decompression of our own output failed - this IS a bug!
    // The fuzzer will catch this as a crash if we abort
    fprintf(stderr,
        "FATAL: Failed to decompress our own gzip output! "
        "level=%d, name=%d, comment=%d, hcrc=%d, status=%d\n",
        level, use_name, use_comment, use_hcrc, status);
    abort();
  }

  // Verify size matches
  if (decompressed_size != input_size) {
    fprintf(stderr,
        "FATAL: Roundtrip size mismatch! "
        "input=%zu, decompressed=%zu, level=%d\n",
        input_size, decompressed_size, level);
    abort();
  }

  // Verify content matches
  if (memcmp(input, decompressed, input_size) != 0) {
    fprintf(stderr,
        "FATAL: Roundtrip data corruption! "
        "input_size=%zu, level=%d\n",
        input_size, level);
    abort();
  }
}

int main(int argc, char ** argv) {
  (void)argc;
  (void)argv;

  // Read input from stdin
  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input || input_size == 0) {
    free(input);
    return 0;
  }

  // Allocate buffers
  uint8_t * compressed = malloc(COMPRESSED_BUFFER_SIZE);
  uint8_t * decompressed = malloc(DECOMPRESSED_BUFFER_SIZE);
  if (!compressed || !decompressed) {
    free(input);
    free(compressed);
    free(decompressed);
    return 0;
  }

  // Test various configurations based on input
  // Use first byte to vary test parameters
  uint8_t flags = input_size > 0 ? input[0] : 0;
  int level = (flags & 0x0F) % 10; // 0-9
  int use_name = (flags & 0x10) != 0;
  int use_comment = (flags & 0x20) != 0;
  int use_hcrc = (flags & 0x40) != 0;

  // Test with the derived configuration
  test_roundtrip(input, input_size, compressed, decompressed, level, use_name,
      use_comment, use_hcrc);

  // Also test with fixed configurations for consistency
  test_roundtrip(input, input_size, compressed, decompressed, 0, 0, 0, 0);
  test_roundtrip(input, input_size, compressed, decompressed, 6, 1, 1, 1);

  free(decompressed);
  free(compressed);
  free(input);

  return 0;
}
