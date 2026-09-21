/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file lzw_bitio.h
 *
 * LZW-specific bit reader/writer. LSB (GIF) or MSB (TIFF) per profile.
 * Variable code width (e.g. 9 to 12 bits). Used by encoder and decoder.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_LZW_LZW_BITIO_H
#define GHOTI_IO_GCOMP_SRC_METHODS_LZW_LZW_BITIO_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/** LSB-first (GIF) or MSB-first (TIFF) */
typedef enum {
  LZW_BITIO_LSB,
  LZW_BITIO_MSB,
} lzw_bitio_order_t;

/**
 * @brief LZW bit reader state.
 */
typedef struct lzw_bitreader_s {
  const uint8_t * data;
  size_t size;
  size_t byte_pos;
  lzw_bitio_order_t order;
  // LSB: bit_buffer holds low bits; MSB: bit_buffer holds high bits
  uint32_t bit_buffer;
  uint32_t bit_count;
} lzw_bitreader_t;

/**
 * @brief LZW bit writer state.
 */
typedef struct lzw_bitwriter_s {
  uint8_t * data;
  size_t size;
  size_t byte_pos;
  lzw_bitio_order_t order;
  uint32_t bit_buffer;
  uint32_t bit_count;
} lzw_bitwriter_t;

/**
 * @brief Initialize bit reader over input buffer.
 */
GCOMP_INTERNAL_API void lzw_bitreader_init(lzw_bitreader_t * reader, const uint8_t * data,
    size_t size, lzw_bitio_order_t order);

/**
 * @brief Read exactly num_bits (9--12) into *out (low bits).
 *
 * @return GCOMP_OK, GCOMP_ERR_INVALID_ARG, or GCOMP_ERR_CORRUPT if not enough
 * input.
 */
GCOMP_INTERNAL_API gcomp_status_t lzw_bitreader_read_bits(
    lzw_bitreader_t * reader, unsigned num_bits, uint32_t * out);

/**
 * @brief True when no more bits available.
 */
int lzw_bitreader_is_eof(const lzw_bitreader_t * reader);

/**
 * @brief Set new input buffer (e.g. for streaming); resets byte_pos only,
 * preserves order, bit_buffer, and bit_count.
 */
void lzw_bitreader_set_buffer(
    lzw_bitreader_t * reader, const uint8_t * data, size_t size);

/**
 * @brief Initialize bit writer over output buffer.
 */
GCOMP_INTERNAL_API void lzw_bitwriter_init(lzw_bitwriter_t * writer, uint8_t * data, size_t size,
    lzw_bitio_order_t order);

/**
 * @brief Write low num_bits of value (9--12).
 *
 * @return GCOMP_OK or GCOMP_ERR_LIMIT if buffer full.
 */
GCOMP_INTERNAL_API gcomp_status_t lzw_bitwriter_write_bits(
    lzw_bitwriter_t * writer, uint32_t value, unsigned num_bits);

/**
 * @brief Flush remaining bits to byte boundary (writes partial byte if any).
 */
GCOMP_INTERNAL_API gcomp_status_t lzw_bitwriter_flush(lzw_bitwriter_t * writer);

/**
 * @brief Number of whole bytes written (after flush, includes partial byte).
 */
GCOMP_INTERNAL_API size_t lzw_bitwriter_bytes_written(const lzw_bitwriter_t * writer);

/**
 * @brief Set new output buffer (e.g. for streaming); resets byte_pos only.
 */
void lzw_bitwriter_set_buffer(
    lzw_bitwriter_t * writer, uint8_t * data, size_t size);

/**
 * @brief True if the writer has bits buffered (not yet flushed to bytes).
 */
int lzw_bitwriter_has_pending_bits(const lzw_bitwriter_t * writer);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_LZW_LZW_BITIO_H
