/**
 * @file zstd_sequences.c
 *
 * Sequences section decoder for the Ghoti.io Compress library.
 *
 * ## Sequences Section Format
 *
 * The sequences section describes how to reconstruct the original data:
 * - Each sequence contains: literal length, match offset, match length
 * - Literals are copied from the literals buffer
 * - Matches are copied from previously decoded output
 *
 * ## Section Layout
 *
 * 1. Number of sequences (1-3 bytes)
 * 2. Symbol compression modes (1 byte)
 * 3. FSE tables (if not predefined/RLE)
 * 4. Compressed bitstream (FSE-encoded sequences)
 *
 * Reference:
 * https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
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
  uint64_t bit_container; ///< Loaded bits (little-endian from stream)
  unsigned bits_consumed; ///< Number of bits consumed from container
  unsigned bits_loaded;   ///< Number of valid bits in container
} zstd_seq_bit_reader_t;

static gcomp_status_t zstd_seq_bit_reader_init(
    zstd_seq_bit_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src || src_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Load up to 8 bytes into the container (little-endian)
  br->bit_container = 0;
  size_t bytes_to_load = (src_size < 8) ? src_size : 8;
  for (size_t i = 0; i < bytes_to_load; i++) {
    br->bit_container |= (uint64_t)src[i] << (i * 8);
  }

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
  br->bits_loaded = (unsigned)((src_size - 1) * 8 + marker_bit_in_byte);
  br->bits_consumed = 0;

  return GCOMP_OK;
}

static uint32_t zstd_seq_bit_reader_read(
    zstd_seq_bit_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  // Check for underflow
  if (br->bits_consumed + nb_bits > br->bits_loaded) {
    return 0; // Not enough bits
  }

  // Read bits from the top of the available range
  // We want bits from position (bits_loaded - bits_consumed - nb_bits) to
  // (bits_loaded - bits_consumed - 1)
  unsigned bit_pos = br->bits_loaded - br->bits_consumed - nb_bits;
  uint32_t value =
      (uint32_t)(br->bit_container >> bit_pos) & ((1U << nb_bits) - 1);

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
    // first)
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

    // Copy match
    if (actual_offset == 0 || actual_offset > out_pos) {
      return GCOMP_ERR_CORRUPT;
    }
    if (out_pos + match_length > dst_capacity) {
      return GCOMP_ERR_LIMIT;
    }

    // Byte-by-byte copy for overlapping matches
    size_t match_src = out_pos - actual_offset;
    for (uint32_t j = 0; j < match_length; j++) {
      dst[out_pos++] = dst[match_src++];
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
// Encoder: Bit Writer for Sequences
//
// FSE bitstreams are written in reverse order. We write bits to a buffer
// starting from the end and working backward.
//

typedef struct {
  uint8_t * buf;          ///< Output buffer (start)
  size_t buf_size;        ///< Buffer capacity
  size_t byte_pos;        ///< Current position (from end, decreasing)
  uint64_t bit_container; ///< Bit accumulator
  unsigned bits_used;     ///< Bits used in container (0-64)
} zstd_enc_bit_writer_t;

static void zstd_enc_bw_init(
    zstd_enc_bit_writer_t * bw, uint8_t * buf, size_t buf_size) {
  bw->buf = buf;
  bw->buf_size = buf_size;
  bw->byte_pos = buf_size;
  bw->bit_container = 0;
  bw->bits_used = 0;
}

static void zstd_enc_bw_add_bits(
    zstd_enc_bit_writer_t * bw, uint32_t value, unsigned nb_bits) {
  if (nb_bits == 0) {
    return;
  }
  // Add bits to the LOW end of the container
  bw->bit_container = (bw->bit_container << nb_bits) | value;
  bw->bits_used += nb_bits;
}

static void zstd_enc_bw_flush_bits(zstd_enc_bit_writer_t * bw) {
  // Flush complete bytes from the HIGH end of the container
  while (bw->bits_used >= 8 && bw->byte_pos > 0) {
    bw->byte_pos--;
    bw->bits_used -= 8;
    bw->buf[bw->byte_pos] = (uint8_t)(bw->bit_container >> bw->bits_used);
    bw->bit_container &= (1ULL << bw->bits_used) - 1;
  }
}

static size_t zstd_enc_bw_close(zstd_enc_bit_writer_t * bw) {
  // Add marker bit (the highest bit in the final byte)
  zstd_enc_bw_add_bits(bw, 1, 1);

  // Flush remaining bits (including marker)
  while (bw->bits_used > 0 && bw->byte_pos > 0) {
    bw->byte_pos--;
    unsigned bits_to_write = (bw->bits_used >= 8) ? 8 : bw->bits_used;
    bw->bits_used -= bits_to_write;
    bw->buf[bw->byte_pos] = (uint8_t)(bw->bit_container >> bw->bits_used);
    if (bw->bits_used > 0) {
      bw->bit_container &= (1ULL << bw->bits_used) - 1;
    }
    else {
      bw->bit_container = 0;
    }
  }

  // Return the final size (from byte_pos to end)
  return bw->buf_size - bw->byte_pos;
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
// Encoder: FSE Encoding Tables
//
// For predefined tables, we need encoding tables that map symbol -> state info
//

typedef struct {
  int16_t delta_nb_bits; ///< Delta for bits calculation
  uint16_t delta_state;  ///< Delta for state update
  uint32_t base_state;   ///< Base state value
} zstd_fse_enc_entry_t;

/**
 * @brief Build FSE encoding table from normalized counts.
 *
 * This creates a table that maps (symbol, state_idx) -> encoding info.
 * For each symbol, we track the delta_nb_bits, delta_state, and base values.
 */
