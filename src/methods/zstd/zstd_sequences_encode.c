/**
 * @file zstd_sequences_encode.c
 *
 * Sequences section encoder for the Ghoti.io Compress library.
 * Encodes LZ sequences (literal_length, offset, match_length) using
 * predefined FSE tables. See zstd_sequences.c for decode and format overview.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
#include "zstd_sequences_private.h"
#include <stddef.h>
#include <stdint.h>

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
    uint32_t ll_extra_val = seq->lit_length - zstd_seq_ll_baseline[ll_code];
    uint32_t ml_extra_val = seq->match_length - zstd_seq_ml_baseline[ml_code];
    uint32_t of_extra_val =
        (of_code > 0) ? seq->match_offset - (1U << of_code) : 0;

    // Number of extra bits
    unsigned ll_nb_extra = zstd_seq_ll_extra_bits[ll_code];
    unsigned ml_nb_extra = zstd_seq_ml_extra_bits[ml_code];
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
