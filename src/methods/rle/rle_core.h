/**
 * @file rle_core.h
 *
 * RLE core primitives: literal span and repeat span emission with
 * output bounds checking. Used by profile-driven encoder/decoder.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_RLE_CORE_H
#define GHOTI_IO_GCOMP_RLE_CORE_H

#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Emit a literal span (raw bytes unchanged).
 *
 * @param output Output buffer (data, size, used)
 * @param data Source bytes
 * @param len Number of bytes to emit
 * @param max_output_bytes Maximum allowed output (0 = unlimited)
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT if would exceed max_output_bytes
 */
gcomp_status_t rle_emit_literal(uint8_t * output_data, size_t output_size,
    size_t * output_used, const uint8_t * data, size_t len,
    uint64_t max_output_bytes);

/**
 * @brief Emit a repeat span (N copies of a single byte).
 *
 * @param output Output buffer (data, size, used)
 * @param byte Byte value to repeat
 * @param count Number of copies
 * @param max_output_bytes Maximum allowed output (0 = unlimited)
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT if would exceed max_output_bytes
 */
gcomp_status_t rle_emit_repeat(uint8_t * output_data, size_t output_size,
    size_t * output_used, uint8_t byte, size_t count,
    uint64_t max_output_bytes);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_RLE_CORE_H