static gcomp_status_t zstd_fse_build_encoding_table(const int16_t * norm_counts,
    unsigned max_symbol, unsigned table_log, zstd_fse_enc_entry_t * table,
    uint16_t * symbol_tt_start) {
  if (!norm_counts || !table || !symbol_tt_start) {
    return GCOMP_ERR_INVALID_ARG;
  }

  unsigned table_size = 1U << table_log;

  // Build cumulative counts
  uint16_t cumul[64]; // Max symbol + 1
  cumul[0] = 0;
  for (unsigned s = 0; s <= max_symbol; s++) {
    if (norm_counts[s] == -1) {
      cumul[s + 1] = cumul[s] + 1;
    }
    else if (norm_counts[s] > 0) {
      cumul[s + 1] = cumul[s] + (uint16_t)norm_counts[s];
    }
    else {
      cumul[s + 1] = cumul[s];
    }
  }

  // Build spread table (position -> symbol)
  uint8_t spread_table[512]; // Max table size
  unsigned high_threshold = table_size - 1;
  unsigned position = 0;
  unsigned step = (table_size >> 1) + (table_size >> 3) + 3;
  unsigned mask = table_size - 1;

  // First pass: handle -1 probability symbols
  for (unsigned s = 0; s <= max_symbol; s++) {
    if (norm_counts[s] == -1) {
      spread_table[high_threshold--] = (uint8_t)s;
    }
  }

  // Second pass: spread remaining symbols
  for (unsigned s = 0; s <= max_symbol; s++) {
    int count = norm_counts[s];
    if (count <= 0) {
      continue;
    }
    for (int i = 0; i < count; i++) {
      spread_table[position] = (uint8_t)s;
      position = (position + step) & mask;
      while (position > high_threshold) {
        position = (position + step) & mask;
      }
    }
  }

  // Build encoding table entries
  uint16_t symbol_next[64];
  for (unsigned s = 0; s <= max_symbol; s++) {
    symbol_next[s] = cumul[s];
  }

  for (unsigned state = 0; state < table_size; state++) {
    uint8_t symbol = spread_table[state];
    uint16_t idx = symbol_next[symbol]++;

    // Find nb_bits
    unsigned nb_bits;
    int count = (norm_counts[symbol] == -1) ? 1 : (int)norm_counts[symbol];
    if (count == 1) {
      nb_bits = table_log;
    }
    else {
      nb_bits = table_log - (31 - __builtin_clz((unsigned)count - 1));
    }

    // Store encoding info
    table[idx].delta_nb_bits =
        (int16_t)((table_log << 16) - ((int)nb_bits << 16));
    table[idx].delta_state = (uint16_t)((1 << nb_bits) - count);
    table[idx].base_state = (uint32_t)state;
  }

  // Record where each symbol's entries start
  for (unsigned s = 0; s <= max_symbol; s++) {
    symbol_tt_start[s] = cumul[s];
  }

  return GCOMP_OK;
}

//
// Predefined encoding tables (built at encode time)
//

// Predefined distributions (same as decoder)
static const int16_t seq_ll_predefined_norm[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1,
    -1, -1};

static const int16_t seq_ml_predefined_norm[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};

