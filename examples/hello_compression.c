/**
 * @file hello_compression.c
 *
 * Minimal example demonstrating buffer-to-buffer compression with the
 * Ghoti.io Compress library using DEFLATE (RFC 1951).
 *
 * This example shows how to:
 * - Register the deflate compression method
 * - Compress a string using gcomp_encode_buffer()
 * - Decompress the data using gcomp_decode_buffer()
 * - Verify the round-trip produces the original data
 *
 * Build: See Makefile target "examples"
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/stream.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  gcomp_status_t status;

  // The message we want to compress
  const char * message = "Hello, Compression! This is a test of the Ghoti.io "
                         "Compress library using DEFLATE encoding.";
  size_t message_len = strlen(message);

  printf("=== Ghoti.io Compress Library - Hello Compression Example ===\n\n");
  printf("Library version: %s\n\n", gcomp_version_string());

  // Step 1: Get the default registry and register the deflate method
  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "Error: Failed to get default registry\n");
    return 1;
  }

  status = gcomp_method_deflate_register(registry);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Error: Failed to register deflate method: %s\n",
        gcomp_status_to_string(status));
    return 1;
  }

  printf("Original message (%zu bytes):\n  \"%s\"\n\n", message_len, message);

  // Step 2: Compress the message
  // Allocate a buffer for compressed output. For small inputs, compressed
  // data might actually be larger due to overhead, so we allocate generously.
  size_t compressed_capacity = message_len + 256;
  uint8_t * compressed = (uint8_t *)malloc(compressed_capacity);
  if (!compressed) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    return 1;
  }

  size_t compressed_size = 0;
  status = gcomp_encode_buffer(registry, "deflate", NULL, /* default options */
      message, message_len, compressed, compressed_capacity, &compressed_size);

  if (status != GCOMP_OK) {
    fprintf(stderr, "Error: Compression failed: %s\n",
        gcomp_status_to_string(status));
    free(compressed);
    return 1;
  }

  printf("Compressed size: %zu bytes (%.1f%% of original)\n", compressed_size,
      (100.0 * compressed_size) / message_len);

  // Print first few bytes of compressed data (hex)
  printf("Compressed data (first 32 bytes): ");
  for (size_t i = 0; i < compressed_size && i < 32; i++) {
    printf("%02x", compressed[i]);
  }
  if (compressed_size > 32) {
    printf("...");
  }
  printf("\n\n");

  // Step 3: Decompress the message
  // We know the original size, but in practice you might need to allocate
  // more and handle GCOMP_ERR_LIMIT (output buffer too small).
  size_t decompressed_capacity = message_len + 1; // +1 for null terminator
  uint8_t * decompressed = (uint8_t *)malloc(decompressed_capacity);
  if (!decompressed) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    free(compressed);
    return 1;
  }

  size_t decompressed_size = 0;
  status = gcomp_decode_buffer(registry, "deflate", NULL, /* default options */
      compressed, compressed_size, decompressed, decompressed_capacity,
      &decompressed_size);

  if (status != GCOMP_OK) {
    fprintf(stderr, "Error: Decompression failed: %s\n",
        gcomp_status_to_string(status));
    free(compressed);
    free(decompressed);
    return 1;
  }

  printf("Decompressed size: %zu bytes\n", decompressed_size);

  // Null-terminate for safe printing
  decompressed[decompressed_size] = '\0';
  printf("Decompressed message:\n  \"%s\"\n\n", decompressed);

  // Step 4: Verify the round-trip
  if (decompressed_size == message_len &&
      memcmp(message, decompressed, message_len) == 0) {
    printf("SUCCESS: Round-trip compression verified!\n");
  }
  else {
    printf("FAILURE: Decompressed data doesn't match original!\n");
    free(compressed);
    free(decompressed);
    return 1;
  }

  // Cleanup
  free(compressed);
  free(decompressed);

  return 0;
}
