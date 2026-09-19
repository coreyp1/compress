/**
 * @file lzw_bitio.c
 *
 * LZW bit I/O: LSB-first (GIF) and MSB-first (TIFF), variable width 9 to 12
 * bits.
 *
 * STREAMING NOTES
 * ===============
 *
 * Encoder/decoder `update()` calls may be given output/input buffers of any
 * size. To support that:
 *
 * - `lzw_bitwriter_set_buffer()` swaps the output window but preserves pending
 *   bits (`bit_buffer`/`bit_count`) so partial bytes can continue into the next
 *   window.
 * - `lzw_bitreader_set_buffer()` swaps the input window but preserves pending
 *   bits so the reader can resume mid-byte across calls.
 *
 * Writer LIMIT behavior:
 * - `lzw_bitwriter_write_bits()` is allowed to fail with `GCOMP_ERR_LIMIT` when
 *   the output window is full. When it does, it rolls back `bit_buffer` and
 *   `bit_count` so the caller can retry with a fresh output buffer.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
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
      // Out of input part-way through a code.  Whatever was read is already
      // in bit_buffer and byte_pos says those bytes were taken, so the next
      // window continues the same code.  See lzw_bitreader_read_bits().
      return GCOMP_ERR_LIMIT;
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
      // Out of input part-way through a code.  Nothing is committed -- the
      // reader's byte_pos and bit_count are left where they were -- so the
      // bytes this code started in are not consumed and are read again, from
      // the same bit offset, when the caller brings more.  See
      // lzw_bitreader_read_bits().
      return GCOMP_ERR_LIMIT;
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

/**
 * @brief Read one code.
 *
 * RUNNING OUT OF INPUT IS NOT CORRUPTION
 * ======================================
 *
 * Codes are 9 to 12 bits and do not stop on byte boundaries, so a window of
 * input almost always ends part-way through one.  For a streaming decoder
 * that is the ordinary case -- the rest of the code is in the bytes that have
 * not arrived yet -- and it is reported as GCOMP_ERR_LIMIT, meaning "come
 * back with more input", not GCOMP_ERR_CORRUPT.
 *
 * It used to be reported as corruption, and the decoder treated it as fatal.
 * The effect was that the LZW decoder could not accept a stream in pieces at
 * all: 5,000 bytes encoded to 557 and then handed over 64 bytes at a time
 * failed on the first call, at offset 0.  Only a whole stream in one buffer
 * ever worked.  A genuinely truncated stream is still caught, by
 * lzw_decoder_finish(), which reports a stream that ended without EOI.
 *
 * @return GCOMP_OK when a code was read, GCOMP_ERR_LIMIT when the window ran
 *         out first, GCOMP_ERR_INVALID_ARG for a width this format cannot
 *         have.
 */
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
  // byte_pos is part of the rollback. A single write can flush more than one
  // byte, so the limit may be hit after some have already landed; leaving
  // byte_pos advanced while bit_buffer/bit_count rewind makes the retry
  // re-emit those bytes, inserting duplicates into the stream.
  size_t saved_pos = writer->byte_pos;
  writer->bit_buffer |= value << writer->bit_count;
  writer->bit_count += num_bits;
  while (writer->bit_count >= 8u) {
    if (writer->byte_pos >= writer->size) {
      writer->bit_buffer = saved_buffer;
      writer->bit_count = saved_count;
      writer->byte_pos = saved_pos;
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
  // byte_pos is part of the rollback. A single write can flush more than one
  // byte, so the limit may be hit after some have already landed; leaving
  // byte_pos advanced while bit_buffer/bit_count rewind makes the retry
  // re-emit those bytes, inserting duplicates into the stream.
  size_t saved_pos = writer->byte_pos;
  writer->bit_buffer = (writer->bit_buffer << num_bits) | value;
  writer->bit_count += num_bits;
  while (writer->bit_count >= 8u) {
    if (writer->byte_pos >= writer->size) {
      writer->bit_buffer = saved_buffer;
      writer->bit_count = saved_count;
      writer->byte_pos = saved_pos;
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