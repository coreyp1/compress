/**
 * @file bitwriter.h
 *
 * Bit writer utilities for the DEFLATE (RFC 1951) method.
 *
 * These helpers provide a simple interface for writing bits to a byte
 * stream in LSB-first order, as required by DEFLATE. They support writing
 * up to 24 bits at a time and flushing to a byte boundary.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_DEFLATE_BITWRITER_H
#define GCOMP_DEFLATE_BITWRITER_H

#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DEFLATE bit writer state.
 *
 * Initialize with ::gcomp_deflate_bitwriter_init() before use.
 */
typedef struct gcomp_deflate_bitwriter_s {
  uint8_t * data;      /**< Output buffer (not owned). */
  size_t size;         /**< Capacity of @ref data in bytes. */
  size_t byte_pos;     /**< Current byte position in @ref data. */
  uint32_t bit_buffer; /**< Buffered bits (LSB-first). */
  uint32_t bit_count;  /**< Number of valid bits in @ref bit_buffer. */
} gcomp_deflate_bitwriter_t;

/**
 * @brief Initialize a DEFLATE bit writer over a byte buffer.
 *
 * @param writer Bit writer to initialize (must not be NULL).
 * @param data   Output buffer (can be NULL if @p size is 0).
 * @param size   Capacity of @p data in bytes.
 */
gcomp_status_t gcomp_deflate_bitwriter_init(
    gcomp_deflate_bitwriter_t * writer, uint8_t * data, size_t size);

/**
 * @brief Write @p num_bits from @p bits to the stream (LSB-first).
 *
 * The least-significant @p num_bits of @p bits are written first.
 *
 * @param writer   Bit writer (must not be NULL).
 * @param bits     Bits to write (only lower @p num_bits bits are used).
 * @param num_bits Number of bits to write (1..24).
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG on bad parameters,
 *         ::GCOMP_ERR_LIMIT if the output buffer is too small.
 */
gcomp_status_t gcomp_deflate_bitwriter_write_bits(
    gcomp_deflate_bitwriter_t * writer, uint32_t bits, uint32_t num_bits);

/**
 * @brief Flush any buffered bits to the next byte boundary.
 *
 * If there are remaining bits in the buffer (not a multiple of 8), this
 * writes one final byte with the remaining bits in the low bits and zeros
 * in the high bits.
 *
 * @param writer Bit writer (must not be NULL).
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG on bad parameters,
 *         ::GCOMP_ERR_LIMIT if the output buffer is too small.
 */
gcomp_status_t gcomp_deflate_bitwriter_flush_to_byte(
    gcomp_deflate_bitwriter_t * writer);

/**
 * @brief Get the number of whole bytes written to the output buffer.
 *
 * This does not include any partially filled byte that has not yet been
 * flushed via ::gcomp_deflate_bitwriter_flush_to_byte().
 *
 * @param writer Bit writer (must not be NULL).
 * @return Number of bytes written so far.
 */
size_t gcomp_deflate_bitwriter_bytes_written(
    const gcomp_deflate_bitwriter_t * writer);

/**
 * @brief Update the output buffer pointer without resetting bit state.
 *
 * Call this to continue writing to a new buffer segment while preserving
 * any partial bits from the previous segment. The byte_pos is reset to 0.
 *
 * @param writer Bit writer (must not be NULL).
 * @param data   New output buffer (can be NULL if @p size is 0).
 * @param size   Capacity of @p data in bytes.
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if writer is NULL.
 */
gcomp_status_t gcomp_deflate_bitwriter_set_buffer(
    gcomp_deflate_bitwriter_t * writer, uint8_t * data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // GCOMP_DEFLATE_BITWRITER_H
