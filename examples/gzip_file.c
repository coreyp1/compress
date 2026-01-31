/**
 * @file gzip_file.c
 *
 * Example demonstrating gzip file compression and decompression with the
 * Ghoti.io Compress library.
 *
 * This example shows how to:
 * - Compress a file to gzip format using the streaming API
 * - Set gzip metadata (filename, modification time)
 * - Decompress a gzip file
 * - Handle errors and verify data integrity
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
#include <time.h>

#define BUFFER_SIZE (64 * 1024) // 64 KB chunks

/**
 * @brief Compress a file to gzip format
 *
 * @param registry Method registry
 * @param input_path Path to input file
 * @param output_path Path to output .gz file
 * @return 0 on success, non-zero on error
 */
static int compress_file(gcomp_registry_t * registry, const char * input_path,
    const char * output_path) {
  FILE * in_file = NULL;
  FILE * out_file = NULL;
  gcomp_encoder_t * encoder = NULL;
  gcomp_options_t * options = NULL;
  uint8_t * in_buffer = NULL;
  uint8_t * out_buffer = NULL;
  int result = 1;
  gcomp_status_t status;

  printf("Compressing: %s -> %s\n", input_path, output_path);

  // Open files
  in_file = fopen(input_path, "rb");
  if (!in_file) {
    fprintf(stderr, "Error: Cannot open input file: %s\n", input_path);
    goto cleanup;
  }

  out_file = fopen(output_path, "wb");
  if (!out_file) {
    fprintf(stderr, "Error: Cannot create output file: %s\n", output_path);
    goto cleanup;
  }

  // Allocate buffers
  in_buffer = (uint8_t *)malloc(BUFFER_SIZE);
  out_buffer = (uint8_t *)malloc(BUFFER_SIZE);
  if (!in_buffer || !out_buffer) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    goto cleanup;
  }

  // Create options with gzip metadata
  status = gcomp_options_create(&options);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Error: Failed to create options: %s\n",
        gcomp_status_to_string(status));
    goto cleanup;
  }

  // Set original filename (extracted basename from path)
  const char * basename = strrchr(input_path, '/');
  if (!basename) {
    basename = strrchr(input_path, '\\'); // Windows path separator
  }
  basename = basename ? basename + 1 : input_path;
  gcomp_options_set_string(options, "gzip.name", basename);

  // Set modification time to current time
  gcomp_options_set_uint64(options, "gzip.mtime", (uint64_t)time(NULL));

  // Set compression level (6 = good balance)
  gcomp_options_set_int64(options, "deflate.level", 6);

  // Create encoder
  status = gcomp_encoder_create(registry, "gzip", options, &encoder);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Error: Failed to create encoder: %s\n",
        gcomp_status_to_string(status));
    goto cleanup;
  }

  // Process input file in chunks
  size_t total_in = 0;
  size_t total_out = 0;

  while (!feof(in_file)) {
    size_t bytes_read = fread(in_buffer, 1, BUFFER_SIZE, in_file);
    if (bytes_read == 0 && ferror(in_file)) {
      fprintf(stderr, "Error: Read error\n");
      goto cleanup;
    }

    total_in += bytes_read;

    // Compress this chunk
    gcomp_buffer_t in_buf = {in_buffer, bytes_read, 0};

    while (in_buf.used < in_buf.size) {
      gcomp_buffer_t out_buf = {out_buffer, BUFFER_SIZE, 0};

      status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        fprintf(stderr, "Error: Compression failed: %s\n",
            gcomp_status_to_string(status));
        goto cleanup;
      }

      // Write compressed output
      if (out_buf.used > 0) {
        if (fwrite(out_buffer, 1, out_buf.used, out_file) != out_buf.used) {
          fprintf(stderr, "Error: Write error\n");
          goto cleanup;
        }
        total_out += out_buf.used;
      }
    }
  }

  // Finish compression
  gcomp_buffer_t out_buf = {out_buffer, BUFFER_SIZE, 0};
  status = gcomp_encoder_finish(encoder, &out_buf);
  if (status != GCOMP_OK) {
    fprintf(
        stderr, "Error: Finish failed: %s\n", gcomp_status_to_string(status));
    goto cleanup;
  }

  if (out_buf.used > 0) {
    if (fwrite(out_buffer, 1, out_buf.used, out_file) != out_buf.used) {
      fprintf(stderr, "Error: Write error\n");
      goto cleanup;
    }
    total_out += out_buf.used;
  }

  printf("  Input:  %zu bytes\n", total_in);
  printf("  Output: %zu bytes (%.1f%%)\n", total_out,
      total_in > 0 ? (100.0 * total_out) / total_in : 0.0);

  result = 0;

cleanup:
  if (encoder)
    gcomp_encoder_destroy(encoder);
  if (options)
    gcomp_options_destroy(options);
  if (in_file)
    fclose(in_file);
  if (out_file)
    fclose(out_file);
  free(in_buffer);
  free(out_buffer);
  return result;
}

/**
 * @brief Decompress a gzip file
 *
 * @param registry Method registry
 * @param input_path Path to input .gz file
 * @param output_path Path to output file
 * @return 0 on success, non-zero on error
 */
