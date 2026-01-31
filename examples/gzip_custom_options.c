/**
 * @file gzip_custom_options.c
 *
 * Example demonstrating gzip options configuration with the Ghoti.io
 * Compress library.
 *
 * This example shows how to:
 * - Configure compression level and strategy
 * - Set gzip header metadata (filename, comment, extra field)
 * - Enable header CRC and FTEXT flags
 * - Configure safety limits for decompression
 * - Handle concatenated gzip streams
 *
 * Build: See Makefile target "examples"
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/stream.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Example 1: Basic compression level configuration
 */
static void example_compression_levels(gcomp_registry_t * registry) {
  printf("\n=== Example 1: Compression Levels ===\n");

  const char * data = "This is sample data that will be compressed at "
                      "different levels to show the trade-off between "
                      "compression ratio and speed. Repeated data like "
                      "this this this this benefits from higher levels.";
  size_t data_len = strlen(data);

  int levels[] = {1, 6, 9};
  const char * level_names[] = {"fast", "default", "best"};

  for (int i = 0; i < 3; i++) {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    gcomp_options_set_int64(opts, "deflate.level", levels[i]);

    uint8_t output[1024];
    size_t output_size = 0;
    gcomp_status_t status = gcomp_encode_buffer(registry, "gzip", opts, data,
        data_len, output, sizeof(output), &output_size);

    if (status == GCOMP_OK) {
      printf("  Level %d (%s): %zu bytes -> %zu bytes (%.1f%%)\n", levels[i],
          level_names[i], data_len, output_size,
          (100.0 * output_size) / data_len);
    }

    gcomp_options_destroy(opts);
  }
}

/**
 * @brief Example 2: Gzip header metadata
 */
static void example_header_metadata(gcomp_registry_t * registry) {
  printf("\n=== Example 2: Gzip Header Metadata ===\n");

  gcomp_options_t * opts = NULL;
  gcomp_status_t status = gcomp_options_create(&opts);
  if (status != GCOMP_OK)
    return;

  // Set filename that will be stored in the gzip header
  gcomp_options_set_string(opts, "gzip.name", "example.txt");

  // Set file comment
  gcomp_options_set_string(
      opts, "gzip.comment", "Created by Ghoti.io Compress");

  // Set modification time (Unix timestamp)
  gcomp_options_set_uint64(opts, "gzip.mtime", 1706745600); // Example timestamp

  // Set operating system (255 = unknown, see RFC 1952 for other values)
  gcomp_options_set_uint64(opts, "gzip.os", 3); // 3 = Unix

  // Enable header CRC for extra integrity
  gcomp_options_set_bool(opts, "gzip.header_crc", 1);

  // Indicate this is ASCII text content
  gcomp_options_set_bool(opts, "gzip.text", 1);

  const char * data = "Hello from gzip with custom metadata!";
  uint8_t output[256];
  size_t output_size = 0;

  status = gcomp_encode_buffer(
      registry, "gzip", opts, data, strlen(data), output, sizeof(output), &output_size);

  if (status == GCOMP_OK) {
    printf("  Compressed with metadata: %zu bytes\n", output_size);

    // Print header bytes to show flags
    printf("  Header (first 20 bytes): ");
    for (size_t i = 0; i < 20 && i < output_size; i++) {
      printf("%02x ", output[i]);
    }
    printf("\n");

    // Decode and show the flags
    if (output_size >= 4) {
      uint8_t flags = output[3];
      printf("  Flags: 0x%02x", flags);
      if (flags & 0x01)
        printf(" FTEXT");
      if (flags & 0x02)
        printf(" FHCRC");
      if (flags & 0x04)
        printf(" FEXTRA");
      if (flags & 0x08)
        printf(" FNAME");
      if (flags & 0x10)
        printf(" FCOMMENT");
      printf("\n");
    }
  }

  gcomp_options_destroy(opts);
}

/**
 * @brief Example 3: Extra field usage
 */
static void example_extra_field(gcomp_registry_t * registry) {
  printf("\n=== Example 3: Extra Field (FEXTRA) ===\n");

  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);

  // Extra field format: pairs of (SI1, SI2, LEN, DATA)
  // SI1/SI2 are subfield identifiers (2 bytes each)
  // This example uses "GH" (0x47, 0x48) as a custom identifier
  uint8_t extra_data[] = {
      'G', 'H',      // SI1, SI2 (subfield ID)
      0x04, 0x00,    // LEN = 4 (little-endian)
      'T', 'E', 'S', 'T' // Subfield data
  };

  gcomp_options_set_bytes(opts, "gzip.extra", extra_data, sizeof(extra_data));

  const char * data = "Data with extra field";
  uint8_t output[256];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(
      registry, "gzip", opts, data, strlen(data), output, sizeof(output), &output_size);

  if (status == GCOMP_OK) {
    printf("  Compressed with extra field: %zu bytes\n", output_size);
    printf("  Extra field can store application-specific metadata\n");
  }

  gcomp_options_destroy(opts);
}

/**
 * @brief Example 4: Decompression safety limits
 */
