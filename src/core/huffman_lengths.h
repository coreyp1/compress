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
 * @file huffman_lengths.h
 *
 * Minimum-redundancy Huffman code lengths under a maximum length.
 *
 * Every format in this library that writes a Huffman code caps the code
 * length: DEFLATE at 15 bits for the literal/length and distance alphabets
 * and 7 for the code-length alphabet (RFC 1951 section 3.2.7), Zstandard at
 * 11 bits for literals (RFC 8878 section 4.2.1).  A plain Huffman tree
 * ignores that cap, so the lengths it produces have to be brought under it -
 * and the way that is done decides how many bits the block costs.
 *
 * Internal only - not part of the public API.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_HUFFMAN_LENGTHS_H
#define GHOTI_IO_GCOMP_SRC_CORE_HUFFMAN_LENGTHS_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute optimal code lengths for @p freq, none longer than
 *        @p max_bits.
 *
 * The result minimises sum(freq[i] * length[i]) over every prefix-free code
 * whose longest code word is at most @p max_bits bits, which is what
 * "optimal" means here: no other assignment of lengths within the cap encodes
 * these frequencies in fewer bits.  Symbols with a zero frequency get length
 * zero and take no part in the code.
 *
 * The lengths always satisfy the Kraft equality - sum(2^-length) == 1 - so
 * the canonical code built from them assigns every available code word.  Both
 * formats require that: DEFLATE defines the code by the construction in RFC
 * 1951 section 3.2.2, and an incomplete set of lengths does not describe one.
 *
 * @param alloc Allocator for scratch space; NULL uses the default.
 * @param freq Frequency of each symbol; @p num_symbols entries.
 * @param num_symbols Size of the alphabet.
 * @param max_bits Longest code word allowed, 1..32.
 * @param lengths_out Receives one length per symbol; @p num_symbols entries.
 * @return GCOMP_OK; GCOMP_ERR_INVALID_ARG when @p max_bits cannot represent
 *         the number of symbols in use, or an argument is NULL;
 *         GCOMP_ERR_MEMORY when scratch space cannot be allocated.
 */
gcomp_status_t gcomp_huffman_code_lengths(const gcomp_allocator_t * alloc,
    const uint32_t * freq, size_t num_symbols, unsigned max_bits,
    uint8_t * lengths_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_HUFFMAN_LENGTHS_H
