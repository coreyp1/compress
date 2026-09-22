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
 * @file zstd_sequences_encode.c
 *
 * Sequences section encoder for the Ghoti.io Compress library.
 * Encodes LZ sequences (literal_length, offset, match_length) using
 * predefined FSE tables. See zstd_sequences.c for decode and format overview.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "../../core/endian.h"
#ifdef GCOMP_TEST_BUILD
#include <assert.h>
#endif
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
  uint64_t measured_bits; ///< Bits offered, when buf is NULL.  See add_bits.
} zstd_enc_bit_writer_t;

static void zstd_enc_bw_init(
    zstd_enc_bit_writer_t * bw, uint8_t * buf, size_t buf_size) {
  bw->buf = buf;
  bw->buf_size = buf_size;
  bw->byte_pos = 0;
  bw->bit_container = 0;
  bw->bits_used = 0;
  bw->overflow = false;
  bw->measured_bits = 0;
}

/**
 * @brief Flush complete bytes from the bit container.
 *
 * Writes LOW bytes to increasing addresses until fewer than 8 bits remain.
 *
 * The container holds at most 64 bits, so a flush has up to seven whole
 * bytes to write and they are the low bytes of one 64-bit word in exactly
 * the order they go to memory.  One store puts them all there, which is why
 * the common path here is a store and not a loop: draining a byte at a time,
 * with its own bounds check each time, was 2% of encoding on its own.
 *
 * That store touches eight bytes whatever the count, so it is only taken
 * when eight bytes are free inside the buffer.  The bytes past the count are
 * not part of the stream; the next flush starts on top of them, and the
 * length this writer finally reports never includes them.  Within eight
 * bytes of the end the loop takes over, so nothing is written past the
 * buffer either way.
 */
