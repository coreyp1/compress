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

#include <ghoti.io/compress/macros.h>
#include "zstd_internal.h"
#include <string.h>
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
  bool overflow;          ///< Set once a write did not fit
} zstd_enc_bit_writer_t;

static void zstd_enc_bw_init(
    zstd_enc_bit_writer_t * bw, uint8_t * buf, size_t buf_size) {
  bw->buf = buf;
  bw->buf_size = buf_size;
  bw->byte_pos = 0;
  bw->bit_container = 0;
  bw->bits_used = 0;
  bw->overflow = false;
}

/**
 * @brief Flush complete bytes from the bit container.
 *
 * Writes LOW bytes to increasing addresses until fewer than 8 bits remain.
 */
// A writer created with buf == NULL measures instead of writing, so a
// candidate encoding can be priced without a buffer to hold it.
static void zstd_enc_bw_flush(zstd_enc_bit_writer_t * bw) {
  while (bw->bits_used >= 8) {
    if (bw->buf && bw->byte_pos >= bw->buf_size) {
      // Out of room.  Record it: the caller decides whether to fall back,
      // and must not be handed a silently truncated bitstream.
      bw->overflow = true;
      return;
    }
    if (bw->buf) {
      bw->buf[bw->byte_pos] = (uint8_t)(bw->bit_container & 0xFF);
    }
    bw->byte_pos++;
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
  for (unsigned i = 0; i < bytes_needed; i++) {
    if (bw->buf && bw->byte_pos >= bw->buf_size) {
      bw->overflow = true;
      break;
    }
    if (bw->buf) {
      bw->buf[bw->byte_pos] = (uint8_t)(bw->bit_container & 0xFF);
    }
    bw->byte_pos++;
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
// The transition value is next_state - base, which spans the whole table
// entry's range: up to 2^nb_bits - 1, and nb_bits reaches 9 for the literal
// length and match length tables (RFC 8878 3.1.1.3.2.1).  Returning it in a
// uint8_t silently truncated it at Accuracy_Log 9 -- which only per-block
// tables ever reach, since the predefined ones are log 6, 5 and 6.

//
// Public API: Encode Sequences
//

/**
 * @brief One symbol type's table for the sequence encoder.
 *
 * `mode` is the Symbol_Compression_Mode written into the modes byte
 * (RFC 8878 3.1.1.3.2): 0 Predefined, 1 RLE, 2 FSE_Compressed.  `entries`
 * and `log` drive the state machine in every mode; an RLE table is a single
 * entry with nb_bits 0 and log 0, which naturally emits no bits at all.
 */
#define ZSTD_SEQ_MAX_TABLE_SIZE (1u << 9)
#define ZSTD_SEQ_MAX_CODES ZSTD_SEQ_ML_CODES

// One symbol of normalized count n needs fewer than 2n state-block slots in
// the index below, so every symbol together needs fewer than twice the
// table size.  The derivation is with zstd_seq_index_table().
#define ZSTD_SEQ_ENC_MAP_SIZE (2u * ZSTD_SEQ_MAX_TABLE_SIZE)

typedef struct {
  const zstd_fse_entry_t * entries;
  size_t size;
  unsigned log;
  unsigned mode;
  uint8_t rle_symbol;
  const int16_t * norm; ///< Normalized counts, mode 2 only.
  unsigned max_symbol;  ///< Highest symbol in `norm`, mode 2 only.

  // Index over `entries` for the encoder's reverse state search, described
  // in full above zstd_seq_index_table().  `map` names the covering entry
  // for every aligned block of states, `map_start` and `map_shift` say
  // where one symbol's blocks begin and how wide they are, and `count` is
  // zero for a symbol the table does not carry at all.
  uint16_t map[ZSTD_SEQ_ENC_MAP_SIZE];
  uint16_t map_start[ZSTD_SEQ_MAX_CODES + 1];
  uint16_t count[ZSTD_SEQ_MAX_CODES + 1];
  uint8_t map_shift[ZSTD_SEQ_MAX_CODES + 1];
} zstd_seq_enc_table_t;

// Fill in the per-symbol index described above.
//
// WHY A PLAIN ARRAY IS ENOUGH
// ===========================
//
// The decoding table gives each state entry a symbol, an nb_bits and a
// new_state, built (zstd_fse.c, and RFC 8878 4.1.1) as
//
//   nb_bits   = table_log - floor(log2(next))
//   new_state = (next << nb_bits) - table_size
//
// where `next` counts a symbol's occurrences and runs over [n, 2n-1] for a
// symbol of normalized count n.  The encoder has to run that backwards: given
// the state the decoder must end up in, which entry of this symbol takes it
// there?  That entry is the one whose range [new_state, new_state + 2^nb_bits)
// contains the target, and those ranges tile [0, table_size) exactly.
//
// Two facts about the formula make the search unnecessary.  First, table_size
// is 2^table_log and nb_bits <= table_log, so table_size is a multiple of
// 2^nb_bits -- and so, therefore, is new_state.  Every range is a power-of-two
// block *aligned* to its own width.  Second, floor(log2(next)) takes only two
// values across [n, 2n-1], so one symbol's ranges have only two widths, the
// wider being twice the narrower.
//
// Aligned blocks of one of two widths tile the state space, so indexing by
// the narrower width lands in exactly one block: the covering entry for a
// target state is map[map_start[sym] + (target >> map_shift[sym])], where
// 2^map_shift is that symbol's narrowest range.  A wide block simply fills
// two adjacent slots.
//
// The cost is bounded.  With h = floor(log2(n)), a symbol whose count is a
// power of two has one width and takes n slots; any other takes 2^(h+1) < 2n.
// Summed over the symbols that is under 2 * table_size, which is the size of
// `map`.
//
// This replaced a bisection over the symbol's entries sorted by new_state,
// which in turn had replaced a scan of the whole table.  At three lookups per
// sequence the bisection was still the single largest cost in encoding --
// 45% of it, with the midpoint calculation alone at 6.5%.
static void zstd_seq_index_table(zstd_seq_enc_table_t * t) {
  memset(t->count, 0, sizeof(t->count));

  // Narrowest range per symbol, as a shift.  A symbol absent from the table
  // keeps count 0 and is never indexed.
  uint8_t min_bits[ZSTD_SEQ_MAX_CODES + 1];
  memset(min_bits, 0xFF, sizeof(min_bits));

  for (size_t i = 0; i < t->size; i++) {
    unsigned sym = t->entries[i].symbol;
    if (sym > ZSTD_SEQ_MAX_CODES) {
      sym = ZSTD_SEQ_MAX_CODES;
    }
    t->count[sym]++;
    if (t->entries[i].nb_bits < min_bits[sym]) {
      min_bits[sym] = t->entries[i].nb_bits;
    }
  }

  unsigned running = 0;
  for (unsigned sym = 0; sym <= ZSTD_SEQ_MAX_CODES; sym++) {
    t->map_start[sym] = (uint16_t)running;
    if (t->count[sym] == 0) {
      t->map_shift[sym] = 0;
      continue;
    }
    t->map_shift[sym] = min_bits[sym];
    unsigned slots = (unsigned)(t->size >> min_bits[sym]);
    if (running + slots > ZSTD_SEQ_ENC_MAP_SIZE) {
      // Unreachable given the bound derived above; the table is left short
      // rather than written past, and the lookup reports the miss.
      t->count[sym] = 0;
      continue;
    }
    running += slots;
  }

  for (size_t i = 0; i < t->size; i++) {
    unsigned sym = t->entries[i].symbol;
    if (sym > ZSTD_SEQ_MAX_CODES) {
      sym = ZSTD_SEQ_MAX_CODES;
    }
    if (t->count[sym] == 0) {
      continue;
    }
    unsigned shift = t->map_shift[sym];
    unsigned base = t->map_start[sym] + (t->entries[i].new_state >> shift);
    unsigned slots = 1u << (t->entries[i].nb_bits - shift);
    for (unsigned k = 0; k < slots; k++) {
      t->map[base + k] = (uint16_t)i;
    }
  }
}

// The entry of `symbol` whose transition range contains `next_state`.
static uint16_t zstd_seq_lookup_encode_state(const zstd_seq_enc_table_t * t,
    uint8_t symbol, uint16_t next_state, uint16_t * bits_out,
    uint8_t * nb_bits_out) {
  unsigned sym = (symbol > ZSTD_SEQ_MAX_CODES) ? ZSTD_SEQ_MAX_CODES : symbol;
  if (t->count[sym] == 0) {
    *bits_out = 0;
    *nb_bits_out = 0;
    return 0xFFFF;
  }

  uint16_t idx = t->map[t->map_start[sym] + (next_state >> t->map_shift[sym])];
  *bits_out = (uint16_t)(next_state - t->entries[idx].new_state);
  *nb_bits_out = t->entries[idx].nb_bits;
  return idx;
}

// Any state carrying `symbol`; used for the last sequence, which has no
// transition out of it.  One symbol's ranges tile [0, size), so the first
// slot of its map is the entry whose range starts at state zero.
static uint16_t zstd_seq_lookup_any_state(
    const zstd_seq_enc_table_t * t, uint8_t symbol) {
  unsigned sym = (symbol > ZSTD_SEQ_MAX_CODES) ? ZSTD_SEQ_MAX_CODES : symbol;
  return (t->count[sym] == 0) ? 0 : t->map[t->map_start[sym]];
}

/**
 * @brief Write the sequences section using the given three tables.
 *
 * Pass output == NULL to measure the encoded size without writing it.
 */
static gcomp_status_t zstd_sequences_encode_using(
    const zstd_sequence_t * sequences, size_t num_sequences,
    const zstd_seq_enc_table_t * ll_t, const zstd_seq_enc_table_t * of_t,
    const zstd_seq_enc_table_t * ml_t, uint8_t * output, size_t output_cap,
    size_t * output_len_out) {
  if (!output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const bool measuring = (output == NULL);
  size_t pos = 0;

  // Writing a byte, or just counting it when measuring.
#define SEQ_PUT(byte)                                                          \
  do {                                                                         \
    if (!measuring) {                                                          \
      if (pos >= output_cap) {                                                 \
        return GCOMP_ERR_LIMIT;                                                \
      }                                                                        \
      output[pos] = (uint8_t)(byte);                                           \
    }                                                                          \
    pos++;                                                                     \
  } while (0)

  if (num_sequences == 0 || !sequences) {
    SEQ_PUT(0);
    *output_len_out = pos;
    return GCOMP_OK;
  }

  // 1. Number_of_Sequences (RFC 8878 3.1.1.3.2.1)
  if (num_sequences < 128) {
    SEQ_PUT(num_sequences);
  }
  else if (num_sequences < 0x7F00) {
    SEQ_PUT((num_sequences >> 8) + 128);
    SEQ_PUT(num_sequences & 0xFF);
  }
  else {
    uint32_t val = (uint32_t)num_sequences - 0x7F00;
    SEQ_PUT(255);
    SEQ_PUT(val & 0xFF);
    SEQ_PUT((val >> 8) & 0xFF);
  }

  // 2. Symbol_Compression_Modes: literals lengths, offsets, match lengths.
  SEQ_PUT((ll_t->mode << 6) | (of_t->mode << 4) | (ml_t->mode << 2));

  // 3. Table descriptions, in the order the modes byte names them.  A
  //    predefined table has none; an RLE table is the single symbol.
  const zstd_seq_enc_table_t * order[3] = {ll_t, of_t, ml_t};
  for (unsigned t = 0; t < 3; t++) {
    const zstd_seq_enc_table_t * tbl = order[t];
    if (tbl->mode == 1) {
      SEQ_PUT(tbl->rle_symbol);
    }
    else if (tbl->mode == 2) {
      size_t written = 0;
      if (measuring) {
        // Descriptions are small; write into a scratch to learn the size.
        uint8_t scratch[128];
        gcomp_status_t st = zstd_fse_write_table_header(scratch,
            sizeof(scratch), tbl->norm, tbl->max_symbol, tbl->log, &written);
        if (st != GCOMP_OK) {
          return st;
        }
      }
      else {
        gcomp_status_t st = zstd_fse_write_table_header(output + pos,
            output_cap - pos, tbl->norm, tbl->max_symbol, tbl->log, &written);
        if (st != GCOMP_OK) {
          return st;
        }
      }
      pos += written;
    }
  }

#undef SEQ_PUT

  if (!measuring && pos >= output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  zstd_enc_bit_writer_t bw;
  zstd_enc_bw_init(&bw, measuring ? NULL : output + pos,
      measuring ? SIZE_MAX : output_cap - pos);

  // The decoder reads, per sequence: OF/ML/LL extra bits, then LL/ML/OF state
  // updates.  The encoder walks the sequences backwards and appends bits in
  // the reverse of the read order, so the last thing appended is the first
  // thing read.
  uint16_t enc_state_ll = 0;
  uint16_t enc_state_of = 0;
  uint16_t enc_state_ml = 0;

  for (size_t idx = num_sequences; idx > 0; idx--) {
    size_t i = idx - 1;
    const zstd_sequence_t * seq = &sequences[i];

    uint8_t ll_code = zstd_enc_get_ll_code(seq->lit_length);
    uint8_t ml_code = zstd_enc_get_ml_code(seq->match_length);
    uint8_t of_code = zstd_enc_get_of_code(seq->match_offset);

    uint32_t ll_extra_val = seq->lit_length - zstd_seq_ll_baseline[ll_code];
    uint32_t ml_extra_val = seq->match_length - zstd_seq_ml_baseline[ml_code];
    uint32_t of_extra_val =
        (of_code > 0) ? seq->match_offset - (1U << of_code) : 0;

    unsigned ll_nb_extra = zstd_seq_ll_extra_bits[ll_code];
    unsigned ml_nb_extra = zstd_seq_ml_extra_bits[ml_code];
    unsigned of_nb_extra = of_code;

    if (i == num_sequences - 1) {
      // Last sequence: no state updates follow it, so any state carrying the
      // symbol will do.
      enc_state_ll = zstd_seq_lookup_any_state(ll_t, ll_code);
      enc_state_of = zstd_seq_lookup_any_state(of_t, of_code);
      enc_state_ml = zstd_seq_lookup_any_state(ml_t, ml_code);
    }
    else {
      uint16_t bits_out;
      uint8_t nb_bits_out;

      uint16_t new_of_state = zstd_seq_lookup_encode_state(
          of_t, of_code, enc_state_of, &bits_out, &nb_bits_out);
      if (new_of_state == 0xFFFF) {
        return GCOMP_ERR_CORRUPT;
      }
      zstd_enc_bw_add_bits(&bw, bits_out, nb_bits_out);
      enc_state_of = new_of_state;

      uint16_t new_ml_state = zstd_seq_lookup_encode_state(
          ml_t, ml_code, enc_state_ml, &bits_out, &nb_bits_out);
      if (new_ml_state == 0xFFFF) {
        return GCOMP_ERR_CORRUPT;
      }
      zstd_enc_bw_add_bits(&bw, bits_out, nb_bits_out);
      enc_state_ml = new_ml_state;

      uint16_t new_ll_state = zstd_seq_lookup_encode_state(
          ll_t, ll_code, enc_state_ll, &bits_out, &nb_bits_out);
      if (new_ll_state == 0xFFFF) {
        return GCOMP_ERR_CORRUPT;
      }
      zstd_enc_bw_add_bits(&bw, bits_out, nb_bits_out);
      enc_state_ll = new_ll_state;
    }

    zstd_enc_bw_add_bits(&bw, ll_extra_val, ll_nb_extra);
    zstd_enc_bw_add_bits(&bw, ml_extra_val, ml_nb_extra);
    zstd_enc_bw_add_bits(&bw, of_extra_val, of_nb_extra);
  }

  // Initial states, read LL then OF then ML, so written in reverse.
  zstd_enc_bw_add_bits(&bw, enc_state_ml, ml_t->log);
  zstd_enc_bw_add_bits(&bw, enc_state_of, of_t->log);
  zstd_enc_bw_add_bits(&bw, enc_state_ll, ll_t->log);

  size_t bitstream_size = zstd_enc_bw_close(&bw);
  if (bw.overflow) {
    return GCOMP_ERR_LIMIT;
  }

  *output_len_out = pos + bitstream_size;
  return GCOMP_OK;
}

/**
 * @brief Build a per-block FSE table for one sequence symbol type.
 *
 * Returns GCOMP_OK with mode 1 (RLE) when the block uses a single symbol, or
 * mode 2 with a table normalized to a per-block distribution.  Any other
 * status means the caller should fall back to the predefined table.
 */
static gcomp_status_t zstd_seq_build_custom_table(const uint32_t * freq,
    unsigned num_codes, size_t num_sequences, unsigned max_table_log,
    int16_t * norm_out, zstd_fse_entry_t * entries, size_t entries_cap,
    zstd_seq_enc_table_t * out) {
  unsigned max_symbol = 0;
  unsigned distinct = 0;
  for (unsigned i = 0; i < num_codes; i++) {
    if (freq[i]) {
      max_symbol = i;
      distinct++;
    }
  }
  if (distinct == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (distinct == 1) {
    // RLE_Mode: one byte of description and no bits in the stream.
    entries[0].symbol = (uint8_t)max_symbol;
    entries[0].nb_bits = 0;
    entries[0].new_state = 0;
    out->entries = entries;
    out->size = 1;
    out->log = 0;
    out->mode = 1;
    out->rle_symbol = (uint8_t)max_symbol;
    out->norm = NULL;
    out->max_symbol = max_symbol;
    zstd_seq_index_table(out);
    return GCOMP_OK;
  }

  unsigned table_log =
      zstd_fse_optimal_table_log(max_table_log, num_sequences, max_symbol);
  if (((size_t)1 << table_log) > entries_cap) {
    return GCOMP_ERR_LIMIT;
  }

  gcomp_status_t status = zstd_fse_normalize_counts(
      freq, max_symbol, num_sequences, table_log, norm_out);
  if (status != GCOMP_OK) {
    return status;
  }

  status = zstd_fse_build_table_from_norm(
      norm_out, max_symbol, table_log, entries);
  if (status != GCOMP_OK) {
    return status;
  }

  out->entries = entries;
  out->size = (size_t)1 << table_log;
  out->log = table_log;
  out->mode = 2;
  out->rle_symbol = 0;
  out->norm = norm_out;
  out->max_symbol = max_symbol;
  zstd_seq_index_table(out);
  return GCOMP_OK;
}

/**
 * @brief Encode a block's sequences section.
 *
 * Builds a per-block FSE table for each of the three symbol types and uses
 * it when it comes out smaller than the predefined tables, which is what the
 * Symbol_Compression_Modes byte exists to allow (RFC 8878 3.1.1.3.2).  Both
 * candidates are priced by encoding them with a counting bit writer, so the
 * choice is made on real sizes rather than an estimate, and the block is only
 * written once.
 *
 * Using the predefined tables unconditionally -- which is what this did --
 * cost about 4.7 bits per sequence on a general corpus, some 170 KB across
 * 4.4 MB of input.
 */
gcomp_status_t zstd_sequences_encode(const zstd_sequence_t * sequences,
    size_t num_sequences, uint8_t * output, size_t output_cap,
    size_t * output_len_out) {
  if (!output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (num_sequences > ZSTD_MAX_NUM_SEQUENCES) {
    return GCOMP_ERR_LIMIT;
  }

  // Predefined tables: always available, and the yardstick.
  zstd_fse_entry_t pre_ll[64];
  zstd_fse_entry_t pre_of[32];
  zstd_fse_entry_t pre_ml[64];
  zstd_fse_build_predefined_ll_table(pre_ll, 64);
  zstd_fse_build_predefined_of_table(pre_of, 32);
  zstd_fse_build_predefined_ml_table(pre_ml, 64);

  zstd_seq_enc_table_t ll_pre = {0}, of_pre = {0}, ml_pre = {0};
  ll_pre.entries = pre_ll; ll_pre.size = 64; ll_pre.log = 6;
  of_pre.entries = pre_of; of_pre.size = 32; of_pre.log = 5;
  ml_pre.entries = pre_ml; ml_pre.size = 64; ml_pre.log = 6;
  zstd_seq_index_table(&ll_pre);
  zstd_seq_index_table(&of_pre);
  zstd_seq_index_table(&ml_pre);

  if (num_sequences == 0 || !sequences) {
    return zstd_sequences_encode_using(sequences, num_sequences, &ll_pre,
        &of_pre, &ml_pre, output, output_cap, output_len_out);
  }

  // Price the predefined tables without writing them.  The custom tables are
  // then written for real: when they win -- which is the common case once a
  // block has more than a handful of sequences -- that is two walks over the
  // sequences rather than three.
  size_t pre_size = 0;
  gcomp_status_t pre_status = zstd_sequences_encode_using(sequences,
      num_sequences, &ll_pre, &of_pre, &ml_pre, NULL, 0, &pre_size);

  // Per-block tables.
  uint32_t ll_freq[ZSTD_SEQ_LL_CODES] = {0};
  uint32_t ml_freq[ZSTD_SEQ_ML_CODES] = {0};
  uint32_t of_freq[ZSTD_SEQ_OF_CODES] = {0};

  for (size_t i = 0; i < num_sequences; i++) {
    uint8_t ll_code = zstd_enc_get_ll_code(sequences[i].lit_length);
    uint8_t ml_code = zstd_enc_get_ml_code(sequences[i].match_length);
    uint8_t of_code = zstd_enc_get_of_code(sequences[i].match_offset);
    if (ll_code >= ZSTD_SEQ_LL_CODES || ml_code >= ZSTD_SEQ_ML_CODES ||
        of_code >= ZSTD_SEQ_OF_CODES) {
      return GCOMP_ERR_CORRUPT;
    }
    ll_freq[ll_code]++;
    ml_freq[ml_code]++;
    of_freq[of_code]++;
  }

  static const unsigned kLlMaxLog = 9; // RFC 8878 3.1.1.3.2.1
  static const unsigned kOfMaxLog = 8;
  static const unsigned kMlMaxLog = 9;

  zstd_fse_entry_t cus_ll[1u << 9];
  zstd_fse_entry_t cus_of[1u << 8];
  zstd_fse_entry_t cus_ml[1u << 9];
  int16_t norm_ll[ZSTD_SEQ_LL_CODES];
  int16_t norm_of[ZSTD_SEQ_OF_CODES];
  int16_t norm_ml[ZSTD_SEQ_ML_CODES];

  static const zstd_seq_enc_table_t kZero = {0};
  zstd_seq_enc_table_t ll_cus = kZero, of_cus = kZero, ml_cus = kZero;
  bool custom_ok =
      zstd_seq_build_custom_table(ll_freq, ZSTD_SEQ_LL_CODES, num_sequences,
          kLlMaxLog, norm_ll, cus_ll, sizeof(cus_ll) / sizeof(cus_ll[0]),
          &ll_cus) == GCOMP_OK &&
      zstd_seq_build_custom_table(of_freq, ZSTD_SEQ_OF_CODES, num_sequences,
          kOfMaxLog, norm_of, cus_of, sizeof(cus_of) / sizeof(cus_of[0]),
          &of_cus) == GCOMP_OK &&
      zstd_seq_build_custom_table(ml_freq, ZSTD_SEQ_ML_CODES, num_sequences,
          kMlMaxLog, norm_ml, cus_ml, sizeof(cus_ml) / sizeof(cus_ml[0]),
          &ml_cus) == GCOMP_OK;

  if (custom_ok) {
    size_t cus_size = 0;
    gcomp_status_t cus_status = zstd_sequences_encode_using(sequences,
        num_sequences, &ll_cus, &of_cus, &ml_cus, output, output_cap,
        &cus_size);

    if (cus_status == GCOMP_OK &&
        (pre_status != GCOMP_OK || cus_size <= pre_size)) {
      *output_len_out = cus_size;
      return GCOMP_OK;
    }
  }

  if (pre_status != GCOMP_OK) {
    return pre_status;
  }

  return zstd_sequences_encode_using(sequences, num_sequences, &ll_pre, &of_pre,
      &ml_pre, output, output_cap, output_len_out);
}
