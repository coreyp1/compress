/**
 * @file lzw_internal.h
 *
 * Internal declarations for the LZW (Lempel-Ziv-Welch) method.
 * Shared between encoder, decoder, core, profile, and bit I/O.
 *
 * DESIGN
 * ------
 * LZW is profile-driven: bit order (LSB vs MSB), CLEAR/EOI codes, and
 * code-width growth rules vary by format (GIF, TIFF). Core maintains
 * dictionary (prefix_code, append_char), decode stack, and KwKwK handling.
 * Encoder/decoder use profile for bit packing and code semantics.
 *
 * STREAMING CONTRACT (WHAT PERSISTS BETWEEN update() CALLS)
 * --------------------------------------------------------
 *
 * LZW packs variable-width codes at the bit level. That means calls may end
 * mid-byte. To support arbitrary chunking without corrupting the stream, the
 * method state retains:
 *
 * - **Encoder (`lzw_encoder_state_t`)**
 *   - `writer.bit_buffer` / `writer.bit_count`: pending (not yet byte-flushed)
 *     bits that must carry into the next output window.
 *   - `prefix_code`: the current “in-progress” string code (the last code is
 *     emitted during `finish()`).
 *   - `core` and `current_bits`: dictionary and current code width.
 *
 * - **Decoder (`lzw_decoder_state_t`)**
 *   - `reader.bit_buffer` / `reader.bit_count`: pending bits from the last
 *     partially consumed byte; required to resume decoding at the correct bit
 *     offset when more input arrives.
 *   - `pending_buf`/`pending_off`/`pending_len`: decoded bytes that could not
 *     be copied to the caller’s output buffer yet (output backpressure).
 *   - `core` and `current_bits`: dictionary and current code width.
 *
 * In other words: `input->used`/`output->used` progress is **byte-oriented**,
 * but LZW’s internal progress is **bit-oriented** and is tracked by the
 * bitreader/bitwriter state in the method.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LZW_INTERNAL_H
#define GHOTI_IO_GCOMP_LZW_INTERNAL_H

#include "../../core/stream_internal.h"
#include "lzw_bitio.h"
#include "lzw_core.h"
#include "lzw_hash.h"
#include "lzw_profile.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// LZW format names (option lzw.format)
//

#define LZW_FORMAT_GIF "gif"
#define LZW_FORMAT_TIFF "tiff"

//
// Encoder state (LW2: core, bit writer, profile, current string, pending write)
//

typedef struct {
  const gcomp_allocator_t * allocator;
  char format[16];
  uint64_t lit_width;
  uint64_t max_code_bits;
  uint64_t max_output_bytes;
  uint64_t max_memory_bytes;
  uint64_t max_expansion_ratio;

  gcomp_memory_tracker_t mem_tracker;

  lzw_core_encoder_t core;
  lzw_bitwriter_t writer;
  lzw_profile_id_t profile_id;
  uint32_t clear_code;
  uint32_t eoi_code;
  unsigned current_bits;
  /** Current string code (0..255 literal, or 258+); UINT32_MAX = no prefix yet
   */
  uint32_t prefix_code;
  /** Code we tried to write but hit LIMIT; retry next update (0 = none) */
  uint32_t pending_code;
  unsigned pending_bits;
  /** 1 after we've emitted CLEAR at start */
  int header_emitted;
  /** Encoder lookup: 1 = hash, 0 = linear */
  int use_hash;
  /** Hash table for (prefix, byte) → code; NULL if use_hash is 0 */
  lzw_encoder_hash_t * hash_table;
  /** Bytes tracked for hash_table (for destroy and limit) */
  size_t hash_table_bytes;
} lzw_encoder_state_t;

//
// Decoder state (LW2: core, bit reader, profile, pending output, limits)
//

typedef struct {
  const gcomp_allocator_t * allocator;
  char format[16];
  uint64_t lit_width;
  uint64_t max_code_bits;
  uint64_t max_output_bytes;
  uint64_t max_memory_bytes;
  uint64_t max_expansion_ratio;
  uint64_t total_input_bytes;
  uint64_t total_output_bytes;

  gcomp_memory_tracker_t mem_tracker;

  lzw_core_decoder_t core;
  lzw_bitreader_t reader;
  lzw_profile_id_t profile_id;
  uint32_t clear_code;
  uint32_t eoi_code;
  unsigned current_bits;
  /** Pending decoded bytes not yet copied to output */
  uint8_t * pending_buf;
  size_t pending_cap; ///< Capacity of pending_buf (1u << max_code_bits)
  size_t pending_off;
  size_t pending_len;
  /** 1 when EOI seen or stream ended */
  int done;
} lzw_decoder_state_t;

//
// Internal API: Encoder
//

gcomp_status_t lzw_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);

void lzw_encoder_destroy(gcomp_encoder_t * encoder);

gcomp_status_t lzw_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

gcomp_status_t lzw_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

gcomp_status_t lzw_encoder_reset(gcomp_encoder_t * encoder);

//
// Internal API: Decoder
//

gcomp_status_t lzw_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);

void lzw_decoder_destroy(gcomp_decoder_t * decoder);

gcomp_status_t lzw_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

gcomp_status_t lzw_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

gcomp_status_t lzw_decoder_reset(gcomp_decoder_t * decoder);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZW_INTERNAL_H
