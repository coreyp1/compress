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
 * @file rle.h
 *
 * RLE (Run-Length Encoding) method for the Ghoti.io Compress library.
 *
 * Stream interpretation is profile-driven. Reference profiles: packbits
 * (TIFF / Apple MacPaint) and tga (Truevision Targa). Use option
 * rle.format to select; default is "packbits". Shared limit options
 * apply (limits.max_output_bytes, limits.max_memory_bytes,
 * limits.max_expansion_ratio).
 *
 * See documentation/modules/rle.md for full API and options.
 */

#ifndef GHOTI_IO_GCOMP_RLE_H
#define GHOTI_IO_GCOMP_RLE_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The largest expansion either RLE profile permits, output over input.
 *
 * PackBits (TIFF 6.0 section 9) writes a run as a one-byte control and one
 * byte of data, standing for at most 128 repetitions: 128 / 2 = 64.  The
 * Targa profile is byte-oriented here too, with the same 128-byte cap on a
 * run, so it is bounded identically.
 *
 * Measured: 32 MiB of zeros compresses to exactly 64:1, which is the bound
 * itself - a run-length encoder on uniform input reaches its own ceiling.
 */
#define GCOMP_RLE_MAX_EXPANSION_RATIO 64ULL

/**
 * @brief Register the RLE method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_rle_register(gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_RLE_H