static void example_safety_limits(gcomp_registry_t * registry) {
  printf("\n=== Example 4: Safety Limits for Decompression ===\n");

  // First, create some compressed data
  const char * data = "Test data for safety limits example";
  uint8_t compressed[256];
  size_t compressed_size = 0;

  gcomp_encode_buffer(registry, "gzip", NULL, data, strlen(data), compressed,
      sizeof(compressed), &compressed_size);

  // Now decompress with strict limits
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);

  // Limit maximum output size to 100 bytes
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 100);

  // Limit expansion ratio (output/input) to 10x
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 10);

  // Limit memory usage
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1024 * 1024);

  // Limit header field sizes (for untrusted input)
  gcomp_options_set_uint64(opts, "gzip.max_name_bytes", 256);
  gcomp_options_set_uint64(opts, "gzip.max_comment_bytes", 1024);
  gcomp_options_set_uint64(opts, "gzip.max_extra_bytes", 1024);

  uint8_t output[256];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_decode_buffer(registry, "gzip", opts,
      compressed, compressed_size, output, sizeof(output), &output_size);

  if (status == GCOMP_OK) {
    printf("  Decompression with limits: OK (%zu bytes)\n", output_size);
  } else {
    printf("  Decompression with limits: %s\n", gcomp_status_to_string(status));
  }

  printf("  Limits protect against:\n");
  printf("    - Decompression bombs (max_output_bytes, max_expansion_ratio)\n");
  printf("    - Memory exhaustion (max_memory_bytes)\n");
  printf("    - Malicious headers (max_name_bytes, max_comment_bytes)\n");

  gcomp_options_destroy(opts);
}

/**
 * @brief Example 5: Concatenated gzip streams
 */
static void example_concatenated_streams(gcomp_registry_t * registry) {
  printf("\n=== Example 5: Concatenated Gzip Streams ===\n");

  // Create two separate gzip streams
  const char * data1 = "First gzip member";
  const char * data2 = "Second gzip member";

  uint8_t comp1[128], comp2[128];
  size_t size1 = 0, size2 = 0;

  gcomp_encode_buffer(
      registry, "gzip", NULL, data1, strlen(data1), comp1, sizeof(comp1), &size1);
  gcomp_encode_buffer(
      registry, "gzip", NULL, data2, strlen(data2), comp2, sizeof(comp2), &size2);

  // Concatenate them
  uint8_t concatenated[256];
  memcpy(concatenated, comp1, size1);
  memcpy(concatenated + size1, comp2, size2);
  size_t total_size = size1 + size2;

  printf("  Created concatenated stream: %zu + %zu = %zu bytes\n", size1, size2,
      total_size);

  // Decode without concat support (only first member)
  {
    uint8_t output[256];
    size_t output_size = 0;

    gcomp_status_t status = gcomp_decode_buffer(registry, "gzip", NULL,
        concatenated, total_size, output, sizeof(output), &output_size);

    if (status == GCOMP_OK) {
      output[output_size] = '\0';
      printf("  Without gzip.concat: '%s' (%zu bytes)\n", output, output_size);
    }
  }

  // Decode with concat support (both members)
  {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    gcomp_options_set_bool(opts, "gzip.concat", 1);

    uint8_t output[256];
    size_t output_size = 0;

    gcomp_status_t status = gcomp_decode_buffer(registry, "gzip", opts,
        concatenated, total_size, output, sizeof(output), &output_size);

    if (status == GCOMP_OK) {
      output[output_size] = '\0';
      printf("  With gzip.concat:    '%s' (%zu bytes)\n", output, output_size);
    }

    gcomp_options_destroy(opts);
  }
}

/**
 * @brief Example 6: Compression strategies
 */
static void example_strategies(gcomp_registry_t * registry) {
  printf("\n=== Example 6: Compression Strategies ===\n");

  // Different data types benefit from different strategies
  const char * text_data = "The quick brown fox jumps over the lazy dog. "
                           "The quick brown fox jumps over the lazy dog.";
  const char * strategies[] = {"default", "filtered", "huffman_only", "rle"};

  for (int i = 0; i < 4; i++) {
    gcomp_options_t * opts = NULL;
    gcomp_options_create(&opts);
    gcomp_options_set_string(opts, "deflate.strategy", strategies[i]);
    gcomp_options_set_int64(opts, "deflate.level", 6);

    uint8_t output[1024];
    size_t output_size = 0;

    gcomp_status_t status = gcomp_encode_buffer(registry, "gzip", opts,
        text_data, strlen(text_data), output, sizeof(output), &output_size);

    if (status == GCOMP_OK) {
      printf("  Strategy '%s': %zu bytes -> %zu bytes\n", strategies[i],
          strlen(text_data), output_size);
    }

    gcomp_options_destroy(opts);
  }

  printf("  Strategy selection:\n");
  printf("    - 'default': Best for general data\n");
  printf("    - 'filtered': Optimized for pre-filtered data (PNG)\n");
  printf("    - 'huffman_only': Skip LZ77 for pre-compressed data\n");
  printf("    - 'rle': Run-length encoding for repeated bytes\n");
}

int main(void) {
  printf("=== Ghoti.io Compress Library - Gzip Options Example ===\n");
  printf("Library version: %s\n", gcomp_version_string());

  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "Error: Failed to get registry\n");
    return 1;
  }

  example_compression_levels(registry);
  example_header_metadata(registry);
  example_extra_field(registry);
  example_safety_limits(registry);
  example_concatenated_streams(registry);
  example_strategies(registry);

  printf("\n=== Examples Complete ===\n");
  return 0;
}
