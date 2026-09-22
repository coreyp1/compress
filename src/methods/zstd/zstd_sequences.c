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
 * @file zstd_sequences.c
 *
 * Sequences section encoder and decoder for the Ghoti.io Compress library.
 *
 * ## Overview
 *
 * The sequences section is the heart of zstd compression. It describes how to
 * reconstruct the original data by interleaving literal copies with match
 * copies (back-references to previously decoded data).
 *
 * ## Sequence Structure
 *
 * Each sequence contains three values:
 * - **Literal Length (LL)**: Number of bytes to copy from literals buffer
 * - **Match Offset (OF)**: Distance back in output to start copying
 * - **Match Length (ML)**: Number of bytes to copy from that position
 *
 * Special offset values 1, 2, 3 indicate "repeat offsets" - reusing recent
 * offsets for better compression on repetitive data.
 *
 * ## Section Layout
 *
 * ```
 * ┌─────────────────────────────────────────────────────────────┐
 * │ Number of sequences (1-3 bytes, variable encoding)          │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Compression modes byte (2 bits each for LL, OF, ML)         │
 * ├─────────────────────────────────────────────────────────────┤
 * │ [LL FSE table] (if mode == FSE)                             │
 * │ [OF FSE table] (if mode == FSE)                             │
 * │ [ML FSE table] (if mode == FSE)                             │
 * ├─────────────────────────────────────────────────────────────┤
 * │ FSE-encoded bitstream (sequences in reverse order)          │
 * └─────────────────────────────────────────────────────────────┘
 * ```
 *
 * ## FSE Bitstream Structure (Backward Bitstream)
 *
 * The bitstream is written and read BACKWARD - the first bytes written end up
 * at the END of the buffer, and reading starts from the end working toward
 * the beginning. A marker bit (highest '1' bit in last byte) indicates where
 * valid data starts.
 *
 * ### Encoding Order (encoder writes, low addresses → high addresses):
 *
 * For sequences 0, 1, 2, ..., N-1 (processing in reverse: N-1 first):
 *
 * 1. **Sequence N-1 (last)**: Extra bits only (LL, ML, OF order)
 * 2. **Sequences N-2 ... 0**: For each:
 *    - State update bits (OF, ML, LL order)
 *    - Extra bits (LL, ML, OF order)
 * 3. **Initial states**: ML state, OF state, LL state
 * 4. **Marker bit**: Single '1' bit at the end
 *
 * ### Decoding Order (decoder reads, from marker toward beginning):
 *
 * 1. Find marker bit (highest '1' in last byte)
 * 2. Read initial states: LL (6 bits), OF (5 bits), ML (6 bits)
 * 3. For each sequence 0 ... N-1:
 *    - Look up symbols from FSE states
 *    - Read extra bits: OF_extra, ML_extra, LL_extra
 *    - If not last sequence: read state updates (LL, ML, OF order)
 *
 * ## Symbol Codes and Extra Bits
 *
 * Each symbol type uses a code + extra bits scheme to represent values:
 *
 * - **Literal Length**: 36 codes (0-35), up to 16 extra bits
 * - **Match Length**: 53 codes (0-52), up to 16 extra bits, base offset 3
 * - **Offset**: Code = floor(log2(offset)), extra = offset - 2^code
 *
 * ## Repeat Offsets
 *
 * Offset codes 1, 2, 3 are special:
 * - Code 1: Use previous match offset (rep_offset_1)
 * - Code 2: Use second-previous offset (rep_offset_2)
 * - Code 3: Use third-previous offset (rep_offset_3)
 *
 * When literal_length == 0, the codes are shifted:
 * - Code 1 → rep_offset_2
 * - Code 2 → rep_offset_3
 * - Code 3 → rep_offset_1 - 1
 *
 * Reference: RFC 8878 (https://www.rfc-editor.org/rfc/rfc8878.html)
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "../../core/fastcopy.h"
#include "zstd_internal.h"
#include "zstd_sequences_private.h"
#include <stdlib.h>
#include <string.h>

//
// Sequences Constants
//

#define SEQ_MODE_PREDEFINED 0 ///< Use predefined FSE table
#define SEQ_MODE_RLE 1        ///< Single symbol repeated
#define SEQ_MODE_FSE 2        ///< FSE-compressed table
#define SEQ_MODE_REPEAT 3     ///< Use previous FSE table

#define SEQ_MAX_LITERAL_LENGTH 131071 ///< Maximum literal length (17 bits)
#define SEQ_MAX_MATCH_LENGTH 131074   ///< Maximum match length (17 bits + 3)
#define SEQ_MAX_OFFSET 0x7FFFFFFF     ///< Maximum offset (31 bits)

// FSE table sizes (max accuracy log = 9, so max table size = 512)
#define FSE_LL_TABLE_SIZE 512 ///< Literal length max table size
#define FSE_ML_TABLE_SIZE 512 ///< Match length max table size
#define FSE_OF_TABLE_SIZE 256 ///< Offset max table size (max log 8)

//
// Baseline and Extra Bits Tables (shared with zstd_sequences_encode.c)
//

const uint32_t zstd_seq_ll_baseline[ZSTD_SEQ_LL_CODES] = {0, 1, 2, 3, 4, 5, 6,
    7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64,
    128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t zstd_seq_ll_extra_bits[ZSTD_SEQ_LL_CODES] = {0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16};

const uint32_t zstd_seq_ml_baseline[ZSTD_SEQ_ML_CODES] = {3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
    29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131,
    259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t zstd_seq_ml_extra_bits[ZSTD_SEQ_ML_CODES] = {0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

//
// Sequence Decoder State
//

typedef struct {
  uint16_t ll_state; ///< Literal length FSE state
  uint16_t ml_state; ///< Match length FSE state
  uint16_t of_state; ///< Offset FSE state
} zstd_seq_state_t;

//
// Bit Reader for Sequences (backward bitstream)
//
// FSE bitstreams in zstd are written in reverse order.  The last byte contains
// a marker bit (the highest set bit) followed by padding zeros.  Reading runs
// from the marker downward toward bit 0.
//
// HOW THIS READS, AND WHY IT CHANGED
// ==================================
//
// The obvious way to write this is to keep a count of bits consumed, and for
// each read work out the absolute bit position wanted, find the eight bytes
// containing it, and extract.  That is what this did, and it meant every read
// -- six per sequence, three FSE states and three sets of extra bits --
// recomputed where it was in the stream from scratch.  Even after the loads
// were cached and the arithmetic reduced to a single subtraction, it was the
// single most expensive function in a Zstandard decode.
//
// It now works the way a bitstream reader is normally built.  The container
// holds sixty-four bits of the stream, `used` counts how many of them have
// been taken starting from the top, and a read is one shift and one mask with
// nothing to look up.  Consuming bits costs an addition.  Only when whole
// bytes have been used up does the window slide down over them, and that is
// one 64-bit load covering the next fifty-six bits -- so the stream is read in
// gulps rather than a field at a time.
//
// THE MAPPING
// ===========
//
// Container bit i is stream bit (byte_pos * 8 + i - bit_offset), so the bits
// waiting to be read sit immediately below bit (64 - used).  The invariant
// tying the two counters together is
//
//   byte_pos * 8 + 64 - used  ==  total_bits - consumed + bit_offset
//
// `bit_offset` is zero for any stream of eight bytes or more.  A shorter one
// is copied into `pad` right-aligned, so that the same 64-bit load works
// without reading past the end of the caller's buffer, and `bit_offset`
// counts the zero bits that padding put in front of it.
//

typedef struct {
  /// Sixty-four bits of the stream; see the mapping above.
  uint64_t container;
  /**
   * Where the container was loaded from.
   *
   * This points into @ref pad for a stream shorter than eight bytes, so the
   * reader is not copyable.  It is a local of zstd_sequences_execute() and is
   * only ever used through a pointer.
   */
  const uint8_t * base;
  size_t base_size;       ///< Bytes of @ref base; at least 8.
  size_t byte_pos;        ///< container == gcomp_read_le64(base + byte_pos).
  unsigned used;          ///< Bits of the container taken, from bit 63 down.
  unsigned bit_offset;    ///< Padding bits in front of a short stream.
  unsigned total_bits;    ///< Data bits in the stream, below the marker.
  /// Data bits read so far; at the end it must be @ref total_bits exactly.
  unsigned bits_consumed;
  /**
   * The largest `used + nb_bits` a read may reach; see the note below.
   *
   * `64 - bit_offset`, fixed for the life of the reader.
   */
  unsigned limit;
  uint8_t pad[8]; ///< A stream shorter than eight bytes, right-aligned.
} zstd_seq_bit_reader_t;

//
// ONE BOUND, NOT TWO
// ==================
//
// A read has two ways to fail: the stream may not hold that many bits, and
// the container may not.  Those were two tests, and they are the same test.
//
// The invariant above says
//
//   byte_pos * 8 + 64 - used  ==  total_bits - bits_consumed + bit_offset
//
// so "the stream holds nb_bits more", which is
// `bits_consumed + nb_bits <= total_bits`, rearranges to
//
//   used + nb_bits  <=  byte_pos * 8 + 64 - bit_offset
//
// and "the container holds them" is `used + nb_bits <= 64`.  The right-hand
// sides differ by `byte_pos * 8`, and `bit_offset` is non-zero only for a
// stream shorter than eight bytes -- which is copied into `pad` and read
// from there, so its `byte_pos` is zero and never moves.  Whenever
// `byte_pos` is positive, therefore, `bit_offset` is zero and the stream
// bound is the looser of the two by at least eight bits; whenever
// `byte_pos` is zero the two coincide.
//
// So `64 - bit_offset` bounds both, for the whole life of the reader, and a
// read tests `used + nb_bits` against that one number.  Exceeding it means
// either "slide the window" or "the stream is out", and which of those it
// was is exactly whether the slide moved: refill, and test again.

/**
 * @brief Slide the container down over the bits that come next.
 *
 * Whole bytes that have been consumed are dropped and the window moves back
 * towards the start of the stream, which is the direction this bitstream is
 * read in.  One 64-bit load refills fifty-six usable bits, so this does real
 * work about once every seven reads rather than on every one.
 *
 * At the start of the stream the window cannot move any further; `used` then
 * grows past eight, which is correct -- the container already holds every bit
 * that is left.
 */
static inline void zstd_seq_bit_reader_refill(zstd_seq_bit_reader_t * br) {
  size_t shift = br->used >> 3;
  if (shift == 0u) {
    return;
  }
  if (shift > br->byte_pos) {
    shift = br->byte_pos;
    if (shift == 0u) {
      return;
    }
  }
  br->byte_pos -= shift;
  br->used -= (unsigned)(shift * 8u);
  br->container = gcomp_read_le64(br->base + br->byte_pos);
}

static gcomp_status_t zstd_seq_bit_reader_init(
    zstd_seq_bit_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src || src_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // The marker is the highest set bit of the last byte; everything below it
  // is data, and the byte cannot be zero.
  uint8_t last_byte = src[src_size - 1];
  if (last_byte == 0) {
    return GCOMP_ERR_CORRUPT;
  }
  unsigned marker_bit_in_byte = 7;
  while (
      marker_bit_in_byte > 0 && ((last_byte >> marker_bit_in_byte) & 1) == 0) {
    marker_bit_in_byte--;
  }

  br->total_bits = (unsigned)((src_size - 1) * 8 + marker_bit_in_byte);
  br->bits_consumed = 0;

  if (src_size >= 8) {
    br->base = src;
    br->base_size = src_size;
    br->bit_offset = 0;
  }
  else {
    // Right-aligned in eight bytes, so that the 64-bit load below reads only
    // memory this reader owns.
    memset(br->pad, 0, sizeof(br->pad));
    memcpy(br->pad + (8u - src_size), src, src_size);
    br->base = br->pad;
    br->base_size = 8;
    br->bit_offset = (unsigned)((8u - src_size) * 8u);
  }

  br->limit = 64u - br->bit_offset;
  br->byte_pos = br->base_size - 8u;
  br->container = gcomp_read_le64(br->base + br->byte_pos);
  // Satisfies the invariant in the comment above; works out to 8 minus the
  // marker's bit position, so between one and eight.
  br->used = (unsigned)(br->base_size * 8u) - (br->total_bits + br->bit_offset);

  return GCOMP_OK;
}

/**
 * @brief Take the next @p nb_bits bits, most significant first, without
 *        refilling.
 *
 * The caller is responsible for having refilled recently enough that the bits
 * are in the container.  How much can be taken between refills is fifty-seven
 * bits: a refill leaves `used` below eight, and the container holds
 * sixty-four.  zstd_sequences_execute() spends that budget deliberately;
 * see the note above its loop.
 *
 * Returns zero when the stream does not hold that many bits, which is how the
 * callers detect the end.
 *
 * `used + nb_bits` cannot exceed 64, and no read reaches past the first bit
 * of the stream.  One test holds both; see the note above the reader.
 */
static inline uint32_t zstd_seq_bit_reader_take(
    zstd_seq_bit_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }
  // The refill budget the caller works to is an argument about the widths the
  // format allows, and it is right; but it is an argument about tables that
  // arrive from outside, so it is not what correctness rests on.  If the bits
  // are not in the container, fetch them.
  //
  // On ordinary data the first test fails -- the refill at the top of the
  // sequence loop keeps it failing, which is what that refill is for -- so it
  // costs one predictable comparison.  Were it removed and the loop's budget
  // wrong, the shift below would move by more than sixty-three, which is
  // undefined rather than merely wrong; were it a bare rejection instead of a
  // refill, a stream with a far offset, a long literal run and a long match
  // in one sequence would decode to the wrong bytes and report success.
  if (br->used + nb_bits > br->limit) {
    zstd_seq_bit_reader_refill(br);
    if (br->used + nb_bits > br->limit) {
      // The slide could not move, so this read reaches past the first bit of
      // the stream: the stream is out.
      return 0;
    }
  }

  uint32_t value = (uint32_t)((br->container >> (64u - br->used - nb_bits)) &
      (((uint64_t)1u << nb_bits) - 1u));

  br->used += nb_bits;
  br->bits_consumed += nb_bits;
  return value;
}

/**
 * @brief Take the next @p nb_bits bits and refill.
 *
 * For the few reads that are not inside the sequence loop, where one refill
 * per read costs nothing.
 */
static inline uint32_t zstd_seq_bit_reader_read(
    zstd_seq_bit_reader_t * br, unsigned nb_bits) {
  uint32_t value = zstd_seq_bit_reader_take(br, nb_bits);
  zstd_seq_bit_reader_refill(br);
  return value;
}

//
// Sequences Header Parsing
//

typedef struct {
  uint32_t num_sequences; ///< Number of sequences
  uint8_t ll_mode;        ///< Literal length compression mode
  uint8_t of_mode;        ///< Offset compression mode
  uint8_t ml_mode;        ///< Match length compression mode
} zstd_sequences_header_t;

static gcomp_status_t zstd_sequences_parse_header(const uint8_t * src,
    size_t src_size, zstd_sequences_header_t * header,
    size_t * bytes_read_out) {
  if (!src || !header || !bytes_read_out || src_size < 1) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t pos = 0;
  uint8_t byte0 = src[pos++];

  // Decode number of sequences
  if (byte0 == 0) {
    header->num_sequences = 0;
    *bytes_read_out = 1;
    return GCOMP_OK;
  }
  else if (byte0 < 128) {
    header->num_sequences = byte0;
  }
  else if (byte0 < 255) {
    if (src_size < 2) {
      return GCOMP_ERR_CORRUPT;
    }
    header->num_sequences = ((byte0 - 128) << 8) + src[pos++];
  }
  else {
    // byte0 == 255
    if (src_size < 3) {
      return GCOMP_ERR_CORRUPT;
    }
    header->num_sequences = src[pos] + ((uint32_t)src[pos + 1] << 8) + 0x7F00;
    pos += 2;
  }

  // Read compression modes byte (if sequences > 0)
  if (header->num_sequences > 0) {
    if (pos >= src_size) {
      return GCOMP_ERR_CORRUPT;
    }
    uint8_t modes = src[pos++];
    header->ll_mode = (modes >> 6) & 0x03;
    header->of_mode = (modes >> 4) & 0x03;
    header->ml_mode = (modes >> 2) & 0x03;
    // Bits 0-1 are reserved
  }
  else {
    header->ll_mode = SEQ_MODE_PREDEFINED;
    header->of_mode = SEQ_MODE_PREDEFINED;
    header->ml_mode = SEQ_MODE_PREDEFINED;
  }

  *bytes_read_out = pos;
  return GCOMP_OK;
}

//
// FSE Table Setup
//

/**
 * @brief Refuse a table that can decode a symbol its alphabet does not have.
 *
 * RFC 8878 sections 3.1.1.3.2.1.1 and 3.1.1.3.2.1.2 give literal lengths 36
 * codes, match lengths 53 and offsets 32; a table built from a stream, from
 * an RLE byte, or carried over from a dictionary can name anything a byte
 * can hold.  Every use of one of those symbols is an index into a baseline
 * table or a shift width, so the range has to be established somewhere.
 *
 * Somewhere is here, once per table, rather than once per sequence.  A table
 * has at most 512 entries and a block has three of them; the loop below
 * costs about a thousandth of what asking three times per sequence did.
 *
 * @param table The decoding table
 * @param table_log Its accuracy log, so that `1 << table_log` entries are
 *        the populated ones -- anything past that is left over from an
 *        earlier block and would reject a good stream
 * @param alphabet_size How many codes the alphabet has
 */
static gcomp_status_t zstd_seq_table_in_range(const zstd_fse_entry_t * table,
    unsigned table_log, unsigned alphabet_size) {
  const size_t entries = ((size_t)1u) << table_log;
  for (size_t i = 0; i < entries; i++) {
    if (table[i].symbol >= alphabet_size) {
      return GCOMP_ERR_CORRUPT;
    }
  }
  return GCOMP_OK;
}

static gcomp_status_t zstd_sequences_setup_tables(zstd_decoder_state_t * state,
    const uint8_t * src, size_t src_size,
    const zstd_sequences_header_t * header, size_t * bytes_read_out) {
  size_t pos = 0;
  gcomp_status_t status;

  // Allocate FSE tables if needed
  if (!state->fse_lit_table) {
    state->fse_lit_table = gcomp_malloc(
        state->allocator, FSE_LL_TABLE_SIZE * sizeof(zstd_fse_entry_t));
    if (!state->fse_lit_table) {
      return GCOMP_ERR_MEMORY;
    }
    state->fse_lit_table_size = FSE_LL_TABLE_SIZE;
    gcomp_memory_track_alloc(
        &state->mem_tracker, FSE_LL_TABLE_SIZE * sizeof(zstd_fse_entry_t));
  }

  if (!state->fse_match_table) {
    state->fse_match_table = gcomp_malloc(
        state->allocator, FSE_ML_TABLE_SIZE * sizeof(zstd_fse_entry_t));
    if (!state->fse_match_table) {
      return GCOMP_ERR_MEMORY;
    }
    state->fse_match_table_size = FSE_ML_TABLE_SIZE;
    gcomp_memory_track_alloc(
        &state->mem_tracker, FSE_ML_TABLE_SIZE * sizeof(zstd_fse_entry_t));
  }

  if (!state->fse_offset_table) {
    state->fse_offset_table = gcomp_malloc(
        state->allocator, FSE_OF_TABLE_SIZE * sizeof(zstd_fse_entry_t));
    if (!state->fse_offset_table) {
      return GCOMP_ERR_MEMORY;
    }
    state->fse_offset_table_size = FSE_OF_TABLE_SIZE;
    gcomp_memory_track_alloc(
        &state->mem_tracker, FSE_OF_TABLE_SIZE * sizeof(zstd_fse_entry_t));
  }

  // Setup literal length table
  switch (header->ll_mode) {
  case SEQ_MODE_PREDEFINED:
    status = zstd_fse_build_predefined_ll_table(
        state->fse_lit_table, state->fse_lit_table_size);
    if (status != GCOMP_OK) {
      return status;
    }
    state->fse_ll_log = 6; // Predefined table has log = 6
    state->fse_ll_ready = true;
    break;

  case SEQ_MODE_RLE:
    // RLE mode: single symbol, table log = 0
    if (pos >= src_size) {
      return GCOMP_ERR_CORRUPT;
    }
    {
      uint8_t symbol = src[pos++];
      state->fse_lit_table[0].symbol = symbol;
      state->fse_lit_table[0].nb_bits = 0;
      state->fse_lit_table[0].new_state = 0;
    }
    state->fse_ll_log = 0; // RLE mode: read 0 bits for initial state
    state->fse_ll_ready = true;
    break;

  case SEQ_MODE_FSE: {
    unsigned table_log, max_symbol;
    size_t header_size;
    status = zstd_fse_build_decoding_table(src + pos, src_size - pos,
        state->fse_lit_table, state->fse_lit_table_size, &table_log,
        &max_symbol, &header_size);
    if (status != GCOMP_OK) {
      return status;
    }
    state->fse_ll_log = table_log;
    state->fse_ll_ready = true;
    pos += header_size;
  } break;

  case SEQ_MODE_REPEAT:
    // Use the table an earlier block or a dictionary left behind, keeping
    // its log.  RFC 8878 section 3.1.1.3.2.1.1: there has to be one.
    if (!state->fse_ll_ready) {
      return GCOMP_ERR_CORRUPT;
    }
    break;
  }

  // Setup offset table
  switch (header->of_mode) {
  case SEQ_MODE_PREDEFINED:
    status = zstd_fse_build_predefined_of_table(
        state->fse_offset_table, state->fse_offset_table_size);
    if (status != GCOMP_OK) {
      return status;
    }
    state->fse_of_log = 5; // Predefined table has log = 5
    state->fse_of_ready = true;
    break;

  case SEQ_MODE_RLE:
    if (pos >= src_size) {
      return GCOMP_ERR_CORRUPT;
    }
    {
      uint8_t symbol = src[pos++];
      state->fse_offset_table[0].symbol = symbol;
      state->fse_offset_table[0].nb_bits = 0;
      state->fse_offset_table[0].new_state = 0;
    }
    state->fse_of_log = 0; // RLE mode: read 0 bits for initial state
    state->fse_of_ready = true;
    break;

  case SEQ_MODE_FSE: {
    unsigned table_log, max_symbol;
    size_t header_size;
    status = zstd_fse_build_decoding_table(src + pos, src_size - pos,
        state->fse_offset_table, state->fse_offset_table_size, &table_log,
        &max_symbol, &header_size);
    if (status != GCOMP_OK) {
      return status;
    }
    state->fse_of_log = table_log;
    state->fse_of_ready = true;
    pos += header_size;
  } break;

  case SEQ_MODE_REPEAT:
    // Use the table an earlier block or a dictionary left behind, keeping
    // its log.  RFC 8878 section 3.1.1.3.2.1.1: there has to be one.
    if (!state->fse_of_ready) {
      return GCOMP_ERR_CORRUPT;
    }
    break;
  }

  // Setup match length table
  switch (header->ml_mode) {
  case SEQ_MODE_PREDEFINED:
    status = zstd_fse_build_predefined_ml_table(
        state->fse_match_table, state->fse_match_table_size);
    if (status != GCOMP_OK) {
      return status;
    }
    state->fse_ml_log = 6; // Predefined table has log = 6
    state->fse_ml_ready = true;
    break;

  case SEQ_MODE_RLE:
    if (pos >= src_size) {
      return GCOMP_ERR_CORRUPT;
    }
    {
      uint8_t symbol = src[pos++];
      state->fse_match_table[0].symbol = symbol;
      state->fse_match_table[0].nb_bits = 0;
      state->fse_match_table[0].new_state = 0;
    }
    state->fse_ml_log = 0; // RLE mode: read 0 bits for initial state
    state->fse_ml_ready = true;
    break;

  case SEQ_MODE_FSE: {
    unsigned table_log, max_symbol;
    size_t header_size;
    status = zstd_fse_build_decoding_table(src + pos, src_size - pos,
        state->fse_match_table, state->fse_match_table_size, &table_log,
        &max_symbol, &header_size);
    if (status != GCOMP_OK) {
      return status;
    }
    state->fse_ml_log = table_log;
    state->fse_ml_ready = true;
    pos += header_size;
  } break;

  case SEQ_MODE_REPEAT:
    // Use the table an earlier block or a dictionary left behind, keeping
    // its log.  RFC 8878 section 3.1.1.3.2.1.1: there has to be one.
    if (!state->fse_ml_ready) {
      return GCOMP_ERR_CORRUPT;
    }
    break;
  }

  // Whatever put the three tables there -- this block, an earlier one, or a
  // dictionary -- the sequence loop indexes tables with what they decode, so
  // this is where that is made safe.  SEQ_MODE_REPEAT is checked too: it
  // costs nothing to re-read a table that was already in range, and leaving
  // it out would mean trusting that every path that can install one has been
  // found.
  status = zstd_seq_table_in_range(
      state->fse_lit_table, state->fse_ll_log, ZSTD_SEQ_LL_CODES);
  if (status != GCOMP_OK) {
    return status;
  }
  status = zstd_seq_table_in_range(
      state->fse_match_table, state->fse_ml_log, ZSTD_SEQ_ML_CODES);
  if (status != GCOMP_OK) {
    return status;
  }
  // Offset codes are a shift width, so 31 is the largest one a 32-bit
  // offset can carry; see zstd_decode_one_sequence().
  status = zstd_seq_table_in_range(
      state->fse_offset_table, state->fse_of_log, 32u);
  if (status != GCOMP_OK) {
    return status;
  }

  *bytes_read_out = pos;
  return GCOMP_OK;
}

//
// Sequence Execution
//

/**
 * @brief One sequence as the bitstream describes it, before the repeat offset
 *        list has been consulted.
 */
typedef struct {
  uint32_t literal_length;
  uint32_t match_length;
  uint32_t offset; ///< Offset_Value: 1-3 name repeat offsets, above that +3.
} zstd_decoded_seq_t;

/**
 * @brief Read one sequence out of the bitstream and move the FSE states on.
 *
 * This is the half of a sequence that depends on nothing but the bitstream --
 * three table lookups, the extra bits, and the state transition.  Nothing here
 * touches the output, the window or the repeat offset list, which is what lets
 * the caller run it a sequence ahead of the copying.
 *
 * @param last Non-zero for the final sequence, which does not move the states
 *             on because nothing will read them.
 */
static inline gcomp_status_t zstd_decode_one_sequence(
    zstd_decoder_state_t * state, zstd_seq_bit_reader_t * br,
    zstd_seq_state_t * seq_state, zstd_decoded_seq_t * out, int last) {
  zstd_seq_bit_reader_refill(br);

  // Decode sequence codes from FSE states
  uint8_t ll_code = state->fse_lit_table[seq_state->ll_state].symbol;
  uint8_t of_code = state->fse_offset_table[seq_state->of_state].symbol;
  uint8_t ml_code = state->fse_match_table[seq_state->ml_state].symbol;

  // Per spec offset = 2^code + extra.  Every one of the three codes is in
  // range because the table it came out of was checked when it was built --
  // zstd_seq_table_in_range() -- which is what lets this index the baseline
  // tables and shift by `of_code` without asking again per sequence.
  if (of_code > 0) {
    uint32_t extra = zstd_seq_bit_reader_take(br, of_code);
    out->offset = (1U << of_code) + extra;
  }
  else {
    out->offset = 1;
  }

  // Read extra bits for match length
  out->match_length = zstd_seq_ml_baseline[ml_code] +
      zstd_seq_bit_reader_take(br, zstd_seq_ml_extra_bits[ml_code]);

  // Read extra bits for literal length
  out->literal_length = zstd_seq_ll_baseline[ll_code] +
      zstd_seq_bit_reader_take(br, zstd_seq_ll_extra_bits[ll_code]);

  // Update FSE states (unless last sequence).
  //
  // The three moves read literal-length bits, then match-length bits, then
  // offset bits, from adjacent positions in the stream; and none of the
  // three tables is indexed by a state the others change.  So all three
  // entries are looked up first and all three sets of bits taken at once,
  // most significant first, which puts literal length's share at the top.
  if (!last) {
    const zstd_fse_entry_t * ll_entry =
        &state->fse_lit_table[seq_state->ll_state];
    const zstd_fse_entry_t * ml_entry =
        &state->fse_match_table[seq_state->ml_state];
    const zstd_fse_entry_t * of_entry =
        &state->fse_offset_table[seq_state->of_state];

    const unsigned ll_nb = ll_entry->nb_bits;
    const unsigned ml_nb = ml_entry->nb_bits;
    const unsigned of_nb = of_entry->nb_bits;

    uint32_t bits = zstd_seq_bit_reader_take(br, ll_nb + ml_nb + of_nb);

    seq_state->ll_state =
        (uint16_t)(ll_entry->new_state + (bits >> (ml_nb + of_nb)));
    seq_state->ml_state = (uint16_t)(ml_entry->new_state +
        ((bits >> of_nb) & ((1u << ml_nb) - 1u)));
    seq_state->of_state =
        (uint16_t)(of_entry->new_state + (bits & ((1u << of_nb) - 1u)));
  }

  return GCOMP_OK;
}

static gcomp_status_t zstd_sequences_execute(zstd_decoder_state_t * state,
    const uint8_t * literals, size_t literals_size, const uint8_t * bitstream,
    size_t bitstream_size, uint32_t num_sequences, uint8_t * dst,
    size_t dst_capacity, size_t * output_size_out) {
  if (num_sequences == 0) {
    // No sequences: just copy literals
    if (literals_size > dst_capacity) {
      return GCOMP_ERR_LIMIT;
    }
    memcpy(dst, literals, literals_size);
    *output_size_out = literals_size;
    return GCOMP_OK;
  }

  // Initialize bit reader
  zstd_seq_bit_reader_t br;
  gcomp_status_t status =
      zstd_seq_bit_reader_init(&br, bitstream, bitstream_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // Initialize FSE states (read initial states from bitstream)
  // State reads are in order: LL, OF, ML
  // Number of bits to read = table log for each table
  zstd_seq_state_t seq_state;
  seq_state.ll_state =
      (uint16_t)zstd_seq_bit_reader_read(&br, state->fse_ll_log);
  seq_state.of_state =
      (uint16_t)zstd_seq_bit_reader_read(&br, state->fse_of_log);
  seq_state.ml_state =
      (uint16_t)zstd_seq_bit_reader_read(&br, state->fse_ml_log);

  size_t lit_pos = 0;
  size_t out_pos = 0;

  // HOW THE BITS ARE SPENT
  // ======================
  //
  // Each pass used to take six separate bit reads, and every one refilled the
  // container afterwards -- a branch, and one time in seven a load, sitting in
  // the dependency chain between each read and the next.  Six refills per
  // sequence to consume bits that arrive fifty-six at a time.
  //
  // A refill leaves `used` below eight and the container holds sixty-four, so
  // fifty-seven bits can be taken before another is needed.  What one sequence
  // can spend is bounded, but not by much:
  //
  //   offset extra bits    at most 31  (the of_code > 31 check)
  //   match length extra   at most 16  (zstd_seq_ml_extra_bits)
  //   literal length extra at most 16  (zstd_seq_ll_extra_bits)
  //   three state moves    at most 27  (FSE accuracy log is capped at 9)
  //
  // Ninety in the worst case, so a single refill per sequence cannot be shown
  // to be enough.  It does not have to be: zstd_seq_bit_reader_take() fetches
  // what it needs when it needs it, so correctness does not rest on this
  // arithmetic about tables that arrive from outside.  The refill at the head
  // of zstd_decode_one_sequence() is an optimisation -- it keeps `used` small
  // enough that the check inside take() never fires on ordinary data.
  //
  // Placing a second refill part way through the sequence was tried, on the
  // reasoning that 31+16 and 16+27 both fit.  It measured slower, 690 MB/s
  // against 730, because it does work the take() check was going to skip.
  // Removing the remaining one as well is slower still, around 655.
  //
  // READING ONE SEQUENCE AHEAD WAS TRIED, AND IS NOT HERE
  // =====================================================
  //
  // Reading a sequence and writing one are nearly independent, which is why
  // zstd_decode_one_sequence() is a function: the reading half walks the FSE
  // tables and the bitstream, the writing half below consults the repeat
  // offset list and moves bytes, and nothing the writing half touches feeds
  // back into the tables.  Only the order the repeat offset list is updated in
  // ties them together.
  //
  // That invites decoding sequence i+1 before writing sequence i, so that the
  // FSE table walk -- a chain where a state indexes a table, the entry gives
  // the bits, and the bits give the next state -- runs while a match copy is
  // in flight.  It was written both ways, with a two-slot array and with two
  // structs, and both were slower: 17,242,528 instructions against 15,746,658,
  // and about 660 MB/s against 735.
  //
  // There is nothing for it to hide.  Decoding 2.5 MB of manual pages at level
  // 9, where the window is 2 MB, misses L1 on 1.30% of data reads and the last
  // level on 0.054%: the FSE tables are two kilobytes and the match sources
  // are inside a window that fits several times over in this machine's 12 MB
  // of last-level cache.  What the pipeline would overlap is a four-cycle L1
  // load that the processor reorders around already, and the bookkeeping to
  // arrange it costs more than that.
  //
  // libzstd does pipeline, but to prefetch the match source, which is a
  // hundreds-of-cycles trip to memory when the window is tens of megabytes.
  // At the window sizes this library uses it is not one.
  zstd_decoded_seq_t cur;

  for (uint32_t i = 0; i < num_sequences; i++) {
    gcomp_status_t seq_status = zstd_decode_one_sequence(
        state, &br, &seq_state, &cur, (i + 1) >= num_sequences);
    if (seq_status != GCOMP_OK) {
      return seq_status;
    }

    const uint32_t offset = cur.offset;
    const uint32_t match_length = cur.match_length;
    const uint32_t literal_length = cur.literal_length;

    // RFC 8878 section 3.1.1.5 (Repeat Offsets).  Offset_Value selects an
    // entry in the three-slot repeat list; a literals length of zero shifts
    // that selection up by one, so Offset_Value 1/2/3 mean rep2/rep3/rep1-1
    // instead of rep1/rep2/rep3.  The list update follows the SELECTED slot,
    // not the raw Offset_Value -- keying the update off Offset_Value left the
    // repeat list wrong for every zero-literal sequence, and the first match
    // that then reused a repeat offset copied from the wrong distance.
    uint32_t actual_offset;
    if (offset > 3) {
      actual_offset = offset - 3;
      state->rep_offset_3 = state->rep_offset_2;
      state->rep_offset_2 = state->rep_offset_1;
      state->rep_offset_1 = actual_offset;
    }
    else {
      unsigned idx = (unsigned)offset - 1u + (literal_length == 0 ? 1u : 0u);

      if (idx == 0) {
        // rep1 reused: the list is unchanged.
        actual_offset = state->rep_offset_1;
      }
      else if (idx == 1) {
        actual_offset = state->rep_offset_2;
        state->rep_offset_2 = state->rep_offset_1;
        state->rep_offset_1 = actual_offset;
      }
      else if (idx == 2) {
        actual_offset = state->rep_offset_3;
        state->rep_offset_3 = state->rep_offset_2;
        state->rep_offset_2 = state->rep_offset_1;
        state->rep_offset_1 = actual_offset;
      }
      else {
        // Only reachable as Offset_Value 3 with no literals: rep1 - 1, which
        // then enters the list as a new offset would.
        if (state->rep_offset_1 <= 1) {
          return GCOMP_ERR_CORRUPT;
        }
        actual_offset = state->rep_offset_1 - 1;
        state->rep_offset_3 = state->rep_offset_2;
        state->rep_offset_2 = state->rep_offset_1;
        state->rep_offset_1 = actual_offset;
      }
    }

    // Copy literals.  Both buffers carry ZSTD_DECODE_SLACK spare bytes past
    // the capacities checked here, which is what lets this overshoot rather
    // than pick a width by length; see fastcopy.h.
    if (lit_pos + literal_length > literals_size) {
      return GCOMP_ERR_CORRUPT;
    }
    if (out_pos + literal_length > dst_capacity) {
      return GCOMP_ERR_LIMIT;
    }
    gcomp_copy_slack(dst + out_pos, literals + lit_pos, literal_length);
    out_pos += literal_length;
    lit_pos += literal_length;

    // Copy match - may reference window buffer (history from previous blocks)
    // or current block's output
    //
    // Total available history = window_size + out_pos
    // - window_size bytes from window_buffer (previous blocks)
    // - out_pos bytes from dst (current block)
    size_t total_history = state->window_size + out_pos;
    if (actual_offset == 0 || actual_offset > total_history) {
      return GCOMP_ERR_CORRUPT;
    }
    if (out_pos + match_length > dst_capacity) {
      return GCOMP_ERR_LIMIT;
    }

    // Copy the match.
    //
    // A match reads from one of two places: the window, which holds what
    // earlier blocks decoded, or this block's own output.  It can start in
    // the window and finish in the output, but it crosses between them at
    // most once, and within the window it wraps at most once more.  So there
    // are at most three flat runs, and asking which one applies per byte --
    // as this used to, along with a circular index computation for every byte
    // that came from the window -- was the largest single cost in a
    // Zstandard decode.
    //
    // Only the first of those runs is the window's, and only a match near the
    // start of a block has one at all.  The rest is the common case and is
    // one call with no loop around it.
    size_t remaining_match = match_length;

    // The part that predates this block, taken from the circular window.
    // Everything here is already written, so these runs never overlap what
    // they are writing.
    while (remaining_match > 0u && actual_offset > out_pos) {
      size_t win_offset = actual_offset - out_pos;
      if (win_offset > state->window_size) {
        return GCOMP_ERR_CORRUPT;
      }
      size_t win_idx = (state->window_pos >= win_offset)
          ? (state->window_pos - win_offset)
          : (state->window_capacity - (win_offset - state->window_pos));

      // Stop at whichever comes first: the end of the match, the end of the
      // window's history, or the wrap of the circular buffer.
      size_t run = remaining_match;
      if (run > win_offset) {
        run = win_offset;
      }
      if (run > state->window_capacity - win_idx) {
        run = state->window_capacity - win_idx;
      }
      if (run == 0u) {
        return GCOMP_ERR_CORRUPT; // Cannot happen; refuse to spin if it does.
      }

      gcomp_copy_short(dst + out_pos, state->window_buffer + win_idx, run);
      out_pos += run;
      remaining_match -= run;
    }

    // The rest comes from what this block has already written.  Source and
    // destination are the same buffer and the source may be the nearer of the
    // two, so this is a repeat of a period rather than a copy, and
    // gcomp_copy_repeat_slack() is the one that knows how to do that without
    // splitting the length into runs of `actual_offset` bytes.
    //
    // The loop above leaves `actual_offset <= out_pos` whenever it leaves
    // anything to do, which is the precondition that function asks for: it
    // exits only when its own condition fails, and out_pos never shrinks.
    // A match length is at least three (zstd_seq_ml_baseline), so there is
    // never a zero-length repeat.
    if (remaining_match > 0u) {
      gcomp_copy_repeat_slack(dst + out_pos, actual_offset, remaining_match);
      out_pos += remaining_match;
    }
  }

  // RFC 8878 section 3.1.1.3.2.1: the bitstream holds exactly the sequences
  // the header counted, and the marker bit says where its data ends.  A
  // decoder that has read the number it was told and is not standing on that
  // boundary has been reading something other than what was written: the
  // count is wrong, or the stream is short of what the count needs, or the
  // tables it decoded with are not the tables it was encoded with.
  //
  // Without this those are silent.  zstd_seq_bit_reader_take() answers zero
  // past the end of the stream, so the sequences after that point are
  // invented rather than refused, and every bound below is still satisfied
  // because invented sequences are small -- a block that had lost bits
  // decoded to plausible bytes and reported success.  libzstd makes the same
  // check (BIT_endOfDStream) and calls the failure corruption_detected.
  if (br.bits_consumed != br.total_bits) {
    return GCOMP_ERR_CORRUPT;
  }

  // Copy remaining literals
  size_t remaining_literals = literals_size - lit_pos;
  if (out_pos + remaining_literals > dst_capacity) {
    return GCOMP_ERR_LIMIT;
  }
  if (remaining_literals > 0) {
    memcpy(dst + out_pos, literals + lit_pos, remaining_literals);
    out_pos += remaining_literals;
  }

  *output_size_out = out_pos;
  return GCOMP_OK;
}

//
// Public API
//

gcomp_status_t zstd_sequences_decode(zstd_decoder_state_t * state,
    const uint8_t * src, size_t src_size, const uint8_t * literals,
    size_t literals_size, uint8_t * dst, size_t dst_capacity,
    size_t * output_size_out, size_t * bytes_read_out) {
  if (!state || !src || !literals || !dst || !output_size_out ||
      !bytes_read_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Parse sequences header
  zstd_sequences_header_t header;
  size_t header_size;

  gcomp_status_t status =
      zstd_sequences_parse_header(src, src_size, &header, &header_size);
  if (status != GCOMP_OK) {
    return status;
  }

  if (header.num_sequences == 0) {
    // No sequences: just copy literals
    if (literals_size > dst_capacity) {
      return GCOMP_ERR_LIMIT;
    }
    memcpy(dst, literals, literals_size);
    *output_size_out = literals_size;
    *bytes_read_out = header_size;
    return GCOMP_OK;
  }

  // Setup FSE tables
  size_t tables_size;
  status = zstd_sequences_setup_tables(
      state, src + header_size, src_size - header_size, &header, &tables_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // Execute sequences
  const uint8_t * bitstream = src + header_size + tables_size;
  size_t bitstream_size = src_size - header_size - tables_size;

  status = zstd_sequences_execute(state, literals, literals_size, bitstream,
      bitstream_size, header.num_sequences, dst, dst_capacity, output_size_out);
  if (status != GCOMP_OK) {
    return status;
  }

  *bytes_read_out = src_size; // Entire section consumed
  return GCOMP_OK;
}
