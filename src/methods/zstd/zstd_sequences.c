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
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
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
// Baseline and Extra Bits Tables
//

// Literal length baseline and extra bits
static const uint32_t ll_baseline[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024,
    2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t ll_extra_bits[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// Match length baseline and extra bits
static const uint32_t ml_baseline[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
    34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051,
    4099, 8195, 16387, 32771, 65539};
static const uint8_t ml_extra_bits[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
    3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

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
// FSE bitstreams in zstd are written in reverse order. The last byte contains
// a marker bit (the highest set bit) followed by padding zeros. We read bits
// from the marker downward toward bit 0.
//

typedef struct {
  const uint8_t * src;        ///< Source data pointer
  size_t src_size;            ///< Total source size
  uint64_t bit_container;     ///< Loaded bits
  unsigned bits_in_container; ///< Valid bits currently in container
  unsigned total_bits;        ///< Total valid bits in stream (excluding marker)
  unsigned bits_consumed;     ///< Total bits consumed from stream
} zstd_seq_bit_reader_t;

/**
 * @brief Reload the bit container from the source stream.
 *
 * For backward bitstreams, we read from high bit positions to low.
 * The container holds a window of bits, and we reload when needed.
 */
static void zstd_seq_bit_reader_reload(zstd_seq_bit_reader_t * br) {
  // The current bit position in the stream is:
  // (total_bits - bits_consumed) from the high end
  // Which corresponds to byte position:
  // (total_bits - bits_consumed) / 8 from the start

  // We need to ensure we have enough bits loaded.
  // Load bytes from the position we need, going backward.

  unsigned bits_remaining = br->total_bits - br->bits_consumed;
  if (bits_remaining == 0) {
    return;
  }

  // Calculate which byte contains the next bits we need
  // bits_remaining is the number of bits left, counting from bit 0
  unsigned next_bit_pos = bits_remaining - 1;
  unsigned byte_containing_next = next_bit_pos / 8;

  // Load up to 8 bytes starting from where we need data
  // We want to load bytes [start_byte, start_byte + 7] or available range
  size_t start_byte = 0;
  if (byte_containing_next >= 7) {
    start_byte = byte_containing_next - 7;
  }

  size_t bytes_to_load = br->src_size - start_byte;
  if (bytes_to_load > 8) {
    bytes_to_load = 8;
  }

  br->bit_container = 0;
  for (size_t i = 0; i < bytes_to_load; i++) {
    br->bit_container |= (uint64_t)br->src[start_byte + i] << (i * 8);
  }

  // Track how many bits are in the container and their position
  // The container now holds bits [start_byte * 8, start_byte * 8 + loaded * 8)
  br->bits_in_container = (unsigned)(bytes_to_load * 8);
}

static gcomp_status_t zstd_seq_bit_reader_init(
    zstd_seq_bit_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src || src_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  br->src = src;
  br->src_size = src_size;

  // Find initialization marker (highest set bit in last byte)
  uint8_t last_byte = src[src_size - 1];
  if (last_byte == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  // Find position of highest set bit
  unsigned marker_bit_in_byte = 7;
  while (
      marker_bit_in_byte > 0 && ((last_byte >> marker_bit_in_byte) & 1) == 0) {
    marker_bit_in_byte--;
  }

  // Total data bits = (src_size - 1) * 8 + marker_bit_in_byte
  // The marker itself is at bit (src_size - 1) * 8 + marker_bit_in_byte
  // Valid data bits are from 0 to marker_bit - 1
  br->total_bits = (unsigned)((src_size - 1) * 8 + marker_bit_in_byte);
  br->bits_consumed = 0;
  br->bits_in_container = 0;

  // Initial load
  zstd_seq_bit_reader_reload(br);

  return GCOMP_OK;
}

static uint32_t zstd_seq_bit_reader_read(
    zstd_seq_bit_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  // Check for underflow
  if (br->bits_consumed + nb_bits > br->total_bits) {
    return 0; // Not enough bits
  }

  // Reload if needed
  zstd_seq_bit_reader_reload(br);

  // Calculate the bit position within the stream
  // We're reading from (total_bits - bits_consumed - nb_bits) to
  // (total_bits - bits_consumed - 1)
  unsigned bits_remaining = br->total_bits - br->bits_consumed;
  unsigned next_bit_pos = bits_remaining - nb_bits;

  // Calculate which byte in the source this corresponds to
  unsigned byte_offset = next_bit_pos / 8;
  unsigned bit_in_byte = next_bit_pos % 8;

  // The container was loaded starting from some byte offset
  // We need to find where our bits are in the container
  unsigned container_start_byte = 0;
  if (br->total_bits > br->bits_consumed) {
    unsigned top_bit = br->total_bits - br->bits_consumed - 1;
    unsigned top_byte = top_bit / 8;
    if (top_byte >= 7) {
      container_start_byte = top_byte - 7;
    }
  }

  // Position in container
  unsigned container_bit_pos =
      (byte_offset - container_start_byte) * 8 + bit_in_byte;

  uint32_t value = (uint32_t)(br->bit_container >> container_bit_pos) &
      ((1U << nb_bits) - 1);

  br->bits_consumed += nb_bits;
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
    pos += header_size;
  } break;

  case SEQ_MODE_REPEAT:
    // Use existing table (already populated, keep existing table log)
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
    pos += header_size;
  } break;

  case SEQ_MODE_REPEAT:
    // Use existing table (already populated, keep existing table log)
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
    pos += header_size;
  } break;

  case SEQ_MODE_REPEAT:
    // Use existing table (already populated, keep existing table log)
    break;
  }

  *bytes_read_out = pos;
  return GCOMP_OK;
}