static const int16_t seq_of_predefined_norm[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

//
// Public API: Encode Sequences
//

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

  // 3. Build FSE encoding tables for predefined distributions
  // Predefined: LL log=6, OF log=5, ML log=6
  zstd_fse_enc_entry_t ll_enc_table[64];
  zstd_fse_enc_entry_t of_enc_table[32];
  zstd_fse_enc_entry_t ml_enc_table[64];
  uint16_t ll_tt_start[64];
  uint16_t of_tt_start[64];
  uint16_t ml_tt_start[64];

  zstd_fse_build_encoding_table(
      seq_ll_predefined_norm, 35, 6, ll_enc_table, ll_tt_start);
  zstd_fse_build_encoding_table(
      seq_of_predefined_norm, 28, 5, of_enc_table, of_tt_start);
  zstd_fse_build_encoding_table(
      seq_ml_predefined_norm, 52, 6, ml_enc_table, ml_tt_start);

  // 4. Encode sequences backwards into bitstream
  // We'll build the bitstream in a temporary buffer (reversed)
  size_t max_bitstream = num_sequences * 40 + 32; // Generous estimate
  if (pos + max_bitstream > output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  zstd_enc_bit_writer_t bw;
  zstd_enc_bw_init(&bw, output + pos, output_cap - pos);

  // Initial FSE states (we'll encode these first but they appear last)
  // Start with state 0 for all
  uint16_t ll_state = 0;
  uint16_t of_state = 0;
  uint16_t ml_state = 0;

  // Process sequences in REVERSE order (bitstream is written backwards)
  for (size_t i = num_sequences; i > 0; i--) {
    const zstd_sequence_t * seq = &sequences[i - 1];

    // Get codes for this sequence
    uint8_t ll_code = zstd_enc_get_ll_code(seq->lit_length);
    uint8_t ml_code = zstd_enc_get_ml_code(seq->match_length);
    uint8_t of_code = zstd_enc_get_of_code(seq->match_offset);

    // Calculate extra bits
    uint32_t ll_extra = seq->lit_length - ll_baseline[ll_code];
    uint32_t ml_extra = seq->match_length - ml_baseline[ml_code];
    uint32_t of_extra = (of_code > 0) ? seq->match_offset - (1U << of_code) : 0;

    // Write state updates (except for last sequence in reverse = first in
    // forward)
    if (i < num_sequences) {
      // Encode current states using FSE
      // OF state
      zstd_enc_bw_add_bits(&bw,
          of_state & ((1U << of_enc_table[of_state].delta_nb_bits) - 1),
          of_enc_table[of_state].delta_nb_bits);
      // ML state
      zstd_enc_bw_add_bits(&bw,
          ml_state & ((1U << ml_enc_table[ml_state].delta_nb_bits) - 1),
          ml_enc_table[ml_state].delta_nb_bits);
      // LL state
      zstd_enc_bw_add_bits(&bw,
          ll_state & ((1U << ll_enc_table[ll_state].delta_nb_bits) - 1),
          ll_enc_table[ll_state].delta_nb_bits);
    }

    // Write extra bits (in reverse order: LL, ML, OF)
    if (ll_extra_bits[ll_code] > 0) {
      zstd_enc_bw_add_bits(&bw, ll_extra, ll_extra_bits[ll_code]);
    }
    if (ml_extra_bits[ml_code] > 0) {
      zstd_enc_bw_add_bits(&bw, ml_extra, ml_extra_bits[ml_code]);
    }
    if (of_code > 0) {
      zstd_enc_bw_add_bits(&bw, of_extra, of_code);
    }

    // Update FSE states for next iteration (going backwards)
    ll_state = ll_tt_start[ll_code];
    ml_state = ml_tt_start[ml_code];
    of_state = of_tt_start[of_code];

    zstd_enc_bw_flush_bits(&bw);
  }

  // Write initial states (these appear last in the bitstream)
  zstd_enc_bw_add_bits(&bw, ml_state, 6); // ML initial state
  zstd_enc_bw_add_bits(&bw, of_state, 5); // OF initial state
  zstd_enc_bw_add_bits(&bw, ll_state, 6); // LL initial state

  // Close bitstream (adds marker and flushes)
  size_t bitstream_size = zstd_enc_bw_close(&bw);

  // Move bitstream to correct position (it was written from end)
  memmove(output + pos, output + pos + (output_cap - pos) - bitstream_size,
      bitstream_size);

  *output_len_out = pos + bitstream_size;
  return GCOMP_OK;
}