// A writer created with buf == NULL measures instead of writing, so a
// candidate encoding can be priced without a buffer to hold it.
static void zstd_enc_bw_flush(zstd_enc_bit_writer_t * bw) {
  unsigned whole = bw->bits_used >> 3;
  if (whole == 0u) {
    return;
  }

  if (!bw->buf) {
    bw->byte_pos += whole;
  }
  else if (bw->byte_pos + 8u <= bw->buf_size) {
    gcomp_write_le64(bw->buf + bw->byte_pos, bw->bit_container);
    bw->byte_pos += whole;
  }
  else {
    for (unsigned i = 0; i < whole; i++) {
      if (bw->byte_pos >= bw->buf_size) {
        // Out of room.  Record it: the caller decides whether to fall back,
        // and must not be handed a silently truncated bitstream.
        //
        // The container is emptied rather than left holding the bits that
        // did not fit, because writing carries on after this returns -- the
        // caller is not told until it asks -- and bits that cannot be
        // flushed would otherwise accumulate.  Once bits_used passed 64 the
        // shift in zstd_enc_bw_add_bits() was undefined, which the
        // sanitizer build reports as a shift exponent of 66.  Nothing
        // written from here on is part of a stream anyone will use.
        bw->overflow = true;
        bw->bit_container = 0;
        bw->bits_used = 0;
        return;
      }
      bw->buf[bw->byte_pos] = (uint8_t)(bw->bit_container >> (i * 8u));
      bw->byte_pos++;
    }
  }

  bw->bit_container >>= (whole * 8u);
  bw->bits_used -= whole * 8u;
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
 *
 * `value` must carry no bits above `nb_bits`, which every caller satisfies:
 * an FSE transition is a target state minus the base of the range holding
 * it, an extra-bits value is its code's offset from that code's baseline
 * (RFC 8878 3.1.1.3.2.1), and an initial state is below its table size.
 * Nothing is masked here.  That also makes a zero-bit write harmless rather
 * than something to test for -- it contributes nothing and advances
 * nothing -- and zero-bit writes are common enough (three of the six calls
 * per sequence, whenever a code has no extra bits) that the test for them
 * cost 2% of encoding.
 */
static inline void zstd_enc_bw_add_bits(
    zstd_enc_bit_writer_t * bw, uint32_t value, unsigned nb_bits,
    const bool measuring) {
  // Measuring.  How long the stream comes out is settled by how many bits go
  // into it and nothing else, so the bits themselves are not assembled: the
  // container, the shifts and the flushing are all work towards bytes that
  // are never written or read.
  //
  // `measuring` is a parameter rather than a test of bw->buf so that it is a
  // constant in each caller: zstd_sequences_encode_using() is one body
  // instantiated twice (see the wrappers at the end of it), which leaves each
  // instance with no branch here at all.  Reading it from the writer instead
  // cost the writing path a perfectly predicted test on all six calls per
  // sequence -- 1,957,143 instructions on 4.7 MB of C source, 0.52% of the
  // encode, spent asking a question whose answer cannot change within one
  // writer's life.
  if (measuring) {
    bw->measured_bits += nb_bits;
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
  // Measuring: the stream is every bit offered plus the marker, rounded up
  // to whole bytes, which is exactly what the writing path below arrives at
  // by writing them.
  if (!bw->buf) {
    return (size_t)((bw->measured_bits + 1u + 7u) / 8u);
  }

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


// The three symbol-to-code conversions RFC 8878 section 3.1.1.3.2.1 defines
// are in zstd_sequences_private.h: the optimal parse prices a candidate by
// the codes it would be written under, so it needs the same mapping this
// does, and an out-of-line call per candidate length would cost more than
// the comparison chain it replaced.

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


// So the question asked once per symbol per sequence is: which state S has
// table[S].symbol == symbol and table[S].new_state <= target < new_state +
// 2^nb_bits?  zstd_seq_next_state() answers it without a search; the derivation
// is above zstd_seq_index_table().
//
// The transition value it returns spans the whole entry's range: up to
// 2^nb_bits - 1, and nb_bits reaches 9 for the literal length and match length
// tables (RFC 8878 3.1.1.3.2.1).  It was once returned in a uint8_t, which
// silently truncated it at Accuracy_Log 9 -- a width only per-block tables
// reach, since the predefined ones are log 6, 5 and 6, so the predefined path
// and every small block were unaffected and the bug hid behind them.

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

/**
 * @brief Everything the encoder needs about one symbol, in one 8-byte load.
 *
 * The inner loop asks three questions per symbol -- is it in the table, how
 * many bits does this transition cost, and which entry makes it -- and they
 * used to be three separate arrays, so three loads. Packed like this they are
 * one, and the answers come out of arithmetic rather than a second indirection.
 * The derivation is above zstd_seq_index_table().
 */
typedef struct {
  int32_t delta_nb_bits;   ///< `(state + this) >> 16` is the transition width.
  int16_t delta_find_state; ///< Added to `state >> width` to index state_table.
  uint16_t count;          ///< Entries carrying this symbol; 0 means absent.
} zstd_seq_symtt_t;

typedef struct {
  const zstd_fse_entry_t * entries;
  size_t size;
  unsigned log;
  unsigned mode;
  uint8_t rle_symbol;
  const int16_t * norm; ///< Normalized counts, mode 2 only.
  unsigned max_symbol;  ///< Highest symbol in `norm`, mode 2 only.

  // The encoder's reverse state machine, described in full above
  // zstd_seq_index_table().  `state_table` holds `size` plus an entry index,
  // which is the form the inner loop carries its state in; `min_bits` is the
  // narrowest transition a symbol has, which the predefined-table lower bound
  // in zstd_sequences_encode() needs and the loop does not.
  zstd_seq_symtt_t symtt[ZSTD_SEQ_MAX_CODES + 1];
  // Twice the largest table, of which only the first `size` entries are ever
  // filled.  The slack is not used: it bounds what a symbol the table does not
  // carry could index, which zstd_seq_covers() is what actually prevents.  See
  // the precondition on zstd_seq_next_state().
  uint16_t state_table[2u * ZSTD_SEQ_MAX_TABLE_SIZE];
  uint8_t min_bits[ZSTD_SEQ_MAX_CODES + 1];
} zstd_seq_enc_table_t;

// Fill in the per-symbol state machine described above.
//
// NO SEARCH, NO INDIRECTION: TWO NUMBERS PER SYMBOL
// =================================================
//
// The decoding table gives each state entry a symbol, an nb_bits and a
// new_state, built (zstd_fse.c, and RFC 8878 4.1.1) as
//
//   nb_bits   = table_log - floor(log2(next))
//   new_state = (next << nb_bits) - table_size
//
// where `next` counts a symbol's occurrences and runs over [n, 2n-1] for a
// symbol of normalized count n.  The encoder has to run that backwards: given
// the state S the decoder must end up in, which entry of this symbol takes it
// there?  That entry is the one whose range [new_state, new_state + 2^nb_bits)
// contains S, and one symbol's ranges tile [0, table_size) exactly.
//
// Invert the formula rather than searching for it.  Carry the state as
//
//   value = table_size + S
//
// and the second line above reads `next = value >> nb_bits` -- so if nb_bits
// were known, the entry would follow from a shift.  It is known from one
// comparison, because floor(log2(next)) takes only two values across
// [n, 2n-1].  Writing max_bits for the wider of the two widths,
//
//   nb_bits = max_bits      when value >= (n << max_bits)
//   nb_bits = max_bits - 1  otherwise
//
// and both cases come out of one addition and one shift if the threshold is
// folded into a bias:
//
//   delta_nb_bits = (max_bits << 16) - (n << max_bits)
//   nb_bits       = (value + delta_nb_bits) >> 16
//
// which works because (n << max_bits) <= 2 * table_size <= 1024, far below the
// 65536 a borrow would have to reach to move the answer by more than one.
//
// The bits to write are S - new_state, and
//
//   S - new_state = (table_size + S) - (next << nb_bits) = value mod 2^nb_bits
//
// so they are the low nb_bits of the state itself -- no second load of the
// entry to subtract from.  What remains is to name the entry: `next` indexes
// this symbol's occurrences from n, so
//
//   state_table[cumul[sym] + (next - n)]
//
// with cumul[sym] the running total of earlier symbols' counts, and the
// subtraction folded into delta_find_state = cumul[sym] - n.  state_table
// holds table_size + i rather than i, which is the form the next iteration
// wants, so one iteration ends exactly where the next begins.
//
// Two loads then, one of them dependent: the symbol's 8-byte descriptor, and
// the state_table slot it points at.  The scheme this replaced indexed a map
// of aligned state blocks -- correct, and the right first thing to build after
// a bisection, but four loads deep by three to answer the same question, and
// 4.21% of level 1 encoding on a general corpus.  Before the map it was a
// bisection over the symbol's entries, and before that a scan of the whole
// table: 45% of encoding, with the midpoint calculation alone at 6.5%.
static void zstd_seq_index_table(zstd_seq_enc_table_t * t) {
  memset(t->symtt, 0, sizeof(t->symtt));
  memset(t->min_bits, 0, sizeof(t->min_bits));
  // Zero is not a state, so an unfilled slot -- including all of the slack --
  // is one the lookup cannot follow into whatever was there before.
  memset(t->state_table, 0, sizeof(t->state_table));

  uint8_t max_bits[ZSTD_SEQ_MAX_CODES + 1];
  uint8_t low_bits[ZSTD_SEQ_MAX_CODES + 1];
  memset(max_bits, 0, sizeof(max_bits));
  memset(low_bits, 0xFF, sizeof(low_bits));

  for (size_t i = 0; i < t->size; i++) {
    unsigned sym = t->entries[i].symbol;
    if (sym > ZSTD_SEQ_MAX_CODES) {
      sym = ZSTD_SEQ_MAX_CODES;
    }
    t->symtt[sym].count++;
    if (t->entries[i].nb_bits > max_bits[sym]) {
      max_bits[sym] = t->entries[i].nb_bits;
    }
    if (t->entries[i].nb_bits < low_bits[sym]) {
      low_bits[sym] = t->entries[i].nb_bits;
    }
  }

  unsigned running = 0;
  for (unsigned sym = 0; sym <= ZSTD_SEQ_MAX_CODES; sym++) {
    const unsigned n = t->symtt[sym].count;
    if (n == 0) {
      continue;
    }
    t->min_bits[sym] = low_bits[sym];
    t->symtt[sym].delta_nb_bits =
        (int32_t)((uint32_t)max_bits[sym] << 16) - (int32_t)(n << max_bits[sym]);
    t->symtt[sym].delta_find_state = (int16_t)((int)running - (int)n);
    running += n;
  }
  // The counts are one per entry, so they sum to the table size and the slots
  // handed out above are exactly state_table's.
#ifdef GCOMP_TEST_BUILD
  assert(running == t->size);
#endif

  for (size_t i = 0; i < t->size; i++) {
    unsigned sym = t->entries[i].symbol;
    if (sym > ZSTD_SEQ_MAX_CODES) {
      sym = ZSTD_SEQ_MAX_CODES;
    }
    const unsigned nb = t->entries[i].nb_bits;
    const unsigned next =
        ((unsigned)t->entries[i].new_state + (unsigned)t->size) >> nb;
    const int slot = (int)next + t->symtt[sym].delta_find_state;
#ifdef GCOMP_TEST_BUILD
    assert(slot >= 0 && (size_t)slot < t->size);
#endif
    // Unreachable for a table zstd_fse.c built: `next` runs over [n, 2n-1] for
    // a symbol of count n, and delta_find_state subtracts that n.  A table that
    // broke the invariant would leave slots at zero, which the lookup reports
    // as a miss and the caller turns into GCOMP_ERR_CORRUPT -- a wrong answer
    // the caller can see, rather than a write past the array.
    if (slot < 0 || (size_t)slot >= t->size) {
      continue;
    }
    t->state_table[slot] = (uint16_t)(t->size + i);
  }
}

// The entry of `symbol` that leaves the decoder in the state `value` names,
// where `value` is table_size plus that state; see zstd_seq_index_table().
// Returns table_size plus the entry's own index -- the same form.
//
// PRECONDITION: the table carries `symbol`, which zstd_seq_covers() establishes
// for the whole block before any of this runs.  It used to be tested here
// instead, once per symbol per sequence -- 4.5 million instructions on 4.7 MB
// of C source to ask a question whose answer is fixed for the block, and which
// the caller has to know anyway in order to choose a table at all.  A symbol
// the table does not carry has a zeroed descriptor, which indexes the slack at
// the end of state_table and reads a zero: a wrong stream rather than a read
// past the array, and the sanitizer build asserts against it.
static inline uint16_t zstd_seq_next_state(const zstd_seq_enc_table_t * t,
    uint8_t symbol, unsigned value, uint32_t * bits_out,
    unsigned * nb_bits_out) {
  const unsigned sym =
      (symbol > ZSTD_SEQ_MAX_CODES) ? ZSTD_SEQ_MAX_CODES : symbol;
  const zstd_seq_symtt_t tt = t->symtt[sym];
#ifdef GCOMP_TEST_BUILD
  assert(tt.count != 0);
#endif
  const unsigned nb = (unsigned)(((int32_t)value + tt.delta_nb_bits) >> 16);
  *nb_bits_out = nb;
  *bits_out = value & ((1u << nb) - 1u);
  return t->state_table[(value >> nb) + tt.delta_find_state];
}

// Does this table carry every code the block uses?
//
// The encoder cannot write a symbol through a table that has no state for it,
// and the predefined tables do not reach every code (RFC 8878 3.1.1.3.2.1): a
// literal length code above 35, a match length above 52 or an offset above 28
// is outside them, and a per-block table normalized from this block's histogram
// is outside nothing.  Asked once per table per block, from the histogram the
// table was built from.
static bool zstd_seq_covers(const zstd_seq_enc_table_t * t,
    const uint32_t * freq, unsigned num_codes) {
  for (unsigned c = 0; c < num_codes && c <= ZSTD_SEQ_MAX_CODES; c++) {
    if (freq[c] != 0u && t->symtt[c].count == 0u) {
      return false;
    }
  }
  return true;
}

// A state carrying `symbol`; used for the last sequence, which has no
// transition out of it and so may start anywhere the symbol appears.  One
// symbol's ranges tile [0, size), so asking for the entry that leaves the
// decoder in state zero names one, and naming it this way rather than by a
// rule of its own keeps the choice identical to what the map-based index
// made -- the stream is byte for byte what it was.
static uint16_t zstd_seq_lookup_any_state(
    const zstd_seq_enc_table_t * t, uint8_t symbol) {
  uint32_t bits = 0;
  unsigned nb = 0;
  return zstd_seq_next_state(t, symbol, (unsigned)t->size, &bits, &nb);
}

/**
 * @brief Write the sequences section using the given three tables.
 *
 * Pass output == NULL to measure the encoded size without writing it.
 */
static inline gcomp_status_t zstd_sequences_encode_using_impl(
    const zstd_sequence_t * sequences, size_t num_sequences,
    const zstd_seq_enc_table_t * ll_t, const zstd_seq_enc_table_t * of_t,
    const zstd_seq_enc_table_t * ml_t, uint8_t * output, size_t output_cap,
    size_t * output_len_out, const bool measuring) {
  if (!output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

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
      uint32_t of_bits, ml_bits, ll_bits;
      unsigned of_nb, ml_nb, ll_nb;

      enc_state_of =
          zstd_seq_next_state(of_t, of_code, enc_state_of, &of_bits, &of_nb);
      enc_state_ml =
          zstd_seq_next_state(ml_t, ml_code, enc_state_ml, &ml_bits, &ml_nb);
      enc_state_ll =
          zstd_seq_next_state(ll_t, ll_code, enc_state_ll, &ll_bits, &ll_nb);

      // One write for all three transitions.  Appending at the high end makes
      // the first value appended the lowest bits of the stream, so packing
      // them in the same order -- OF, then ML above it, then LL above that --
      // lays down exactly the bits three separate calls did.  An accuracy log
      // is at most 9 (RFC 8878 3.1.1.3.2.1), so this is at most 27 bits and
      // cannot overflow the 32 a call carries.
      uint32_t packed = of_bits;
      unsigned packed_nb = of_nb;
      packed |= (uint32_t)ml_bits << packed_nb;
      packed_nb += ml_nb;
      packed |= (uint32_t)ll_bits << packed_nb;
      packed_nb += ll_nb;
      zstd_enc_bw_add_bits(&bw, packed, packed_nb, measuring);
    }

    // Literal and match length extra bits together: each is at most 16 bits
    // (RFC 8878 3.1.1.3.2.1), so the pair fits the 32 a call carries.  The
    // offset's are up to 31 and go on their own.
    zstd_enc_bw_add_bits(&bw, ll_extra_val | (ml_extra_val << ll_nb_extra),
        ll_nb_extra + ml_nb_extra, measuring);
    zstd_enc_bw_add_bits(&bw, of_extra_val, of_nb_extra, measuring);
  }

  // Initial states, read LL then OF then ML, so written in reverse.  The loop
  // carries a state as `size` plus an entry index (zstd_seq_index_table()); the
  // stream wants the index, in `log` bits.
  zstd_enc_bw_add_bits(
      &bw, (uint32_t)(enc_state_ml - ml_t->size), ml_t->log, measuring);
  zstd_enc_bw_add_bits(
      &bw, (uint32_t)(enc_state_of - of_t->size), of_t->log, measuring);
  zstd_enc_bw_add_bits(
      &bw, (uint32_t)(enc_state_ll - ll_t->size), ll_t->log, measuring);

  size_t bitstream_size = zstd_enc_bw_close(&bw);
  if (bw.overflow) {
    return GCOMP_ERR_LIMIT;
  }

  *output_len_out = pos + bitstream_size;
  return GCOMP_OK;
}

// The two instantiations.  Writing and measuring differ only in whether the
// bits are assembled, and every test of that would be answered the same way
// for the whole of one call; making it a template parameter in the only sense
// C has lets each instance be compiled without it.  There is still one body,
// so the two cannot drift apart.
static gcomp_status_t zstd_sequences_encode_using(
    const zstd_sequence_t * sequences, size_t num_sequences,
    const zstd_seq_enc_table_t * ll_t, const zstd_seq_enc_table_t * of_t,
    const zstd_seq_enc_table_t * ml_t, uint8_t * output, size_t output_cap,
    size_t * output_len_out) {
  if (output == NULL) {
    return zstd_sequences_encode_using_impl(sequences, num_sequences, ll_t,
        of_t, ml_t, NULL, 0, output_len_out, true);
  }
  return zstd_sequences_encode_using_impl(sequences, num_sequences, ll_t, of_t,
      ml_t, output, output_cap, output_len_out, false);
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

  // A lower bound on what the predefined tables would cost, worked out from
  // the histogram alone.
  //
  // The block is written with whichever table set is smaller, and finding
  // out used to mean encoding it twice: once with the predefined tables to
  // price them, once with its own to write.  That pricing walk was a sixth
  // of encoding, and on real data its answer is not close -- across 604
  // blocks of a general corpus at four levels the per-block tables won every
  // one of them, never by less than 12.3%.
  //
  // It does not have to be exact to settle the question.  Every part of the
  // cost except the state transitions is fixed: the extra bits a code
  // carries are the code's own (RFC 8878 3.1.1.3.2.1), the three initial
  // states cost one accuracy log each, and the marker is one bit.  A state
  // transition costs at least the narrowest range width the symbol has in
  // the table, which zstd_seq_index_table() already worked out as
  // min_bits.  All of that is a sum over the 36 or so codes rather than a
  // walk over the sequences.
  //
  // If the per-block tables come out no larger than this bound they are no
  // larger than the real thing either, and the choice is made without the
  // walk.  When the bound does not settle it, the walk still happens, below.
  uint64_t pre_lb_bits = 1u + ll_pre.log + of_pre.log + ml_pre.log;
  bool pre_lb_usable = true;
  for (unsigned c = 0; c < ZSTD_SEQ_LL_CODES; c++) {
    if (!ll_freq[c]) {
      continue;
    }
    if (!ll_pre.symtt[c].count) {
      pre_lb_usable = false; // Predefined cannot carry this code at all.
      break;
    }
    pre_lb_bits += (uint64_t)ll_freq[c] *
        (zstd_seq_ll_extra_bits[c] + ll_pre.min_bits[c]);
  }
  for (unsigned c = 0; pre_lb_usable && c < ZSTD_SEQ_ML_CODES; c++) {
    if (!ml_freq[c]) {
      continue;
    }
    if (!ml_pre.symtt[c].count) {
      pre_lb_usable = false;
      break;
    }
    pre_lb_bits += (uint64_t)ml_freq[c] *
        (zstd_seq_ml_extra_bits[c] + ml_pre.min_bits[c]);
  }
  for (unsigned c = 0; pre_lb_usable && c < ZSTD_SEQ_OF_CODES; c++) {
    if (!of_freq[c]) {
      continue;
    }
    if (!of_pre.symtt[c].count) {
      pre_lb_usable = false;
      break;
    }
    // An offset code's extra bits are the code itself
    // (RFC 8878 3.1.1.3.2.1.1).
    pre_lb_bits += (uint64_t)of_freq[c] * (c + of_pre.min_bits[c]);
  }

  // The last sequence has no successor and writes no state transitions at
  // all; the sum above counted three for it like every other.  Taking back
  // the least those three could have cost keeps the bound under the truth.
  // Without this it ran over the real size on one block in every 151, by
  // just under 1% -- enough to have chosen the wrong table.
  if (pre_lb_usable) {
    const zstd_sequence_t * last = &sequences[num_sequences - 1];
    uint8_t last_ll = zstd_enc_get_ll_code(last->lit_length);
    uint8_t last_ml = zstd_enc_get_ml_code(last->match_length);
    uint8_t last_of = zstd_enc_get_of_code(last->match_offset);
    uint64_t back = (uint64_t)ll_pre.min_bits[last_ll] +
        ml_pre.min_bits[last_ml] + of_pre.min_bits[last_of];
    pre_lb_bits = (pre_lb_bits > back) ? (pre_lb_bits - back) : 0u;
  }

  // Predefined mode writes the sequence count and the modes byte and no
  // table descriptions at all (RFC 8878 3.1.1.3.2).
  size_t pre_lb = (num_sequences < 128)
      ? 1u
      : ((num_sequences < 0x7F00) ? 2u : 3u);
  pre_lb += 1u + (size_t)((pre_lb_bits + 7u) / 8u);

#ifdef GCOMP_TEST_BUILD
  // The bound must never come out above what the walk would have found.  If
  // it did, a block would take the per-block tables when the predefined ones
  // were smaller, and nothing in the output would show it: on ordinary data
  // the per-block tables win by more than 12%, so a bound wrong by a few
  // percent still picks the same table and the stream stays byte for byte
  // what it was.  Only the ratio would move, and only on the rare block
  // where the two are close.
  //
  // So the sanitizer build prices every block both ways and checks one
  // against the other, which puts the whole test suite and the fuzz corpus
  // behind this.  It found the bound wrong once already, on one block in
  // every 151: the last sequence writes no state transitions, and the sum
  // above had counted three for it.
  if (pre_lb_usable) {
    size_t exact_pre = 0;
    if (zstd_sequences_encode_using(sequences, num_sequences, &ll_pre, &of_pre,
            &ml_pre, NULL, 0, &exact_pre) == GCOMP_OK) {
      assert(pre_lb <= exact_pre);
    }
  }
#endif

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

  // Every code this block uses has a state in the per-block tables, or they
  // cannot be used: see zstd_seq_next_state().  Normalization is supposed to
  // give every nonzero frequency a count, so this is a check on
  // zstd_seq_build_custom_table() rather than on the data, and failing it costs
  // only the fall through to the predefined tables below.
  custom_ok = custom_ok &&
      zstd_seq_covers(&ll_cus, ll_freq, ZSTD_SEQ_LL_CODES) &&
      zstd_seq_covers(&of_cus, of_freq, ZSTD_SEQ_OF_CODES) &&
      zstd_seq_covers(&ml_cus, ml_freq, ZSTD_SEQ_ML_CODES);

  if (custom_ok) {
    size_t cus_size = 0;
    gcomp_status_t cus_status = zstd_sequences_encode_using(sequences,
        num_sequences, &ll_cus, &of_cus, &ml_cus, output, output_cap,
        &cus_size);

    if (cus_status == GCOMP_OK) {
      // No larger than the bound means no larger than the real thing.
      if (pre_lb_usable && cus_size <= pre_lb) {
        *output_len_out = cus_size;
        return GCOMP_OK;
      }

      // Too close to call from the bound, so price it properly -- unless the
      // predefined tables cannot carry this block at all, which is the same
      // condition that made the bound unusable.  Pricing them then used to walk
      // the sequences and come back with GCOMP_ERR_CORRUPT, which is this
      // answer arrived at the long way.
      size_t pre_size = 0;
      gcomp_status_t pre_status = pre_lb_usable
          ? zstd_sequences_encode_using(sequences, num_sequences, &ll_pre,
                &of_pre, &ml_pre, NULL, 0, &pre_size)
          : GCOMP_ERR_CORRUPT;
      if (pre_status != GCOMP_OK || cus_size <= pre_size) {
        *output_len_out = cus_size;
        return GCOMP_OK;
      }
    }
  }

  // Predefined it is.  Writing it reports whatever pricing it would have:
  // there is nothing left to compare it against, so there is no reason to
  // walk the sequences a second time to find out first.
  //
  // Unless they do not reach every code this block uses, which leaves nothing
  // that can write it -- the per-block tables are already known to have failed
  // to get here.  The caller stores the block raw.
  if (!pre_lb_usable) {
    return GCOMP_ERR_CORRUPT;
  }
  return zstd_sequences_encode_using(sequences, num_sequences, &ll_pre, &of_pre,
      &ml_pre, output, output_cap, output_len_out);
}
