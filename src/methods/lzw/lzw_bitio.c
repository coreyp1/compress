/**
 * @file lzw_bitio.c
 *
 * LZW bit I/O: LSB-first (GIF) and MSB-first (TIFF), variable width 9 to 12
 * bits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lzw_bitio.h"

#define LZW_BITIO_MAX_BITS 12

void lzw_bitreader_init(lzw_bitreader_t * reader, const uint8_t * data,
    size_t size, lzw_bitio_order_t order) {
  if (!reader) {
    return;
  }
  reader->data = data;
  reader->size = size;
  reader->byte_pos = 0;
  reader->order = order;
  reader->bit_buffer = 0;
  reader->bit_count = 0;
}

// LSB: same as DEFLATE - low bits first in stream, value in low bits of *out
static gcomp_status_t read_lsb(
    lzw_bitreader_t * reader, unsigned num_bits, uint32_t * out) {
  while (reader->bit_count < num_bits) {
    if (reader->byte_pos >= reader->size) {
      return GCOMP_ERR_CORRUPT;
    }
    reader->bit_buffer |= (uint32_t)(reader->data[reader->byte_pos])
        << reader->bit_count;
    reader->bit_count += 8;
    reader->byte_pos++;
  }
  uint32_t mask = (1u << num_bits) - 1u;
  *out = reader->bit_buffer & mask;
  reader->bit_buffer >>= num_bits;
  reader->bit_count -= num_bits;
  return GCOMP_OK;
}

// MSB: first bit of stream is MSB of first byte; bit_count is bit offset in
// current byte (0..7)
static gcomp_status_t read_msb(
    lzw_bitreader_t * reader, unsigned num_bits, uint32_t * out) {
  uint32_t value = 0;
  size_t byte_pos = reader->byte_pos;
  unsigned bit_offset = reader->bit_count;

  for (unsigned i = 0; i < num_bits; i++) {
    if (byte_pos >= reader->size) {
      return GCOMP_ERR_CORRUPT;
    }
    unsigned bit = (reader->data[byte_pos] >> (7u - bit_offset)) & 1u;
    value = (value << 1) | bit;
    bit_offset++;
    if (bit_offset == 8u) {
      bit_offset = 0;
      byte_pos++;
    }
  }
  reader->byte_pos = byte_pos;
  reader->bit_count = bit_offset;
  *out = value;
  return GCOMP_OK;
}

gcomp_status_t lzw_bitreader_read_bits(
    lzw_bitreader_t * reader, unsigned num_bits, uint32_t * out) {
  if (!reader || !out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (num_bits < 9 || num_bits > LZW_BITIO_MAX_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (reader->order == LZW_BITIO_LSB) {
    return read_lsb(reader, num_bits, out);
  }
  return read_msb(reader, num_bits, out);
}

int lzw_bitreader_is_eof(const lzw_bitreader_t * reader) {
  if (!reader) {
    return 1;
  }
  if (reader->order == LZW_BITIO_LSB) {
    return (reader->byte_pos >= reader->size && reader->bit_count == 0) ? 1 : 0;
  }
  return (reader->byte_pos >= reader->size) ? 1 : 0;
}

void lzw_bitreader_set_buffer(
    lzw_bitreader_t * reader, const uint8_t * data, size_t size) {
  if (!reader) {
    return;
  }
  reader->data = data;
  reader->size = size;
  reader->byte_pos = 0;
}

void lzw_bitwriter_init(lzw_bitwriter_t * writer, uint8_t * data, size_t size,
    lzw_bitio_order_t order) {
  if (!writer) {
    return;
  }
  writer->data = data;
  writer->size = size;
  writer->byte_pos = 0;
  writer->order = order;
  writer->bit_buffer = 0;
  writer->bit_count = 0;
}

// LSB: add low num_bits of value to buffer; flush full bytes.
// On GCOMP_ERR_LIMIT, leaves writer state unchanged (rollback) so caller can
// retry after providing more buffer.
static gcomp_status_t write_lsb(
    lzw_bitwriter_t * writer, uint32_t value, unsigned num_bits) {
  uint32_t mask = (1u << num_bits) - 1u;
  value &= mask;
  uint32_t saved_buffer = writer->bit_buffer;
  uint32_t saved_count = writer->bit_count;
  writer->bit_buffer |= value << writer->bit_count;
  writer->bit_count += num_bits;
  while (writer->bit_count >= 8u) {
    if (writer->byte_pos >= writer->size) {
      writer->bit_buffer = saved_buffer;
      writer->bit_count = saved_count;
      return GCOMP_ERR_LIMIT;
    }
    writer->data[writer->byte_pos++] = (uint8_t)(writer->bit_buffer & 0xFFu);
    writer->bit_buffer >>= 8u;
    writer->bit_count -= 8u;
  }
  return GCOMP_OK;
}

// MSB: add num_bits to high part of buffer; flush full bytes (high byte first).
// On GCOMP_ERR_LIMIT, rolls back writer state so caller can retry.
static gcomp_status_t write_msb(
    lzw_bitwriter_t * writer, uint32_t value, unsigned num_bits) {
  uint32_t mask = (1u << num_bits) - 1u;
  value &= mask;
  uint32_t saved_buffer = writer->bit_buffer;
  uint32_t saved_count = writer->bit_count;
  writer->bit_buffer = (writer->bit_buffer << num_bits) | value;
  writer->bit_count += num_bits;
  while (writer->bit_count >= 8u) {
    if (writer->byte_pos >= writer->size) {
      writer->bit_buffer = saved_buffer;
      writer->bit_count = saved_count;
      return GCOMP_ERR_LIMIT;
    }
    writer->data[writer->byte_pos++] =
        (uint8_t)((writer->bit_buffer >> (writer->bit_count - 8)) & 0xFFu);
    writer->bit_count -= 8u;
    writer->bit_buffer &= (1u << writer->bit_count) - 1u;
  }
  return GCOMP_OK;
}

gcomp_status_t lzw_bitwriter_write_bits(
    lzw_bitwriter_t * writer, uint32_t value, unsigned num_bits) {
  if (!writer) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (num_bits < 9 || num_bits > LZW_BITIO_MAX_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (writer->order == LZW_BITIO_LSB) {
    return write_lsb(writer, value, num_bits);
  }
  return write_msb(writer, value, num_bits);
}

gcomp_status_t lzw_bitwriter_flush(lzw_bitwriter_t * writer) {
  if (!writer) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (writer->bit_count == 0) {
    return GCOMP_OK;
  }
  if (writer->byte_pos >= writer->size) {
    return GCOMP_ERR_LIMIT;
  }
  if (writer->order == LZW_BITIO_LSB) {
    writer->data[writer->byte_pos++] = (uint8_t)(writer->bit_buffer & 0xFFu);
  }
  else {
    writer->data[writer->byte_pos++] =
        (uint8_t)((writer->bit_buffer << (8 - writer->bit_count)) & 0xFFu);
  }
  writer->bit_buffer = 0;
  writer->bit_count = 0;
  return GCOMP_OK;
}

int lzw_bitwriter_has_pending_bits(const lzw_bitwriter_t * writer) {
  return writer && writer->bit_count != 0;
}

size_t lzw_bitwriter_bytes_written(const lzw_bitwriter_t * writer) {
  return writer ? writer->byte_pos : 0;
}

void lzw_bitwriter_set_buffer(
    lzw_bitwriter_t * writer, uint8_t * data, size_t size) {
  if (!writer) {
    return;
  }
  writer->data = data;
  writer->size = size;
  writer->byte_pos = 0;
}