/**
 * @file bitreader.h
 *
 * Bit reader utilities for the DEFLATE (RFC 1951) method.
 *
 * These helpers provide a simple interface for reading bits from a byte
 * stream in LSB-first order, as required by DEFLATE. They support reading
 * up to 24 bits at a time, byte alignment, and robust EOF handling.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_DEFLATE_BITREADER_H
#define GCOMP_DEFLATE_BITREADER_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DEFLATE bit reader state.
 *
 * Initialize with ::gcomp_deflate_bitreader_init() before use.
 */
typedef struct gcomp_deflate_bitreader_s {
  const uint8_t * data; ///< Pointer to input bytes (not owned).
  size_t size;          ///< Total size of @ref data in bytes.
  size_t byte_pos;      ///< Current byte position in @ref data.
  uint32_t bit_buffer;  ///< Buffered bits (LSB-first).
  uint32_t bit_count;   ///< Number of valid bits in @ref bit_buffer.
} gcomp_deflate_bitreader_t;

/**
 * @brief Initialize a DEFLATE bit reader over a byte buffer.
 *
 * @param reader Bit reader to initialize (must not be NULL).
 * @param data   Input buffer (can be NULL if @p size is 0).
 * @param size   Size of @p data in bytes.
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_bitreader_init(
    gcomp_deflate_bitreader_t * reader, const uint8_t * data, size_t size);

/**
 * @brief Read @p num_bits bits from the stream (LSB-first).
 *
 * Bits are returned in the least-significant bits of @p out. For example,
 * if the next 3 bits in the stream are 0b101, then @p out will be 0b000...0101.
 *
 * @param reader   Bit reader (must not be NULL).
 * @param num_bits Number of bits to read (1..24).
 * @param out      Output value pointer (must not be NULL).
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG on bad parameters,
 *         ::GCOMP_ERR_CORRUPT if there are not enough bits remaining.
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_bitreader_read_bits(
    gcomp_deflate_bitreader_t * reader, uint32_t num_bits, uint32_t * out);

/**
 * @brief Align the reader to the next byte boundary.
 *
 * Discards any remaining bits up to the next multiple of 8 from the stream.
 * After this call, the next read will start at a byte boundary relative to
 * the original input buffer.
 *
 * @param reader Bit reader (must not be NULL).
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if @p reader is NULL.
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_bitreader_align_to_byte(
    gcomp_deflate_bitreader_t * reader);

/**
 * @brief Query whether the reader has reached the end of the input.
 *
 * This returns true when there are no more whole bytes available and there
 * are no buffered bits remaining.
 *
 * @param reader Bit reader (must not be NULL).
 * @return 1 if at end of stream, 0 otherwise.
 */
GCOMP_INTERNAL_API int gcomp_deflate_bitreader_is_eof(
    const gcomp_deflate_bitreader_t * reader);

#ifdef __cplusplus
}
#endif

#endif // GCOMP_DEFLATE_BITREADER_H
