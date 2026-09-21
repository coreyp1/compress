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
 * @file rle_core.h
 *
 * RLE core primitives: literal span and repeat span emission with
 * output bounds checking.
 *
 * The core is format-agnostic: it only writes raw bytes (literal or repeated)
 * and enforces max_output_bytes and buffer size using safe math. Token
 * parsing and grammar (PackBits vs TGA) live in rle_profile; the decoder
 * profile calls these primitives after interpreting each token.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_RLE_RLE_CORE_H
#define GHOTI_IO_GCOMP_SRC_METHODS_RLE_RLE_CORE_H

#include <ghoti.io/compress/macros.h>

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
GCOMP_INTERNAL_API gcomp_status_t rle_emit_literal(uint8_t * output_data, size_t output_size,
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
GCOMP_INTERNAL_API gcomp_status_t rle_emit_repeat(uint8_t * output_data, size_t output_size,
    size_t * output_used, uint8_t byte, size_t count,
    uint64_t max_output_bytes);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_RLE_RLE_CORE_H