static int decompress_file(gcomp_registry_t * registry, const char * input_path,
    const char * output_path) {
  FILE * in_file = NULL;
  FILE * out_file = NULL;
  gcomp_decoder_t * decoder = NULL;
  uint8_t * in_buffer = NULL;
  uint8_t * out_buffer = NULL;
  int result = 1;
  gcomp_status_t status;

  printf("Decompressing: %s -> %s\n", input_path, output_path);

  // Open files
  in_file = fopen(input_path, "rb");
  if (!in_file) {
    fprintf(stderr, "Error: Cannot open input file: %s\n", input_path);
    goto cleanup;
  }

  out_file = fopen(output_path, "wb");
  if (!out_file) {
    fprintf(stderr, "Error: Cannot create output file: %s\n", output_path);
    goto cleanup;
  }

  // Allocate buffers
  in_buffer = (uint8_t *)malloc(BUFFER_SIZE);
  out_buffer = (uint8_t *)malloc(BUFFER_SIZE);
  if (!in_buffer || !out_buffer) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    goto cleanup;
  }

  // Create decoder
  status = gcomp_decoder_create(registry, "gzip", NULL, &decoder);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Error: Failed to create decoder: %s\n",
        gcomp_status_to_string(status));
    goto cleanup;
  }

  // Process compressed file
  size_t total_in = 0;
  size_t total_out = 0;

  while (!feof(in_file)) {
    size_t bytes_read = fread(in_buffer, 1, BUFFER_SIZE, in_file);
    if (bytes_read == 0 && ferror(in_file)) {
      fprintf(stderr, "Error: Read error\n");
      goto cleanup;
    }

    if (bytes_read == 0)
      break;

    total_in += bytes_read;

    // Decompress this chunk
    gcomp_buffer_t in_buf = {in_buffer, bytes_read, 0};

    while (in_buf.used < in_buf.size) {
      gcomp_buffer_t out_buf = {out_buffer, BUFFER_SIZE, 0};

      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        fprintf(stderr, "Error: Decompression failed: %s\n",
            gcomp_status_to_string(status));
        fprintf(
            stderr, "Detail: %s\n", gcomp_decoder_get_error_detail(decoder));
        goto cleanup;
      }

      // Write decompressed output
      if (out_buf.used > 0) {
        if (fwrite(out_buffer, 1, out_buf.used, out_file) != out_buf.used) {
          fprintf(stderr, "Error: Write error\n");
          goto cleanup;
        }
        total_out += out_buf.used;
      }

      // If no progress, break inner loop
      if (in_buf.used == 0 && out_buf.used == 0)
        break;
    }
  }

  // Finish decompression (validates CRC32 and ISIZE)
  gcomp_buffer_t out_buf = {out_buffer, BUFFER_SIZE, 0};
  status = gcomp_decoder_finish(decoder, &out_buf);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Error: Verification failed: %s\n",
        gcomp_status_to_string(status));
    fprintf(stderr, "Detail: %s\n", gcomp_decoder_get_error_detail(decoder));
    goto cleanup;
  }

  if (out_buf.used > 0) {
    if (fwrite(out_buffer, 1, out_buf.used, out_file) != out_buf.used) {
      fprintf(stderr, "Error: Write error\n");
      goto cleanup;
    }
    total_out += out_buf.used;
  }

  printf("  Input:  %zu bytes\n", total_in);
  printf("  Output: %zu bytes\n", total_out);

  result = 0;

cleanup:
  if (decoder)
    gcomp_decoder_destroy(decoder);
  if (in_file)
    fclose(in_file);
  if (out_file)
    fclose(out_file);
  free(in_buffer);
  free(out_buffer);
  return result;
}

int main(int argc, char ** argv) {
  printf("=== Ghoti.io Compress Library - Gzip File Example ===\n\n");
  printf("Library version: %s\n\n", gcomp_version_string());

  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "Error: Failed to get registry\n");
    return 1;
  }

  // If no arguments, demonstrate with in-memory data
  if (argc < 3) {
    printf(
        "Usage: %s <input-file> <output-file.gz>      (compress)\n", argv[0]);
    printf("       %s -d <input-file.gz> <output-file>   (decompress)\n\n",
        argv[0]);
    printf("Running demo with in-memory data...\n\n");

    // Demo: compress and decompress in memory
    const char * test_data =
        "This is a test of gzip compression using the Ghoti.io Compress "
        "library. The gzip format adds a header with metadata and a trailer "
        "with CRC32 checksum for data integrity verification.";
    size_t test_len = strlen(test_data);

    // Compress
    uint8_t compressed[1024];
    size_t compressed_size = 0;
    gcomp_status_t status = gcomp_encode_buffer(registry, "gzip", NULL,
        test_data, test_len, compressed, sizeof(compressed), &compressed_size);

    if (status != GCOMP_OK) {
      fprintf(
          stderr, "Compression failed: %s\n", gcomp_status_to_string(status));
      return 1;
    }

    printf("Compressed %zu bytes -> %zu bytes (%.1f%%)\n", test_len,
        compressed_size, (100.0 * compressed_size) / test_len);

    // Decompress
    char decompressed[1024];
    size_t decompressed_size = 0;
    status =
        gcomp_decode_buffer(registry, "gzip", NULL, compressed, compressed_size,
            decompressed, sizeof(decompressed) - 1, &decompressed_size);

    if (status != GCOMP_OK) {
      fprintf(
          stderr, "Decompression failed: %s\n", gcomp_status_to_string(status));
      return 1;
    }

    decompressed[decompressed_size] = '\0';

    // Verify
    if (decompressed_size == test_len &&
        memcmp(test_data, decompressed, test_len) == 0) {
      printf("SUCCESS: Round-trip verified!\n");
    }
    else {
      printf("FAILURE: Data mismatch!\n");
      return 1;
    }

    return 0;
  }

  // Handle command-line arguments
  if (argc == 3) {
    // Compress mode
    return compress_file(registry, argv[1], argv[2]);
  }
  else if (argc == 4 && strcmp(argv[1], "-d") == 0) {
    // Decompress mode
    return decompress_file(registry, argv[2], argv[3]);
  }
  else {
    fprintf(stderr, "Invalid arguments\n");
    return 1;
  }
}
