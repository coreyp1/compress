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
 * @file lzw_core.h
 *
 * LZW core: dictionary (prefix_code, append_char), decode stack, KwKwK
 * handling, dictionary reset, max dictionary size. Used by encoder and
 * decoder; profile drives code-width and CLEAR/EOI semantics.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_LZW_LZW_CORE_H
#define GHOTI_IO_GCOMP_SRC_METHODS_LZW_LZW_CORE_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/** Maximum supported code width (bits); max table size 4096. */
#define LZW_CORE_MAX_CODE_BITS 12

/** Maximum number of codes (2^LZW_CORE_MAX_CODE_BITS). */
#define LZW_CORE_MAX_CODES (1u << LZW_CORE_MAX_CODE_BITS)

/**
 * @brief LZW decoder core state.
 *
 * Holds the string table (prefix_code, append_char), decode stack, and
 * state for KwKwK. Allocated and initialized by lzw_core_decoder_init();
 * lzw_core_decoder_reset() re-initializes without reallocating.
 */
typedef struct lzw_core_decoder_s {
  const gcomp_allocator_t * allocator;
  uint16_t * prefix_code; ///< Prefix code for each table entry [0..capacity-1].
  uint8_t * append_char;  ///< Append byte for each table entry.
  uint8_t * stack;        ///< Decode stack (reverse output); length capacity.
  uint32_t capacity;      ///< Max codes (e.g. 4096).
  /** First dictionary code: one above EOI, so CLEAR and EOI sit below it.
   * Everything under this is a literal root.  It is 258 only when a literal
   * is eight bits wide; a four-colour GIF clears at 4 and starts here at 6. */
  uint32_t first_code;
  uint32_t next_code;     ///< Next code to assign.
  uint32_t prev_code;     ///< Previous code decoded (for KwKwK).
  uint8_t prev_first_byte; ///< First byte of previous string (for KwKwK).
  int has_prev;            ///< 1 if prev_code is valid.
} lzw_core_decoder_t;

/**
 * @brief LZW encoder core state.
 *
 * Same string table layout as decoder; encoder finds or adds (prefix, byte).
 */
typedef struct lzw_core_encoder_s {
  const gcomp_allocator_t * allocator;
  uint16_t * prefix_code;
  uint8_t * append_char;
  uint32_t capacity;
  uint32_t first_code; ///< First dictionary code; see the decoder's.
  uint32_t next_code;
} lzw_core_encoder_t;

/**
 * @brief Initialize the decoder core.
 *
 * Allocates prefix_code, append_char, and stack. Table is reset to
 * literals only; next_code is set to first_sequence_code (e.g. 258 for
 * CLEAR=256, EOI=257).
 *
 * @param core Decoder core (must not be NULL).
 * @param allocator Registry allocator (must not be NULL).
 * @param max_code_bits Maximum code width in bits (e.g. 12).
 * @param clear_code CLEAR code value (e.g. 256).
 * @param eoi_code EOI code value (e.g. 257).
 * @return GCOMP_OK on success, GCOMP_ERR_MEMORY on allocation failure,
 *         GCOMP_ERR_INVALID_ARG if max_code_bits > LZW_CORE_MAX_CODE_BITS.
 */
GCOMP_INTERNAL_API gcomp_status_t lzw_core_decoder_init(lzw_core_decoder_t * core,
    const gcomp_allocator_t * allocator, unsigned max_code_bits,
    uint32_t clear_code, uint32_t eoi_code);

/**
 * @brief Reset decoder table to initial state (literals only).
 *
 * Retains allocated buffers. next_code set to first_sequence_code.
 */
GCOMP_INTERNAL_API void lzw_core_decoder_reset(
    lzw_core_decoder_t * core, uint32_t clear_code, uint32_t eoi_code);

/**
 * @brief Free decoder core buffers (caller frees the struct if needed).
 */
GCOMP_INTERNAL_API void lzw_core_decoder_destroy(lzw_core_decoder_t * core);

/**
 * @brief Decode one code and append its string to output.
 *
 * Handles KwKwK: when code == next_code, emits prev_string + first_byte(prev).
 * Caller must not pass clear_code or eoi_code; handle CLEAR/EOI before calling.
 *
 * @param core Decoder core.
 * @param code Code to decode (must be < next_code or == next_code for KwKwK).
 * @param output_data Output buffer.
 * @param output_size Output buffer capacity.
 * @param output_used Current used count; updated on success.
 * @return GCOMP_OK on success, GCOMP_ERR_CORRUPT if code > next_code,
 *         GCOMP_ERR_LIMIT if output would exceed output_size.
 */
GCOMP_INTERNAL_API gcomp_status_t lzw_core_decoder_decode(lzw_core_decoder_t * core, uint32_t code,
    uint8_t * output_data, size_t output_size, size_t * output_used);

/**
 * @brief Initialize the encoder core.
 *
 * Allocates string table. Table starts with literals only; next_code is
 * first_sequence_code (e.g. 258).
 */
GCOMP_INTERNAL_API gcomp_status_t lzw_core_encoder_init(lzw_core_encoder_t * core,
    const gcomp_allocator_t * allocator, unsigned max_code_bits,
    uint32_t clear_code, uint32_t eoi_code);

/**
 * @brief Reset encoder table to initial state.
 */
GCOMP_INTERNAL_API void lzw_core_encoder_reset(
    lzw_core_encoder_t * core, uint32_t clear_code, uint32_t eoi_code);

/**
 * @brief Free encoder core buffers.
 */
GCOMP_INTERNAL_API void lzw_core_encoder_destroy(lzw_core_encoder_t * core);

/**
 * @brief Find code for (prefix_code, append_byte), or 0 if not found.
 *
 * Returns 0 when the pair is not in the table (caller can use 0 to mean
 * "not found" only if 0 is not a valid code for a pair; in LZW, 0 is a
 * literal so valid). So we need a different convention: return the code
 * (1..capacity-1) or 0 for "not found". So we use 0 as "not found" and
 * valid codes are 1..capacity-1. But literal 0 is code 0. So we cannot
 * use 0 for not found. Use a separate "found" out-parameter:
 * lzw_core_encoder_find(core, prefix, byte, &code_out) -> 1 if found, 0 if not;
 * code_out set when found.
 */
GCOMP_INTERNAL_API int lzw_core_encoder_find(const lzw_core_encoder_t * core, uint32_t prefix,
    uint8_t byte, uint32_t * code_out);

/**
 * @brief Add (prefix_code, append_byte) to the table; return new code.
 *
 * Call only when table is not full (next_code < capacity). Returns the
 * code that was assigned.
 */
GCOMP_INTERNAL_API uint32_t lzw_core_encoder_add(
    lzw_core_encoder_t * core, uint32_t prefix, uint8_t byte);

/**
 * @brief Check if the encoder table is full (next_code >= capacity).
 */
GCOMP_INTERNAL_API int lzw_core_encoder_is_full(const lzw_core_encoder_t * core);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_LZW_LZW_CORE_H
