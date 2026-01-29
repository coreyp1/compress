/**
 * @file bitwriter.c
 *
 * Bit writer utilities for the DEFLATE (RFC 1951) method.
 *
 * Provides LSB-first bit writing to a byte stream with support for flushing
 * to a byte boundary. This module is internal to the DEFLATE implementation
 * but is exposed for testing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "bitwriter.h"

gcomp_status_t gcomp_deflate_bitwriter_init(
    gcomp_deflate_bitwriter_t * writer, uint8_t * data, size_t size) {
  if (!writer) {
    return GCOMP_ERR_INVALID_ARG;
  }

  writer->data = data;
  writer->size = size;
  writer->byte_pos = 0;
  writer->bit_buffer = 0;
  writer->bit_count = 0;

  return GCOMP_OK;
}

static gcomp_status_t gcomp_deflate_bitwriter_flush_full_bytes(
    gcomp_deflate_bitwriter_t * writer) {
  while (writer->bit_count >= 8u) {
    if (writer->byte_pos >= writer->size) {
      return GCOMP_ERR_LIMIT;
    }

    writer->data[writer->byte_pos] = (uint8_t)(writer->bit_buffer & 0xFFu);
    writer->byte_pos++;

    writer->bit_buffer >>= 8u;
    writer->bit_count -= 8u;
  }

  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_bitwriter_write_bits(
    gcomp_deflate_bitwriter_t * writer, uint32_t bits, uint32_t num_bits) {
  gcomp_status_t status;

  if (!writer) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (num_bits == 0 || num_bits > 24) {
    // Limit to 24 bits to keep operations safe and sufficient for DEFLATE.
    return GCOMP_ERR_INVALID_ARG;
  }

  // Mask out only the requested number of bits.
  bits &= (num_bits == 32) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);

  writer->bit_buffer |= (bits << writer->bit_count);
  writer->bit_count += num_bits;

  status = gcomp_deflate_bitwriter_flush_full_bytes(writer);
  if (status != GCOMP_OK) {
    return status;
  }

  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_bitwriter_flush_to_byte(
    gcomp_deflate_bitwriter_t * writer) {
  gcomp_status_t status;

  if (!writer) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // First flush any complete bytes.
  status = gcomp_deflate_bitwriter_flush_full_bytes(writer);
  if (status != GCOMP_OK) {
    return status;
  }

  // If there are remaining bits, write one final byte.
  if (writer->bit_count > 0u) {
    if (writer->byte_pos >= writer->size) {
      return GCOMP_ERR_LIMIT;
    }

    writer->data[writer->byte_pos] = (uint8_t)(writer->bit_buffer & 0xFFu);
    writer->byte_pos++;

    writer->bit_buffer = 0;
    writer->bit_count = 0;
  }

  return GCOMP_OK;
}

size_t gcomp_deflate_bitwriter_bytes_written(
    const gcomp_deflate_bitwriter_t * writer) {
  if (!writer) {
    return 0;
  }

  return writer->byte_pos;
}
