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

#include <ghoti.io/compress/macros.h>
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
  /**
   * First source byte the container currently holds, and whether it holds
   * anything at all.
   *
   * The container is eight bytes of the stream, and which eight depends only
   * on how far the reader has got.  Remembering which ones are loaded turns
   * the reload below into a comparison for every read that stays inside them,
   * which is nearly all of them: a sequence reads its three FSE states and
   * its extra bits from a handful of adjacent bytes.
   *
   * Without this the reader rebuilt the container from the source, one byte
   * at a time, on every single read -- about forty instructions to extract as
   * few as one bit -- and was 38.4% of a Zstandard decode.
   */
  size_t container_start_byte;
  int container_loaded;
} zstd_seq_bit_reader_t;

/**
 * @brief Reload the bit container from the source stream.
 *
 * For backward bitstreams, we read from high bit positions to low.
 * The container holds a window of bits, and we reload when needed.
 */
/**
 * @brief The source byte the container must begin at to cover the next bits.
 *
 * The stream is read backwards, so the bits wanted next are the highest ones
 * not yet consumed.  The container holds the eight bytes ending with the one
 * those bits live in -- or starts at the beginning of the stream when there
 * are fewer than eight bytes before it.
 */
static inline size_t zstd_seq_bit_reader_window(unsigned bits_remaining) {
  unsigned top_byte = (bits_remaining - 1u) / 8u;
  return (top_byte >= 7u) ? (size_t)(top_byte - 7u) : (size_t)0u;
}

/**
 * @brief Put the eight bytes beginning at @p start_byte into the container.
 *
 * Does nothing when they are already there, which is most of the time: a
 * sequence reads its three FSE states and its extra bits out of a handful of
 * adjacent bytes.
 */
static void zstd_seq_bit_reader_load(
    zstd_seq_bit_reader_t * br, size_t start_byte) {
  if (br->container_loaded && br->container_start_byte == start_byte) {
    return;
  }

  size_t bytes_to_load = br->src_size - start_byte;
  if (bytes_to_load > 8) {
    bytes_to_load = 8;
  }

  if (bytes_to_load == 8u) {
    // Byte i of the source at bit i*8 is a little-endian 64-bit read, which
    // every compiler here folds into a single load.  This used to be an
    // eight-iteration byte loop and was worth about a sixth of the decode.
    br->bit_container = gcomp_read_le64(br->src + start_byte);
  }
  else {
    // Fewer than eight bytes left in the stream; the tail goes a byte at a
    // time so that nothing past the end is read.
    br->bit_container = 0;
    for (size_t i = 0; i < bytes_to_load; i++) {
      br->bit_container |= (uint64_t)br->src[start_byte + i] << (i * 8);
    }
  }

  br->bits_in_container = (unsigned)(bytes_to_load * 8);
  br->container_start_byte = start_byte;
  br->container_loaded = 1;
}

/**
 * @brief Reload the bit container from the source stream.
 */
static void zstd_seq_bit_reader_reload(zstd_seq_bit_reader_t * br) {
  unsigned bits_remaining = br->total_bits - br->bits_consumed;
  if (bits_remaining == 0) {
    return;
  }
  zstd_seq_bit_reader_load(br, zstd_seq_bit_reader_window(bits_remaining));
}

static gcomp_status_t zstd_seq_bit_reader_init(
    zstd_seq_bit_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src || src_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  br->src = src;
  br->src_size = src_size;
  br->bit_container = 0;
  br->bits_in_container = 0;
  br->container_start_byte = 0;
  br->container_loaded = 0;

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

/**
 * @brief Read the next @p nb_bits bits, most significant first.
 *
 * This is the hottest function in a Zstandard decode -- six calls per
 * sequence, three for the FSE states and three for the extra bits -- so
 * everything it needs is worked out exactly once.  It used to compute where
 * the container starts twice, once here and once inside the reload, and then
 * find the bit offset within it with a division, a modulo, a multiply and an
 * add.  The offset is just the distance from the container's first bit:
 * (p/8 - start)*8 + p%8 is p - start*8.
 */
static uint32_t zstd_seq_bit_reader_read(
    zstd_seq_bit_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  // Not enough bits left to satisfy this read.
  if (br->bits_consumed + nb_bits > br->total_bits) {
    return 0;
  }

  const unsigned bits_remaining = br->total_bits - br->bits_consumed;
  const size_t start_byte = zstd_seq_bit_reader_window(bits_remaining);
  zstd_seq_bit_reader_load(br, start_byte);

  // The bits wanted run from next_bit_pos upwards, counting from the bottom
  // of the stream; the container starts at start_byte * 8 of that same count.
  const unsigned next_bit_pos = bits_remaining - nb_bits;
  const unsigned container_bit_pos =
      next_bit_pos - (unsigned)(start_byte * 8u);

  uint32_t value = (uint32_t)(br->bit_container >> container_bit_pos) &
      ((1U << nb_bits) - 1u);

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
      uint32_t extra =
          zstd_seq_bit_reader_read(&br, zstd_seq_ml_extra_bits[ml_code]);
      match_length = zstd_seq_ml_baseline[ml_code] + extra;
    }
    else {
      return GCOMP_ERR_CORRUPT;
    }

    // Read extra bits for literal length
    uint32_t literal_length;
    if (ll_code < 36) {
      uint32_t extra =
          zstd_seq_bit_reader_read(&br, zstd_seq_ll_extra_bits[ll_code]);
      literal_length = zstd_seq_ll_baseline[ll_code] + extra;
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

      memcpy(dst + out_pos, state->window_buffer + win_idx, run);
      out_pos += run;
      remaining_match -= run;
    }

    // The rest comes from what this block has already written.  Source and
    // destination are the same buffer, so a run stops at the distance between
    // them; past that the copy would have to repeat what it just wrote.
    while (remaining_match > 0u) {
      if (actual_offset > out_pos) {
        return GCOMP_ERR_CORRUPT; // Reaches back further than anything held.
      }
      size_t run = (actual_offset < remaining_match) ? (size_t)actual_offset
                                                     : remaining_match;
      if (actual_offset == 1u) {
        memset(dst + out_pos, dst[out_pos - 1u], run);
      }
      else {
        memcpy(dst + out_pos, dst + out_pos - actual_offset, run);
      }
      out_pos += run;
      remaining_match -= run;
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
