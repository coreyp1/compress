/**
 * @file bitreader.c
 *
 * Bit reader utilities for the DEFLATE (RFC 1951) method.
 *
 * Provides LSB-first bit reading from a byte stream with support for byte
 * alignment and robust EOF handling. This module is internal to the DEFLATE
 * implementation but is exposed for testing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "bitreader.h"

gcomp_status_t gcomp_deflate_bitreader_init(
    gcomp_deflate_bitreader_t * reader, const uint8_t * data, size_t size) {
  if (!reader) {
    return GCOMP_ERR_INVALID_ARG;
  }

  reader->data = data;
  reader->size = size;
  reader->byte_pos = 0;
  reader->bit_buffer = 0;
  reader->bit_count = 0;

  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_bitreader_read_bits(
    gcomp_deflate_bitreader_t * reader, uint32_t num_bits, uint32_t * out) {
  uint32_t mask;

  if (!reader || !out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (num_bits == 0 || num_bits > 24) {
    // Limit to 24 bits to keep mask construction safe and sufficient for
    // DEFLATE needs (code lengths, distance/length codes, etc.).
    return GCOMP_ERR_INVALID_ARG;
  }

  // Fill buffer until we have enough bits or run out of input.
  while (reader->bit_count < num_bits) {
    if (reader->byte_pos >= reader->size) {
      // Not enough bits remaining to satisfy the request.
      return GCOMP_ERR_CORRUPT;
    }

    reader->bit_buffer |= ((uint32_t)reader->data[reader->byte_pos])
        << reader->bit_count;
    reader->bit_count += 8;
    reader->byte_pos++;
  }

  mask = (num_bits == 32) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);
  *out = reader->bit_buffer & mask;

  reader->bit_buffer >>= num_bits;
  reader->bit_count -= num_bits;

  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_bitreader_align_to_byte(
    gcomp_deflate_bitreader_t * reader) {
  uint32_t skip;

  if (!reader) {
    return GCOMP_ERR_INVALID_ARG;
  }

  skip = reader->bit_count % 8u;
  if (skip != 0u) {
    reader->bit_buffer >>= skip;
    reader->bit_count -= skip;
  }

  return GCOMP_OK;
}

int gcomp_deflate_bitreader_is_eof(const gcomp_deflate_bitreader_t * reader) {
  if (!reader) {
    return 1;
  }

  return (reader->byte_pos >= reader->size && reader->bit_count == 0u) ? 1 : 0;
}
