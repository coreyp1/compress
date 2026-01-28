/**
 * @file crc32.h
 *
 * CRC32 computation for the Ghoti.io Compress library.
 *
 * This module provides CRC32 computation compatible with RFC 1952 (gzip
 * format). The CRC32 algorithm uses the IEEE 802.3 polynomial (0xEDB88320).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_CRC32_H
#define GHOTI_IO_GCOMP_CRC32_H

#include <ghoti.io/compress/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute CRC32 for a buffer
 *
 * Computes the CRC32 checksum for the given buffer. This is compatible
 * with RFC 1952 (gzip format) and uses the IEEE 802.3 polynomial.
 *
 * @param data Pointer to the data buffer
 * @param len Length of the data buffer in bytes
 * @return The CRC32 checksum value
 *
 * @note This function uses standard CRC32 (RFC 1952) with initial value
 *       GCOMP_CRC32_INIT (0xFFFFFFFF) and returns the unfinalized result.
 *       For incremental computation, use GCOMP_CRC32_INIT and
 *       gcomp_crc32_update(). Call gcomp_crc32_finalize() if you need the
 *       finalized value for file formats (e.g., RFC 1952 gzip format
 *       requires finalized CRC32).
 *
 * Example:
 * @code
 * const uint8_t data[] = {0x01, 0x02, 0x03};
 * uint32_t crc = gcomp_crc32(data, sizeof(data));
 * @endcode
 */
GCOMP_API uint32_t gcomp_crc32(const uint8_t * data, size_t len);

/**
 * @brief Standard CRC32 initial value
 *
 * Standard CRC32 (RFC 1952) uses initial value 0xFFFFFFFF. This is the
 * standard initialization value used in gzip, PNG, and ZIP file formats.
 *
 * Example:
 * @code
 * uint32_t crc = GCOMP_CRC32_INIT;
 * crc = gcomp_crc32_update(crc, data1, len1);
 * crc = gcomp_crc32_update(crc, data2, len2);
 * crc = gcomp_crc32_finalize(crc);
 * @endcode
 */
#define GCOMP_CRC32_INIT 0xFFFFFFFFU

/**
 * @brief Update CRC32 computation with more data
 *
 * Updates the CRC32 computation with additional data. This allows for
 * incremental computation of CRC32 across multiple buffers.
 *
 * @param crc Current CRC32 value (from init or previous update)
 * @param data Pointer to the data buffer
 * @param len Length of the data buffer in bytes
 * @return Updated CRC32 value
 *
 * Example:
 * @code
 * uint32_t crc = GCOMP_CRC32_INIT;
 * crc = gcomp_crc32_update(crc, chunk1, len1);
 * crc = gcomp_crc32_update(crc, chunk2, len2);
 * crc = gcomp_crc32_finalize(crc);
 * @endcode
 */
GCOMP_API uint32_t gcomp_crc32_update(
    uint32_t crc, const uint8_t * data, size_t len);

/**
 * @brief Finalize CRC32 computation
 *
 * Finalizes the CRC32 computation by XORing with 0xFFFFFFFF. This is
 * required to match the RFC 1952 format.
 *
 * @param crc Current CRC32 value
 * @return Finalized CRC32 value
 *
 * Example:
 * @code
 * uint32_t crc = GCOMP_CRC32_INIT;
 * crc = gcomp_crc32_update(crc, data, len);
 * crc = gcomp_crc32_finalize(crc);
 * @endcode
 */
GCOMP_API uint32_t gcomp_crc32_finalize(uint32_t crc);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_CRC32_H */
