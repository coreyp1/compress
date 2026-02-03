/**
 * @file endian.h
 *
 * Endian conversion utilities for the Ghoti.io Compress library.
 *
 * This header provides portable little-endian read/write functions used
 * by compression methods that serialize multi-byte integers (gzip, LZ4).
 *
 * These functions use explicit byte manipulation to ensure correct behavior
 * regardless of the host system's native byte order. They are implemented
 * as inline functions for optimal performance.
 *
 * **Caller contract:** No bounds checking is performed inside these helpers.
 * The caller must ensure the buffer has at least N bytes available before
 * calling the N-byte read/write function (e.g. 2 for gcomp_read_le16, 4 for
 * gcomp_read_le32, 8 for gcomp_read_le64, and the corresponding write
 * functions). Violating this contract causes out-of-bounds access.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ENDIAN_H
#define GHOTI_IO_GCOMP_ENDIAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read a 16-bit little-endian value from a byte buffer.
 *
 * @param buf Pointer to at least 2 bytes of data
 * @return The 16-bit value in host byte order
 */
static inline uint16_t gcomp_read_le16(const uint8_t * buf) {
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/**
 * @brief Read a 32-bit little-endian value from a byte buffer.
 *
 * @param buf Pointer to at least 4 bytes of data
 * @return The 32-bit value in host byte order
 */
static inline uint32_t gcomp_read_le32(const uint8_t * buf) {
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
      ((uint32_t)buf[3] << 24);
}

/**
 * @brief Read a 64-bit little-endian value from a byte buffer.
 *
 * @param buf Pointer to at least 8 bytes of data
 * @return The 64-bit value in host byte order
 */
static inline uint64_t gcomp_read_le64(const uint8_t * buf) {
  return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) |
      ((uint64_t)buf[3] << 24) | ((uint64_t)buf[4] << 32) |
      ((uint64_t)buf[5] << 40) | ((uint64_t)buf[6] << 48) |
      ((uint64_t)buf[7] << 56);
}

/**
 * @brief Write a 16-bit value to a buffer in little-endian order.
 *
 * @param buf Pointer to at least 2 bytes of writable memory
 * @param val The 16-bit value to write
 */
static inline void gcomp_write_le16(uint8_t * buf, uint16_t val) {
  buf[0] = (uint8_t)(val);
  buf[1] = (uint8_t)(val >> 8);
}

/**
 * @brief Write a 32-bit value to a buffer in little-endian order.
 *
 * @param buf Pointer to at least 4 bytes of writable memory
 * @param val The 32-bit value to write
 */
static inline void gcomp_write_le32(uint8_t * buf, uint32_t val) {
  buf[0] = (uint8_t)(val);
  buf[1] = (uint8_t)(val >> 8);
  buf[2] = (uint8_t)(val >> 16);
  buf[3] = (uint8_t)(val >> 24);
}

/**
 * @brief Write a 64-bit value to a buffer in little-endian order.
 *
 * @param buf Pointer to at least 8 bytes of writable memory
 * @param val The 64-bit value to write
 */
static inline void gcomp_write_le64(uint8_t * buf, uint64_t val) {
  buf[0] = (uint8_t)(val);
  buf[1] = (uint8_t)(val >> 8);
  buf[2] = (uint8_t)(val >> 16);
  buf[3] = (uint8_t)(val >> 24);
  buf[4] = (uint8_t)(val >> 32);
  buf[5] = (uint8_t)(val >> 40);
  buf[6] = (uint8_t)(val >> 48);
  buf[7] = (uint8_t)(val >> 56);
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ENDIAN_H
