/**
 * @file chunked_streaming.c
 *
 * Example demonstrating chunked streaming compression/decompression with the
 * Ghoti.io Compress library.
 *
 * This example shows how to:
 * - Create an encoder/decoder using the streaming API
 * - Process data in chunks using gcomp_encoder_update()/gcomp_decoder_update()
 * - Finalize streams using gcomp_encoder_finish()/gcomp_decoder_finish()
 * - Handle partial input/output progress
 *
 * This approach is useful when:
 * - Processing large files that don't fit in memory
 * - Streaming data over a network
 * - Working with data of unknown size
 *
 * Build: See Makefile target "examples"
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/stream.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Simulated input data - in real applications this might come from a file,
// network socket, or other streaming source
static const char * INPUT_DATA =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
    "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris "
    "nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in "
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
    "culpa qui officia deserunt mollit anim id est laborum. "
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
    "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris "
    "nisi ut aliquip ex ea commodo consequat.";

// Chunk sizes for demonstration - small to show multiple iterations
#define INPUT_CHUNK_SIZE 64   // Read input in 64-byte chunks
#define OUTPUT_CHUNK_SIZE 128 // Write output in 128-byte chunks

/**
 * @brief Demonstrate chunked encoding (compression)
 *
 * @param registry The method registry
 * @param input_data Pointer to input data
 * @param input_size Size of input data
 * @param output_buffer Pre-allocated buffer for output (must be large enough)
 * @param output_capacity Capacity of output buffer
 * @param output_size_out Receives actual compressed size
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t compress_chunked(gcomp_registry_t * registry,
    const uint8_t * input_data, size_t input_size, uint8_t * output_buffer,
    size_t output_capacity, size_t * output_size_out) {
  gcomp_status_t status;
  gcomp_encoder_t * encoder = NULL;
  gcomp_options_t * options = NULL;

  printf("\n--- Chunked Compression ---\n");
  printf("Input size: %zu bytes\n", input_size);
  printf("Processing in %d-byte input chunks, %d-byte output chunks\n",
      INPUT_CHUNK_SIZE, OUTPUT_CHUNK_SIZE);

  // Create options with compression level 6 (default, good balance)
  status = gcomp_options_create(&options);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Failed to create options: %s\n",
        gcomp_status_to_string(status));
    return status;
  }

  status = gcomp_options_set_int64(options, "deflate.level", 6);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Failed to set compression level: %s\n",
        gcomp_status_to_string(status));
    gcomp_options_destroy(options);
    return status;
  }

  // Create the encoder
  status = gcomp_encoder_create(registry, "deflate", options, &encoder);
  gcomp_options_destroy(options); // Options can be destroyed after encoder
                                  // creation
  if (status != GCOMP_OK) {
    fprintf(stderr, "Failed to create encoder: %s\n",
        gcomp_status_to_string(status));
    return status;
  }

  // Process input in chunks
  size_t total_input_consumed = 0;
  size_t total_output_produced = 0;
  int chunk_count = 0;

  while (total_input_consumed < input_size) {
    // Determine how much input to provide this iteration
    size_t remaining_input = input_size - total_input_consumed;
    size_t this_input_size = (remaining_input < INPUT_CHUNK_SIZE)
        ? remaining_input
        : INPUT_CHUNK_SIZE;

    // Set up input buffer (const data pointer, size, used counter)
    gcomp_buffer_t input_buf = {
        .data = input_data + total_input_consumed,
        .size = this_input_size,
        .used = 0,
    };

    // Set up output buffer (writable portion of our output buffer)
    gcomp_buffer_t output_buf = {
        .data = output_buffer + total_output_produced,
        .size = (output_capacity - total_output_produced < OUTPUT_CHUNK_SIZE)
            ? output_capacity - total_output_produced
            : OUTPUT_CHUNK_SIZE,
        .used = 0,
    };

    // Call update - this may consume some/all input and produce some output
    status = gcomp_encoder_update(encoder, &input_buf, &output_buf);
    if (status != GCOMP_OK) {
      fprintf(stderr, "Encoder update failed: %s\n",
          gcomp_status_to_string(status));
      gcomp_encoder_destroy(encoder);
      return status;
    }

    // Track progress
    total_input_consumed += input_buf.used;
    total_output_produced += output_buf.used;
    chunk_count++;

    printf("  Chunk %d: consumed %zu bytes, produced %zu bytes\n", chunk_count,
        input_buf.used, output_buf.used);

    // If no progress was made, we need a larger output buffer
    if (input_buf.used == 0 && output_buf.used == 0) {
      fprintf(stderr, "No progress made - output buffer may be too small\n");
      gcomp_encoder_destroy(encoder);
      return GCOMP_ERR_LIMIT;
    }
  }

  // Finish encoding - flushes any remaining data and writes final block
  // Note: finish() needs enough output space to complete in one call.
  // If GCOMP_ERR_LIMIT is returned, the output buffer was too small.
  printf("  Finishing stream...\n");
  {
    size_t remaining_capacity = output_capacity - total_output_produced;
    gcomp_buffer_t output_buf = {
        .data = output_buffer + total_output_produced,
        .size = remaining_capacity,
        .used = 0,
    };

    status = gcomp_encoder_finish(encoder, &output_buf);
    total_output_produced += output_buf.used;

    if (status == GCOMP_OK) {
      printf("  Finish completed, produced %zu bytes\n", output_buf.used);
    }
    else if (status == GCOMP_ERR_LIMIT) {
      fprintf(stderr,
          "Encoder finish failed: output buffer too small (had %zu bytes "
          "remaining)\n",
          remaining_capacity);
      gcomp_encoder_destroy(encoder);
      return status;
    }
    else {
      fprintf(stderr, "Encoder finish failed: %s\n",
          gcomp_status_to_string(status));
      gcomp_encoder_destroy(encoder);
      return status;
    }
  }

  printf("Compression complete: %zu bytes -> %zu bytes (%.1f%%)\n", input_size,
      total_output_produced, (100.0 * total_output_produced) / input_size);

  gcomp_encoder_destroy(encoder);
  *output_size_out = total_output_produced;
  return GCOMP_OK;
}

/**
 * @brief Demonstrate chunked decoding (decompression)
 *
 * @param registry The method registry
 * @param compressed_data Pointer to compressed data
 * @param compressed_size Size of compressed data
 * @param output_buffer Pre-allocated buffer for output
 * @param output_capacity Capacity of output buffer
 * @param output_size_out Receives actual decompressed size
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t decompress_chunked(gcomp_registry_t * registry,
    const uint8_t * compressed_data, size_t compressed_size,
    uint8_t * output_buffer, size_t output_capacity, size_t * output_size_out) {
  gcomp_status_t status;
  gcomp_decoder_t * decoder = NULL;

  printf("\n--- Chunked Decompression ---\n");
  printf("Compressed size: %zu bytes\n", compressed_size);
  printf("Processing in %d-byte input chunks, %d-byte output chunks\n",
      INPUT_CHUNK_SIZE, OUTPUT_CHUNK_SIZE);

  // Create the decoder (no options needed for defaults)
  status = gcomp_decoder_create(registry, "deflate", NULL, &decoder);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Failed to create decoder: %s\n",
        gcomp_status_to_string(status));
    return status;
  }

  // Process compressed data in chunks
  size_t total_input_consumed = 0;
  size_t total_output_produced = 0;
  int chunk_count = 0;

  while (total_input_consumed < compressed_size) {
    // Determine how much input to provide this iteration
    size_t remaining_input = compressed_size - total_input_consumed;
    size_t this_input_size = (remaining_input < INPUT_CHUNK_SIZE)
        ? remaining_input
        : INPUT_CHUNK_SIZE;

    gcomp_buffer_t input_buf = {
        .data = compressed_data + total_input_consumed,
        .size = this_input_size,
        .used = 0,
    };

    gcomp_buffer_t output_buf = {
        .data = output_buffer + total_output_produced,
        .size = (output_capacity - total_output_produced < OUTPUT_CHUNK_SIZE)
            ? output_capacity - total_output_produced
            : OUTPUT_CHUNK_SIZE,
        .used = 0,
    };

    status = gcomp_decoder_update(decoder, &input_buf, &output_buf);
    if (status != GCOMP_OK) {
      fprintf(stderr, "Decoder update failed: %s\n",
          gcomp_status_to_string(status));
      gcomp_decoder_destroy(decoder);
      return status;
    }

    total_input_consumed += input_buf.used;
    total_output_produced += output_buf.used;
    chunk_count++;

    printf("  Chunk %d: consumed %zu bytes, produced %zu bytes\n", chunk_count,
        input_buf.used, output_buf.used);
  }

  // Drain any remaining buffered output from the decoder
  // The decoder may have decoded more data than fits in our small output chunks
  printf("  Draining buffered output...\n");
  while (1) {
    gcomp_buffer_t empty_input = {.data = NULL, .size = 0, .used = 0};
    gcomp_buffer_t output_buf = {
        .data = output_buffer + total_output_produced,
        .size = (output_capacity - total_output_produced < OUTPUT_CHUNK_SIZE)
            ? output_capacity - total_output_produced
            : OUTPUT_CHUNK_SIZE,
        .used = 0,
    };

    status = gcomp_decoder_update(decoder, &empty_input, &output_buf);
    if (status != GCOMP_OK) {
      fprintf(
          stderr, "Decoder drain failed: %s\n", gcomp_status_to_string(status));
      gcomp_decoder_destroy(decoder);
      return status;
    }

    if (output_buf.used == 0) {
      // No more buffered output
      break;
    }

    total_output_produced += output_buf.used;
    printf("  Drain: produced %zu bytes\n", output_buf.used);
  }

  // Finish decoding
  // Note: finish() validates the stream is complete. For deflate, this ensures
  // the final block was received.
  printf("  Finishing stream...\n");
  {
    size_t remaining_capacity = output_capacity - total_output_produced;
    gcomp_buffer_t output_buf = {
        .data = output_buffer + total_output_produced,
        .size = remaining_capacity,
        .used = 0,
    };

    status = gcomp_decoder_finish(decoder, &output_buf);
    total_output_produced += output_buf.used;

    if (status == GCOMP_OK) {
      printf("  Finish completed, produced %zu bytes\n", output_buf.used);
    }
    else if (status == GCOMP_ERR_LIMIT) {
      fprintf(stderr,
          "Decoder finish failed: output buffer too small (had %zu bytes "
          "remaining)\n",
          remaining_capacity);
      gcomp_decoder_destroy(decoder);
      return status;
    }
    else {
      fprintf(stderr, "Decoder finish failed: %s\n",
          gcomp_status_to_string(status));
      gcomp_decoder_destroy(decoder);
      return status;
    }
  }

  printf("Decompression complete: %zu bytes -> %zu bytes\n", compressed_size,
      total_output_produced);

  gcomp_decoder_destroy(decoder);
  *output_size_out = total_output_produced;
  return GCOMP_OK;
}

int main(void) {
  gcomp_status_t status;

  printf("=== Ghoti.io Compress Library - Chunked Streaming Example ===\n\n");
  printf("Library version: %s\n", gcomp_version_string());

  // Set up registry
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

  // Prepare input data
  size_t input_size = strlen(INPUT_DATA);
  printf("Input text (%zu bytes):\n", input_size);
  printf("  \"%.60s...\"\n", INPUT_DATA);

  // Allocate buffers for compressed and decompressed data
  size_t compressed_capacity = input_size + 256;
  uint8_t * compressed = (uint8_t *)malloc(compressed_capacity);

  size_t decompressed_capacity = input_size + 1; // +1 for null terminator
  uint8_t * decompressed = (uint8_t *)malloc(decompressed_capacity);

  if (!compressed || !decompressed) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    free(compressed);
    free(decompressed);
    return 1;
  }

  // Compress
  size_t compressed_size = 0;
  status = compress_chunked(registry, (const uint8_t *)INPUT_DATA, input_size,
      compressed, compressed_capacity, &compressed_size);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Compression failed\n");
    free(compressed);
    free(decompressed);
    return 1;
  }

  // Decompress
  size_t decompressed_size = 0;
  status = decompress_chunked(registry, compressed, compressed_size,
      decompressed, decompressed_capacity, &decompressed_size);
  if (status != GCOMP_OK) {
    fprintf(stderr, "Decompression failed\n");
    free(compressed);
    free(decompressed);
    return 1;
  }

  // Verify
  printf("\n--- Verification ---\n");
  if (decompressed_size == input_size &&
      memcmp(INPUT_DATA, decompressed, input_size) == 0) {
    printf("SUCCESS: Round-trip streaming compression verified!\n");
    printf("Original: %zu bytes -> Compressed: %zu bytes -> Decompressed: %zu "
           "bytes\n",
        input_size, compressed_size, decompressed_size);
  }
  else {
    printf("FAILURE: Decompressed data doesn't match original!\n");
    printf(
        "Expected %zu bytes, got %zu bytes\n", input_size, decompressed_size);
    free(compressed);
    free(decompressed);
    return 1;
  }

  // Cleanup
  free(compressed);
  free(decompressed);

  return 0;
}