//
// Sequence Execution
//

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

  for (uint32_t i = 0; i < num_sequences; i++) {
    // Decode sequence codes from FSE states
    uint8_t ll_code = state->fse_lit_table[seq_state.ll_state].symbol;
    uint8_t of_code = state->fse_offset_table[seq_state.of_state].symbol;
    uint8_t ml_code = state->fse_match_table[seq_state.ml_state].symbol;

    // Read extra bits for offset first (important: offset extra bits read
    // first). Per spec offset = 2^code + extra; code must be at most 31 to
    // avoid undefined shift on 32-bit type.
    if (of_code > 31) {
      return GCOMP_ERR_CORRUPT;
    }
    uint32_t offset;
    if (of_code > 0) {
      uint32_t extra = zstd_seq_bit_reader_read(&br, of_code);
      offset = (1U << of_code) + extra;
    }
    else {
      offset = 1;
    }

    // Read extra bits for match length
    uint32_t match_length;
    if (ml_code < 53) {
      uint32_t extra = zstd_seq_bit_reader_read(&br, ml_extra_bits[ml_code]);
      match_length = ml_baseline[ml_code] + extra;
    }
    else {
      return GCOMP_ERR_CORRUPT;
    }

    // Read extra bits for literal length
    uint32_t literal_length;
    if (ll_code < 36) {
      uint32_t extra = zstd_seq_bit_reader_read(&br, ll_extra_bits[ll_code]);
      literal_length = ll_baseline[ll_code] + extra;
    }
    else {
      return GCOMP_ERR_CORRUPT;
    }

    // Update FSE states (unless last sequence)
    if (i < num_sequences - 1) {
      // Update LL state
      {
        const zstd_fse_entry_t * entry =
            &state->fse_lit_table[seq_state.ll_state];
        uint32_t bits = zstd_seq_bit_reader_read(&br, entry->nb_bits);
        seq_state.ll_state = entry->new_state + bits;
      }

      // Update ML state
      {
        const zstd_fse_entry_t * entry =
            &state->fse_match_table[seq_state.ml_state];
        uint32_t bits = zstd_seq_bit_reader_read(&br, entry->nb_bits);
        seq_state.ml_state = entry->new_state + bits;
      }

      // Update OF state
      {
        const zstd_fse_entry_t * entry =
            &state->fse_offset_table[seq_state.of_state];
        uint32_t bits = zstd_seq_bit_reader_read(&br, entry->nb_bits);
        seq_state.of_state = entry->new_state + bits;
      }
    }

    // Handle repeat offsets
    uint32_t actual_offset = offset;
    if (offset <= 3) {
      // Repeat offset
      if (literal_length == 0) {
        // Special case: offset codes are shifted
        if (offset == 3) {
          actual_offset = state->rep_offset_1 - 1;
        }
        else if (offset == 1) {
          actual_offset = state->rep_offset_2;
        }
        else { // offset == 2
          actual_offset = state->rep_offset_3;
        }
      }
      else {
        if (offset == 1) {
          actual_offset = state->rep_offset_1;
        }
        else if (offset == 2) {
          actual_offset = state->rep_offset_2;
        }
        else { // offset == 3
          actual_offset = state->rep_offset_3;
        }
      }

      // Update repeat offsets
      if (offset != 1) {
        if (offset == 2) {
          uint32_t temp = state->rep_offset_2;
          state->rep_offset_2 = state->rep_offset_1;
          state->rep_offset_1 = temp;
        }
        else { // offset == 3
          uint32_t temp = state->rep_offset_3;
          state->rep_offset_3 = state->rep_offset_2;
          state->rep_offset_2 = state->rep_offset_1;
          state->rep_offset_1 = temp;
        }
      }
    }
    else {
      // New offset
      actual_offset = offset - 3;
      state->rep_offset_3 = state->rep_offset_2;
      state->rep_offset_2 = state->rep_offset_1;
      state->rep_offset_1 = actual_offset;
    }

    // Copy literals
    if (lit_pos + literal_length > literals_size) {
      return GCOMP_ERR_CORRUPT;
    }
    if (out_pos + literal_length > dst_capacity) {
      return GCOMP_ERR_LIMIT;
    }
    memcpy(dst + out_pos, literals + lit_pos, literal_length);
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

    // Byte-by-byte copy for overlapping matches
    // If offset <= out_pos, copy from current block's output
    // If offset > out_pos, part or all comes from window buffer
    for (uint32_t j = 0; j < match_length; j++) {
      uint8_t byte;
      if (actual_offset <= out_pos) {
        // Source is within current block's output
        byte = dst[out_pos - actual_offset];
      }
      else {
        // Source is in window buffer (circular)
        // Position in window = window_size - (actual_offset - out_pos)
        size_t win_offset = actual_offset - out_pos;
        if (win_offset > state->window_size) {
          return GCOMP_ERR_CORRUPT;
        }
        size_t win_idx;
        if (state->window_pos >= win_offset) {
          win_idx = state->window_pos - win_offset;
        }
        else {
          // Wrap around in circular buffer
          win_idx = state->window_capacity - (win_offset - state->window_pos);
        }
        byte = state->window_buffer[win_idx];
      }
      dst[out_pos++] = byte;
    }
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

//
// Encoder: Bit Writer for Sequences (Backward Bitstream)
//
// ============================================================================
// FSE BACKWARD BITSTREAM FORMAT
// ============================================================================
//
// Zstd FSE bitstreams are written and read BACKWARD. This is counter-intuitive
// but enables efficient decoding. Here's how it works:
//
// MEMORY LAYOUT (after encoding):
//   Address:    Low ──────────────────────────────────────────────> High
//   Bytes:      [B0] [B1] [B2] ... [Bn-1] [Bn]
//                                          ^
//                                          └── Marker bit is highest '1' here
//
// DECODER BEHAVIOR:
//   1. Load bytes from END of buffer (Bn, Bn-1, ...)
//   2. Find marker (highest set bit)
//   3. Read bits from HIGH positions toward LOW positions
//   4. First bits read are INITIAL STATES (LL, OF, ML)
//   5. Then read sequence data (extra bits, state updates)
//
// ENCODER STRATEGY:
//   To make first-written bits appear at highest positions (read first):
//   1. Add bits at HIGH end of accumulator (left-shift existing, OR new bits)
//   2. Flush LOW bytes to increasing addresses
//   3. Add marker bit LAST (ends up at highest position in last byte)
//
// EXAMPLE (simplified):
//   Write: A(6 bits), B(5 bits), marker
//   Accumulator after A:    0b000000AAAAAA (bits 0-5)
//   Accumulator after B:    0bBBBBBAAAAAA (bits 0-10, B above A)
//   After marker:           0b1BBBBBAAAAAА (bit 11 is marker)
//   Flush to memory:        [low byte of A] [high bits + marker]
//   Decoder reads marker first, then B, then A
//
// This approach ensures the decoder's read order matches the logical order
// of the encoded data (initial states first, then sequences).
//

typedef struct {
  uint8_t * buf;          ///< Output buffer (start)
  size_t buf_size;        ///< Buffer capacity
  size_t byte_pos;        ///< Current write position (increasing)
  uint64_t bit_container; ///< Bit accumulator
  unsigned bits_used;     ///< Bits used in container (0-64)
} zstd_enc_bit_writer_t;

static void zstd_enc_bw_init(
    zstd_enc_bit_writer_t * bw, uint8_t * buf, size_t buf_size) {
  bw->buf = buf;
  bw->buf_size = buf_size;
  bw->byte_pos = 0;
  bw->bit_container = 0;
  bw->bits_used = 0;
}

/**
 * @brief Flush complete bytes from the bit container.
 *
 * Writes LOW bytes to increasing addresses until fewer than 8 bits remain.
 */
static void zstd_enc_bw_flush(zstd_enc_bit_writer_t * bw) {
  while (bw->bits_used >= 8 && bw->byte_pos < bw->buf_size) {
    bw->buf[bw->byte_pos++] = (uint8_t)(bw->bit_container & 0xFF);
    bw->bit_container >>= 8;
    bw->bits_used -= 8;
  }
}

/**
 * @brief Add bits to the bitstream.
 *
 * New bits are added at the HIGH end (above existing bits).
 * First bits added end up at LOW positions; last bits at HIGH positions.
 * Combined with flushing LOW bytes to increasing addresses, this means:
 * - First bits added → LOW addresses → LOW bit positions when loaded
 * - Last bits added (marker) → HIGH addresses → HIGH bit positions when loaded
 *
 * Automatically flushes when the container gets too full.
 */
static void zstd_enc_bw_add_bits(
    zstd_enc_bit_writer_t * bw, uint32_t value, unsigned nb_bits) {
  if (nb_bits == 0) {
    return;
  }

  // Flush if we don't have room for the new bits
  // Keep at least 32 bits of headroom for safety
  if (bw->bits_used + nb_bits > 56) {
    zstd_enc_bw_flush(bw);
  }

  // Add new bits above existing bits
  bw->bit_container |= ((uint64_t)value << bw->bits_used);
  bw->bits_used += nb_bits;
}


/**
 * @brief Close the bitstream and add the marker bit.
 *
 * The marker is added at the HIGH end (becomes highest set bit in last byte).
 * Returns the total stream size.
 */
static size_t zstd_enc_bw_close(zstd_enc_bit_writer_t * bw) {
  // Add marker bit at HIGH end
  bw->bit_container |= (1ULL << bw->bits_used);
  bw->bits_used++;

  // Calculate how many bytes we need to write
  // We need ceil(bits_used / 8) bytes
  unsigned bytes_needed = (bw->bits_used + 7) / 8;

  // Flush exactly the needed bytes
  for (unsigned i = 0; i < bytes_needed && bw->byte_pos < bw->buf_size; i++) {
    bw->buf[bw->byte_pos++] = (uint8_t)(bw->bit_container & 0xFF);
    bw->bit_container >>= 8;
  }

  return bw->byte_pos;
}


//
// Encoder: Symbol to Code Conversion
//

static uint8_t zstd_enc_get_ll_code(uint32_t ll) {
  if (ll < 16) {
    return (uint8_t)ll;
  }
  if (ll < 18) {
    return 16;
  }
  if (ll < 20) {
    return 17;
  }
  if (ll < 22) {
    return 18;
  }
  if (ll < 24) {
    return 19;
  }
  if (ll < 28) {
    return 20;
  }
  if (ll < 32) {
    return 21;
  }
  if (ll < 40) {
    return 22;
  }
  if (ll < 48) {
    return 23;
  }
  if (ll < 64) {
    return 24;
  }
  if (ll < 128) {
    return 25;
  }
  if (ll < 256) {
    return 26;
  }
  if (ll < 512) {
    return 27;
  }
  if (ll < 1024) {
    return 28;
  }
  if (ll < 2048) {
    return 29;
  }
  if (ll < 4096) {
    return 30;
  }
  if (ll < 8192) {
    return 31;
  }
  if (ll < 16384) {
    return 32;
  }
  if (ll < 32768) {
    return 33;
  }
  if (ll < 65536) {
    return 34;
  }
  return 35;
}

static uint8_t zstd_enc_get_ml_code(uint32_t ml) {
  if (ml < 3) {
    return 0;
  }
  ml -= 3;
  if (ml < 32) {
    return (uint8_t)ml;
  }
  if (ml < 34) {
    return 32;
  }
  if (ml < 36) {
    return 33;
  }
  if (ml < 38) {
    return 34;
  }
  if (ml < 40) {
    return 35;
  }
  if (ml < 44) {
    return 36;
  }
  if (ml < 48) {
    return 37;
  }
  if (ml < 56) {
    return 38;
  }
  if (ml < 64) {
    return 39;
  }
  if (ml < 80) {
    return 40;
  }
  if (ml < 96) {
    return 41;
  }
  if (ml < 128) {
    return 42;
  }
  if (ml < 256) {
    return 43;
  }
  if (ml < 512) {
    return 44;
  }
  if (ml < 1024) {
    return 45;
  }
  if (ml < 2048) {
    return 46;
  }
  if (ml < 4096) {
    return 47;
  }
  if (ml < 8192) {
    return 48;
  }
  if (ml < 16384) {
    return 49;
  }
  if (ml < 32768) {
    return 50;
  }
  if (ml < 65536) {
    return 51;
  }
  return 52;
}

static uint8_t zstd_enc_get_of_code(uint32_t offset) {
  if (offset == 0) {
    return 0;
  }
  // of_code = highest set bit position (floor(log2(offset)))
  return (uint8_t)(31 - __builtin_clz(offset));
}


//
// FSE Encoding Helpers
//
// ============================================================================
// FSE STATE MACHINE FOR ENCODING
// ============================================================================
//
// FSE encoding works by finding states that will produce the desired symbols
// when decoded. The key insight is that encoding is the REVERSE of decoding:
//
// DECODING (given state S):
//   symbol = table[S].symbol
//   nb_bits = table[S].nb_bits
//   bits = read_bits(nb_bits)
//   next_state = table[S].new_state + bits
//
// ENCODING (given symbol and target next_state):
//   Find state S where:
//     - table[S].symbol == symbol
//     - table[S].new_state <= next_state < table[S].new_state + 2^nb_bits
//   Output bits = next_state - table[S].new_state
//
// The encoding algorithm processes sequences in REVERSE order:
// 1. Start with the last sequence, pick any valid state for each symbol
// 2. For each earlier sequence, find states that transition TO the current
// states
// 3. After processing all sequences, output the final states as initial states
//

/**
 * @brief Find any FSE state that decodes to the given symbol.
 *
 * Used for the last sequence where we can pick any valid state for the symbol.
 * The returned state becomes the "target" for encoding the previous sequence.
 *
 * @param table FSE decoding table
 * @param table_size Number of entries in table
 * @param symbol Symbol to find
 * @return State index, or 0 as fallback
 */
static uint16_t zstd_enc_find_state_for_symbol(
    const zstd_fse_entry_t * table, size_t table_size, uint8_t symbol) {
  for (size_t i = 0; i < table_size; i++) {
    if (table[i].symbol == symbol) {
      return (uint16_t)i;
    }
  }
  return 0; // Fallback (should not happen for valid symbols)
}

/**
 * @brief Find FSE encoding state given symbol and target next-state.
 *
 * For encoding, we need to find a state S such that:
 * - table[S].symbol == symbol
 * - table[S].new_state <= next_state < table[S].new_state + (1 <<
 * table[S].nb_bits)
 *
 * The bits to output are: next_state - table[S].new_state
 *
 * @param table FSE decoding table
 * @param table_size Table size
 * @param symbol Symbol to encode
 * @param next_state Target state after decoding (state decoder will be in)
 * @param bits_out Output: bits to write to bitstream
 * @param nb_bits_out Output: number of bits to write
 * @return State index, or 0xFFFF if not found
 */
static uint16_t zstd_enc_find_encode_state(const zstd_fse_entry_t * table,
    size_t table_size, uint8_t symbol, uint16_t next_state, uint8_t * bits_out,
    uint8_t * nb_bits_out) {
  for (size_t i = 0; i < table_size; i++) {
    if (table[i].symbol != symbol) {
      continue;
    }
    uint16_t base = table[i].new_state;
    uint8_t nb_bits = table[i].nb_bits;
    uint16_t range = (uint16_t)(1U << nb_bits);
    if (next_state >= base && next_state < base + range) {
      *bits_out = (uint8_t)(next_state - base);
      *nb_bits_out = nb_bits;
      return (uint16_t)i;
    }
  }
  // Should not happen for valid symbols and states
  *bits_out = 0;
  *nb_bits_out = 0;
  return 0xFFFF;
}

//
// Public API: Encode Sequences
//

/**
 * @brief Encode sequences using predefined FSE tables.
 *
 * This function encodes a series of LZ sequences (literal_length, offset,
 * match_length) using the predefined FSE tables specified in RFC 8878.
 *
 * ## Output Format
 *
 * ```
 * [num_sequences: 1-3 bytes]
 * [modes_byte: 0x00 for all predefined]
 * [FSE bitstream: variable]
 * ```
 *
 * ## Encoding Algorithm
 *
 * 1. Write sequence count (variable length encoding)
 * 2. Write modes byte (0x00 = all predefined tables)
 * 3. Build predefined FSE tables
 * 4. Process sequences in REVERSE order (last → first):
 *    - For last sequence: find initial states, add extra bits
 *    - For others: find states that transition to target, add state bits +
 * extra
 * 5. Add initial states to bitstream
 * 6. Add marker bit and close bitstream
 *
 * ## Bitstream Layout (after encoding)
 *
 * Memory order (low address → high address):
 * ```
 * [extra bits seq N-1] [state+extra seq N-2] ... [state+extra seq 0]
 * [ML_init] [OF_init] [LL_init] [marker]
 * ```
 *
 * Decoder reads from marker backward, getting:
 * - Initial states (LL, OF, ML)
 * - For each sequence: extra bits (OF, ML, LL), then state updates
 *
 * @param sequences Array of sequences to encode
 * @param num_sequences Number of sequences
 * @param output Output buffer
 * @param output_cap Output buffer capacity
 * @param output_len_out Output: bytes written
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_sequences_encode_predefined(
    const zstd_sequence_t * sequences, size_t num_sequences, uint8_t * output,
    size_t output_cap, size_t * output_len_out) {
  if (!output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t pos = 0;

  // Handle zero sequences
  if (num_sequences == 0 || !sequences) {
    if (output_cap < 1) {
      return GCOMP_ERR_LIMIT;
    }
    output[pos++] = 0; // 0 sequences
    *output_len_out = pos;
    return GCOMP_OK;
  }

  // 1. Write number of sequences (1-3 bytes)
  if (num_sequences < 128) {
    if (pos >= output_cap) {
      return GCOMP_ERR_LIMIT;
    }
    output[pos++] = (uint8_t)num_sequences;
  }
  else if (num_sequences < 0x7F00) {
    if (pos + 2 > output_cap) {
      return GCOMP_ERR_LIMIT;
    }
    output[pos++] = (uint8_t)((num_sequences >> 8) + 128);
    output[pos++] = (uint8_t)(num_sequences & 0xFF);
  }
  else {
    if (pos + 3 > output_cap) {
      return GCOMP_ERR_LIMIT;
    }
    output[pos++] = 255;
    uint32_t val = (uint32_t)num_sequences - 0x7F00;
    output[pos++] = (uint8_t)(val & 0xFF);
    output[pos++] = (uint8_t)((val >> 8) & 0xFF);
  }

  // 2. Write compression modes byte (all predefined: LL=0, OF=0, ML=0)
  if (pos >= output_cap) {
    return GCOMP_ERR_LIMIT;
  }
  output[pos++] = 0x00; // All modes = 0 (predefined)

  // 3. Build predefined FSE decoding tables for state lookup
  // Predefined table sizes: LL log=6 (64 entries), OF log=5 (32), ML log=6 (64)
  zstd_fse_entry_t ll_table[64];
  zstd_fse_entry_t of_table[32];
  zstd_fse_entry_t ml_table[64];

  zstd_fse_build_predefined_ll_table(ll_table, 64);
  zstd_fse_build_predefined_of_table(of_table, 32);
  zstd_fse_build_predefined_ml_table(ml_table, 64);

  // Check for reasonable sequence count to avoid excessive stack usage
  if (num_sequences > 65535) {
    return GCOMP_ERR_LIMIT; // Too many sequences
  }

  // Estimate max bitstream size:
  // - Initial states: 6 + 5 + 6 = 17 bits
  // - Per sequence: up to 16 (OF extra) + 16 (ML extra) + 16 (LL extra) +
  //                 6 (LL state) + 6 (ML state) + 5 (OF state) = 65 bits
  // - Marker: 1 bit
  // Round up generously
  size_t max_bitstream = 20 + num_sequences * 10; // ~80 bits/seq is very safe
  if (pos + max_bitstream > output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  zstd_enc_bit_writer_t bw;
  zstd_enc_bw_init(&bw, output + pos, output_cap - pos);

  // FSE encoding state machine:
  // We track the "next state" for each table - this is the state the decoder
  // will be in AFTER reading the state update bits we output.
  //
  // Algorithm:
  // 1. Process sequences in REVERSE order (last to first)
  // 2. For the last sequence (processed first): find initial states for symbols
  // 3. For each prior sequence: find state that transitions to current state
  // 4. Output initial states at the end (decoder reads them first)

  // Encoding states (initially set when processing last sequence)
  uint16_t enc_state_ll = 0;
  uint16_t enc_state_of = 0;
  uint16_t enc_state_ml = 0;

  // Process sequences in reverse order
  //
  // Decoder reads per sequence (not last):
  //   1. OF_extra, ML_extra, LL_extra (extra bits)
  //   2. LL_bits, ML_bits, OF_bits (state update bits)
  //
  // Encoder adds in reverse: OF_bits, ML_bits, LL_bits, LL_extra, ML_extra,
  // OF_extra
  //
  // For the last sequence (processed first when encoding in reverse):
  //   - No state update bits
  //   - Just find valid states for symbols
  //
  for (size_t idx = num_sequences; idx > 0; idx--) {
    size_t i = idx - 1;
    const zstd_sequence_t * seq = &sequences[i];

    // Compute symbol codes
    uint8_t ll_code = zstd_enc_get_ll_code(seq->lit_length);
    uint8_t ml_code = zstd_enc_get_ml_code(seq->match_length);
    uint8_t of_code = zstd_enc_get_of_code(seq->match_offset);

    // Extra bit values
    uint32_t ll_extra_val = seq->lit_length - ll_baseline[ll_code];
    uint32_t ml_extra_val = seq->match_length - ml_baseline[ml_code];
    uint32_t of_extra_val =
        (of_code > 0) ? seq->match_offset - (1U << of_code) : 0;

    // Number of extra bits
    unsigned ll_nb_extra = ll_extra_bits[ll_code];
    unsigned ml_nb_extra = ml_extra_bits[ml_code];
    unsigned of_nb_extra = of_code;

    if (i == num_sequences - 1) {
      // Last sequence (processed first): just find any valid state for symbol
      // No state update bits for the last sequence
      enc_state_ll = zstd_enc_find_state_for_symbol(ll_table, 64, ll_code);
      enc_state_of = zstd_enc_find_state_for_symbol(of_table, 32, of_code);
      enc_state_ml = zstd_enc_find_state_for_symbol(ml_table, 64, ml_code);

      // Add extra bits for last sequence (decoder reads: OF, ML, LL)
      // Encoder adds in reverse: LL, ML, OF
      if (ll_nb_extra > 0) {
        zstd_enc_bw_add_bits(&bw, ll_extra_val, ll_nb_extra);
      }
      if (ml_nb_extra > 0) {
        zstd_enc_bw_add_bits(&bw, ml_extra_val, ml_nb_extra);
      }
      if (of_nb_extra > 0) {
        zstd_enc_bw_add_bits(&bw, of_extra_val, of_nb_extra);
      }
    }
    else {
      // Not last sequence: output state update bits first (decoder reads last),
      // then extra bits
      //
      // Decoder reads: OF_extra, ML_extra, LL_extra, LL_bits, ML_bits, OF_bits
      // Encoder adds reverse: OF_bits, ML_bits, LL_bits, LL_extra, ML_extra,
      // OF_extra

      uint8_t bits_out, nb_bits_out;

      // OF state update (decoder reads last among state updates)
      uint16_t new_of_state = zstd_enc_find_encode_state(
          of_table, 32, of_code, enc_state_of, &bits_out, &nb_bits_out);
      if (new_of_state == 0xFFFF) {
        return GCOMP_ERR_CORRUPT; // Should not happen
      }
      if (nb_bits_out > 0) {
        zstd_enc_bw_add_bits(&bw, bits_out, nb_bits_out);
      }
      enc_state_of = new_of_state;

      // ML state update
      uint16_t new_ml_state = zstd_enc_find_encode_state(
          ml_table, 64, ml_code, enc_state_ml, &bits_out, &nb_bits_out);
      if (new_ml_state == 0xFFFF) {
        return GCOMP_ERR_CORRUPT; // Should not happen
      }
      if (nb_bits_out > 0) {
        zstd_enc_bw_add_bits(&bw, bits_out, nb_bits_out);
      }
      enc_state_ml = new_ml_state;

      // LL state update (decoder reads first among state updates)
      uint16_t new_ll_state = zstd_enc_find_encode_state(
          ll_table, 64, ll_code, enc_state_ll, &bits_out, &nb_bits_out);
      if (new_ll_state == 0xFFFF) {
        return GCOMP_ERR_CORRUPT; // Should not happen
      }
      if (nb_bits_out > 0) {
        zstd_enc_bw_add_bits(&bw, bits_out, nb_bits_out);
      }
      enc_state_ll = new_ll_state;

      // Now add extra bits (decoder reads first among all bits for this seq)
      // Decoder reads: OF, ML, LL
      // Encoder adds in reverse: LL, ML, OF
      if (ll_nb_extra > 0) {
        zstd_enc_bw_add_bits(&bw, ll_extra_val, ll_nb_extra);
      }
      if (ml_nb_extra > 0) {
        zstd_enc_bw_add_bits(&bw, ml_extra_val, ml_nb_extra);
      }
      if (of_nb_extra > 0) {
        zstd_enc_bw_add_bits(&bw, of_extra_val, of_nb_extra);
      }
    }
  }

  // Add initial states (decoder reads: LL, OF, ML)
  // Encoder writes in reverse: ML, OF, LL
  zstd_enc_bw_add_bits(&bw, enc_state_ml, 6);
  zstd_enc_bw_add_bits(&bw, enc_state_of, 5);
  zstd_enc_bw_add_bits(&bw, enc_state_ll, 6);

  // Close bitstream - adds marker at HIGH end and flushes
  size_t bitstream_size = zstd_enc_bw_close(&bw);

  *output_len_out = pos + bitstream_size;
  return GCOMP_OK;
}
