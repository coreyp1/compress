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
 * @file zstd_fse.c
 *
 * FSE (Finite State Entropy) decoder for the Ghoti.io Compress library.
 *
 * ## Algorithm Overview
 *
 * FSE (Finite State Entropy) is an asymmetric numeral system (ANS) variant
 * that achieves near-optimal compression. It works by representing symbols
 * as state transitions, where each state encodes:
 * - The current symbol
 * - Number of bits to read for the next state
 * - Base value for computing the next state
 *
 * ### Decoding Process
 *
 * 1. Initialize state by reading `table_log` bits from the bitstream
 * 2. Look up `table[state]` to get:
 *    - `symbol`: The decoded symbol
 *    - `nb_bits`: Number of bits to read for state update
 *    - `new_state`: Base value for next state
 * 3. Read `nb_bits` from bitstream, add to `new_state` for next state
 * 4. Repeat until all symbols are decoded
 *
 * ### Encoding Process (reverse of decoding)
 *
 * 1. Process symbols in REVERSE order (last to first)
 * 2. For each symbol, find state that will decode to that symbol
 * 3. Output state update bits (difference from base)
 * 4. Output initial states at the end (read first by decoder)
 *
 * ## Table Building (Critical for Interoperability)
 *
 * FSE tables are built from normalized frequency distributions. The building
 * algorithm must match exactly between encoder and decoder:
 *
 * ### Symbol Distribution
 *
 * - Each symbol has a "count" (frequency) in the normalized distribution
 * - Count of -1 means "less than 1" probability (rare symbols)
 * - Total counts must sum to `table_size` (2^table_log)
 *
 * ### State Assignment (RFC 8878 Section 4.1.1)
 *
 * 1. **Less-than-one symbols (-1 count)**: Placed at high end of table
 *    (states table_size-1, table_size-2, ...) in the ORDER THEY APPEAR
 *    in the distribution. For predefined tables, this means INCREASING
 *    symbol order: symbol 46 gets state 63, symbol 47 gets state 62, etc.
 *
 * 2. **Regular symbols**: Distributed using spread function:
 *    - step = (table_size >> 1) + (table_size >> 3) + 3
 *    - position = (position + step) & (table_size - 1)
 *    - Skip positions already used by -1 symbols
 *
 * 3. **State transition computation**: For each state i with symbol s:
 *    - Track occurrence count for symbol s
 *    - nb_bits = table_log - floor(log2(occurrence))
 *    - new_state = (occurrence << nb_bits) - table_size
 *
 * ## Zstd Usage
 *
 * In Zstd, FSE is used for encoding/decoding:
 * - Huffman weights (to describe Huffman trees)
 * - Literal length codes (36 symbols, predefined log=6)
 * - Match length codes (53 symbols, predefined log=6)
 * - Match offset codes (29 symbols, predefined log=5)
 *
 * ## FSE Encoding (for Huffman Weights)
 *
 * When the Huffman encoder has >127 symbols, it compresses the weight
 * sequence with FSE. This module provides:
 *
 * - zstd_fse_write_table_header(): Writes the FSE table header from
 *   normalized counts (inverse of the decoder's read). Same bit format
 *   as read_table_header so external decoders can parse it.
 * - zstd_fse_build_table_from_norm(): Builds the FSE decoding table from
 *   norm_counts (used by the encoder to drive the FSE state machine when
 *   encoding the weight sequence in reverse order).
 *
 * The weight FSE alphabet is small (0..12). We use table_log = 6 (64
 * states). The encoder in zstd_huf.c encodes weights in reverse, outputs
 * initial state, then marker bit (backward bitstream).
 *
 * ## Predefined Tables
 *
 * Zstd defines predefined FSE tables for sequences that provide good
 * compression for typical data without needing to transmit custom tables.
 * The predefined distributions are fixed by the specification.
 *
 * Reference: RFC 8878 (https://www.rfc-editor.org/rfc/rfc8878.html)
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "zstd_internal.h"
#include <string.h>

//
// FSE Constants
//

#define FSE_MAX_ACCURACY_LOG 9      ///< Maximum accuracy log for FSE tables
#define FSE_MAX_SYMBOL_VALUE 255    ///< Maximum symbol value
#define FSE_MAX_TABLE_SIZE (1 << 9) ///< Maximum table size (2^9 = 512)
#define FSE_MIN_TABLE_LOG 5         ///< Minimum table log

//
// Bit Reader
//

typedef struct {
  const uint8_t * src;    ///< Source data pointer
  size_t src_size;        ///< Total source size
  size_t byte_pos;        ///< Current byte position
  uint64_t bit_container; ///< Bit accumulator
  unsigned bit_pos;       ///< Bits consumed from container (0-63)
} zstd_bit_reader_t;

/**
 * @brief Initialize a bit reader.
 *
 * The bit reader is initialized to read from the END of the buffer
 * backwards, as FSE bitstreams are written in reverse order.
 */
static gcomp_status_t zstd_bit_reader_init(
    zstd_bit_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src || src_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  br->src = src;
  br->src_size = src_size;

  // Find the last set bit (initialization marker)
  // FSE streams end with a '1' bit marker
  size_t last_byte_idx = src_size - 1;
  uint8_t last_byte = src[last_byte_idx];

  if (last_byte == 0) {
    // Invalid: last byte must have at least one bit set
    return GCOMP_ERR_CORRUPT;
  }

  // Count leading zeros to find the marker bit
  unsigned marker_pos = 7;
  while (marker_pos > 0 && ((last_byte >> marker_pos) & 1) == 0) {
    marker_pos--;
  }

  // Initialize container with available bytes (read backwards)
  br->bit_container = 0;
  br->bit_pos = 0;

  // Load initial bits (up to 8 bytes from end)
  size_t bytes_to_load = (src_size < 8) ? src_size : 8;
  br->byte_pos = src_size - bytes_to_load;

  for (size_t i = 0; i < bytes_to_load; i++) {
    br->bit_container |= (uint64_t)src[br->byte_pos + i] << (i * 8);
  }

  // Skip the marker bit
  br->bit_pos =
      (bytes_to_load * 8) - ((last_byte_idx - br->byte_pos) * 8 + marker_pos);

  return GCOMP_OK;
}

/**
 * @brief Reload bits into the container.
 */
static void zstd_bit_reader_reload(zstd_bit_reader_t * br) {
  if (br->bit_pos >= 8 && br->byte_pos > 0) {
    // Shift out consumed bytes
    unsigned bytes_consumed = br->bit_pos / 8;
    if (bytes_consumed > br->byte_pos) {
      bytes_consumed = (unsigned)br->byte_pos;
    }

    // Load new bytes from the front
    br->bit_container >>= (bytes_consumed * 8);
    br->bit_pos -= bytes_consumed * 8;

    for (unsigned i = 0; i < bytes_consumed && br->byte_pos > 0; i++) {
      br->byte_pos--;
      br->bit_container |= (uint64_t)br->src[br->byte_pos] << (56 - i * 8);
    }
  }
}

/**
 * @brief Read bits from the stream.
 */
static uint32_t zstd_bit_reader_read(zstd_bit_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  zstd_bit_reader_reload(br);

  uint32_t result =
      (uint32_t)(br->bit_container >> br->bit_pos) & ((1U << nb_bits) - 1);
  br->bit_pos += nb_bits;

  return result;
}

/**
 * @brief Peek bits without consuming.
 */
static uint32_t zstd_bit_reader_peek(zstd_bit_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  zstd_bit_reader_reload(br);

  return (uint32_t)(br->bit_container >> br->bit_pos) & ((1U << nb_bits) - 1);
}

//
// Forward Bit Reader (for table headers)
//

typedef struct {
  const uint8_t * src;     ///< Source data pointer
  size_t src_size;         ///< Total source size
  size_t byte_pos;         ///< Current byte position
  uint64_t bit_container;  ///< Bit accumulator
  unsigned bits_available; ///< Bits available in container
} zstd_fwd_bit_reader_t;

/**
 * @brief Initialize a forward bit reader.
 */
static gcomp_status_t zstd_fwd_bit_reader_init(
    zstd_fwd_bit_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src) {
    return GCOMP_ERR_INVALID_ARG;
  }

  br->src = src;
  br->src_size = src_size;
  br->byte_pos = 0;
  br->bit_container = 0;
  br->bits_available = 0;

  return GCOMP_OK;
}

/**
 * @brief Ensure at least n bits are available.
 */
static gcomp_status_t zstd_fwd_bit_reader_ensure(
    zstd_fwd_bit_reader_t * br, unsigned nb_bits) {
  while (br->bits_available < nb_bits && br->byte_pos < br->src_size) {
    br->bit_container |= (uint64_t)br->src[br->byte_pos] << br->bits_available;
    br->byte_pos++;
    br->bits_available += 8;
  }

  if (br->bits_available < nb_bits) {
    return GCOMP_ERR_CORRUPT;
  }

  return GCOMP_OK;
}

/**
 * @brief Read bits from forward bit reader.
 */
static uint32_t zstd_fwd_bit_reader_read(
    zstd_fwd_bit_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  uint32_t result = (uint32_t)(br->bit_container & ((1ULL << nb_bits) - 1));
  br->bit_container >>= nb_bits;
  br->bits_available -= nb_bits;

  return result;
}

//
// Forward Bit Writer (for FSE table header encoding)
//

typedef struct {
  uint8_t * buf;
  size_t buf_size;
  size_t byte_pos;
  uint64_t bit_container;
  unsigned bits_used;
} zstd_fwd_bit_writer_t;

static void zstd_fwd_bit_writer_init(
    zstd_fwd_bit_writer_t * bw, uint8_t * buf, size_t buf_size) {
  bw->buf = buf;
  bw->buf_size = buf_size;
  bw->byte_pos = 0;
  bw->bit_container = 0;
  bw->bits_used = 0;
}

static gcomp_status_t zstd_fwd_bit_writer_write(
    zstd_fwd_bit_writer_t * bw, uint32_t value, unsigned nb_bits) {
  if (nb_bits == 0) {
    return GCOMP_OK;
  }
  // Keep flushing until the value fits.  Flushing a single byte is not always
  // enough: with the container near full, one byte leaves 56 bits used and a
  // 10-bit field would still run past the end of the accumulator.
  while (bw->bits_used + nb_bits > 64) {
    if (bw->bits_used < 8) {
      return GCOMP_ERR_INVALID_ARG; // nb_bits wider than the accumulator
    }
    if (bw->byte_pos >= bw->buf_size) {
      return GCOMP_ERR_LIMIT;
    }
    bw->buf[bw->byte_pos++] = (uint8_t)(bw->bit_container & 0xFF);
    bw->bit_container >>= 8;
    bw->bits_used -= 8;
  }
  bw->bit_container |= (uint64_t)value << bw->bits_used;
  bw->bits_used += nb_bits;
  return GCOMP_OK;
}

static gcomp_status_t zstd_fwd_bit_writer_flush(zstd_fwd_bit_writer_t * bw) {
  while (bw->bits_used > 0 && bw->byte_pos < bw->buf_size) {
    bw->buf[bw->byte_pos++] = (uint8_t)(bw->bit_container & 0xFF);
    bw->bit_container >>= 8;
    bw->bits_used = (bw->bits_used <= 8) ? 0 : bw->bits_used - 8;
  }
  return (bw->bits_used == 0) ? GCOMP_OK : GCOMP_ERR_LIMIT;
}

//
// FSE Table Building
//

/**
 * @brief Build FSE decoding table from normalized counts.
 *
 * This function constructs an FSE decoding table that maps state indices to
 * (symbol, nb_bits, new_state) tuples. The algorithm has three passes:
 *
 * ## Pass 1: Place "less-than-one" probability symbols
 *
 * Symbols with count == -1 have probability < 1/table_size. These are placed
 * at the HIGH end of the table (states table_size-1 down) in the order they
 * appear in the input distribution.
 *
 * **CRITICAL**: For predefined tables, symbols appear in increasing order,
 * so the FIRST -1 symbol encountered gets state (table_size-1), the second
 * gets (table_size-2), etc. This order MUST match between encoder and decoder
 * for interoperability with external zstd implementations.
 *
 * Example for ML predefined table (table_size=64, symbols 46-52 have count=-1):
 * - Symbol 46 → state 63 (first -1 encountered)
 * - Symbol 47 → state 62
 * - Symbol 48 → state 61
 * - ...
 * - Symbol 52 → state 57
 *
 * ## Pass 2: Spread remaining symbols
 *
 * Regular symbols (count > 0) are distributed using a deterministic spread
 * function that provides good mixing:
 * - step = (table_size/2) + (table_size/8) + 3 = table_size*5/8 + 3
 * - For each occurrence of symbol s: table[position].symbol = s
 * - position = (position + step) mod table_size, skipping -1 symbol positions
 *
 * ## Pass 3: Compute state transitions
 *
 * For each table entry i with symbol s (tracking occurrence number for s):
 * - nb_bits = table_log - floor(log2(occurrence_number))
 * - new_state = (occurrence_number << nb_bits) - table_size
 *
 * The decoder uses these to update state:
 *   next_state = new_state + read_bits(nb_bits)
 *
 * @param norm_counts Normalized symbol counts (-1 for less-than-one
 * probability)
 * @param max_symbol Maximum symbol value (0 to max_symbol inclusive)
 * @param table_log Log2 of table size (table has 2^table_log entries)
 * @param table Output table (must be at least 1 << table_log entries)
 * @return GCOMP_OK on success
 */
static gcomp_status_t zstd_fse_build_table(const int16_t * norm_counts,
    unsigned max_symbol, unsigned table_log, zstd_fse_entry_t * table) {
  if (!norm_counts || !table) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (table_log > FSE_MAX_ACCURACY_LOG) {
    return GCOMP_ERR_CORRUPT;
  }

  unsigned table_size = 1U << table_log;
  unsigned high_threshold = table_size - 1;

  // Symbol occurrence tracking (reused in pass 3)
  uint16_t symbol_next[FSE_MAX_SYMBOL_VALUE + 1];

  //
  // Pass 1: Place -1 probability symbols at high end of table
  //
  // RFC 8878 Section 4.1.1: "Less-than-one probability symbols are assigned
  // a single cell, starting from the end of the table... symbols are placed
  // in decreasing order [of table position], while the list is iterated in
  // its given order [increasing symbol order for predefined tables]."
  //
  for (unsigned s = 0; s <= max_symbol; s++) {
    if (norm_counts[s] == -1) {
      table[high_threshold].symbol = (uint8_t)s;
      high_threshold--;
      symbol_next[s] = 1; // -1 symbols have 1 occurrence for pass 3
    }
    else {
      symbol_next[s] = (uint16_t)(norm_counts[s] > 0 ? norm_counts[s] : 0);
    }
  }

  //
  // Pass 2: Spread regular symbols using deterministic step function
  //
  // The step value provides good distribution while being coprime to table_size
  // (since table_size is a power of 2 and step is odd).
  //
  unsigned position = 0;
  unsigned step = (table_size >> 1) + (table_size >> 3) + 3;
  unsigned mask = table_size - 1;

  for (unsigned s = 0; s <= max_symbol; s++) {
    int count = norm_counts[s];
    if (count <= 0) {
      continue; // Skip -1 and 0 count symbols
    }

    for (int i = 0; i < count; i++) {
      table[position].symbol = (uint8_t)s;

      // Advance to next position, skipping positions reserved for -1 symbols
      position = (position + step) & mask;
      while (position > high_threshold) {
        position = (position + step) & mask;
      }
    }
  }

  //
  // Pass 3: Compute nb_bits and new_state for state transitions
  //
  // For each state entry, we compute how many bits the decoder reads and
  // the base value for computing the next state.
  //
  // Formula derivation:
  // - occurrence: 1-based count of this symbol's appearances seen so far
  // - high_bit: floor(log2(occurrence)) = position of highest set bit
  // - nb_bits: bits needed = table_log - high_bit
  // - new_state: base value = (occurrence << nb_bits) - table_size
  //
  // Decoder computes: next_state = new_state + bits_read
  // This spreads states evenly across the valid range for the symbol.
  //
  for (unsigned i = 0; i < table_size; i++) {
    uint8_t s = table[i].symbol;
    uint16_t next = symbol_next[s]++;

    unsigned high_bit = (next > 0) ? (31 - __builtin_clz(next)) : 0;
    unsigned nb_bits = table_log - high_bit;
    unsigned new_state_base = (next << nb_bits) - table_size;

    table[i].nb_bits = (uint8_t)nb_bits;
    table[i].new_state = (uint16_t)new_state_base;
  }

  return GCOMP_OK;
}

//
// Public API: FSE Table Header Reading
//

gcomp_status_t zstd_fse_read_table_header(const uint8_t * src, size_t src_size,
    int16_t * norm_counts, unsigned * max_symbol_out, unsigned * table_log_out,
    size_t * bytes_read_out) {
  if (!src || !norm_counts || !max_symbol_out || !table_log_out ||
      !bytes_read_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (src_size < 1) {
    return GCOMP_ERR_CORRUPT;
  }

  // RFC 8878 section 4.1.1 (FSE Table Description).  The field width shrinks
  // as the distribution is consumed, and the spec defines that shrinking as a
  // recurrence on (threshold, nbBits) -- NOT as a width recomputed from
  // `remaining` on each round.  Recomputing it, and starting `remaining` at
  // 2^Accuracy_Log instead of 2^Accuracy_Log + 1, desynchronised the reader
  // from the bitstream after the first few symbols: the counts stayed
  // self-consistent enough to build a table, so nothing failed loudly, but
  // the table was not the one the encoder wrote.
  size_t bit_pos = 0;
  const size_t total_bits = src_size * 8;

  // Little-endian, least-significant-bit-first: bit i of the description is
  // bit (i % 8) of byte (i / 8).

  unsigned accuracy_log;
  {
    if (total_bits < 4) {
      return GCOMP_ERR_CORRUPT;
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) {
      v |= (uint32_t)((src[(bit_pos + i) >> 3] >> ((bit_pos + i) & 7)) & 1u)
          << i;
    }
    bit_pos += 4;
    accuracy_log = v + FSE_MIN_TABLE_LOG;
  }

  if (accuracy_log > FSE_MAX_ACCURACY_LOG) {
    return GCOMP_ERR_CORRUPT;
  }

  int remaining = (1 << accuracy_log) + 1;
  int threshold = 1 << accuracy_log;
  unsigned nb_bits = accuracy_log + 1;

  unsigned symbol = 0;
  bool previous0 = false;

  memset(norm_counts, 0, (FSE_MAX_SYMBOL_VALUE + 1) * sizeof(int16_t));

#define FSE_NC_PEEK(dst, count)                                                \
  do {                                                                         \
    if (bit_pos + (count) > total_bits) {                                      \
      return GCOMP_ERR_CORRUPT;                                                \
    }                                                                          \
    uint32_t v_ = 0;                                                           \
    for (unsigned i_ = 0; i_ < (count); i_++) {                                \
      size_t b_ = bit_pos + i_;                                                \
      v_ |= (uint32_t)((src[b_ >> 3] >> (b_ & 7)) & 1u) << i_;                 \
    }                                                                          \
    (dst) = v_;                                                                \
  } while (0)

  while (previous0 || remaining > 1) {
    if (symbol > FSE_MAX_SYMBOL_VALUE) {
      return GCOMP_ERR_CORRUPT;
    }

    if (previous0) {
      // A zero count is followed by 2-bit repeat groups: 0b11 means "three
      // more zeroes, keep reading", any other value ends the run.
      for (;;) {
        uint32_t repeat;
        FSE_NC_PEEK(repeat, 2);
        bit_pos += 2;
        unsigned zeros = (repeat == 3) ? 3u : (unsigned)repeat;
        for (unsigned i = 0; i < zeros; i++) {
          if (symbol > FSE_MAX_SYMBOL_VALUE) {
            return GCOMP_ERR_CORRUPT;
          }
          norm_counts[symbol++] = 0;
        }
        if (repeat != 3) {
          break;
        }
      }
      previous0 = false;
      continue;
    }

    int max = (2 * threshold - 1) - remaining;
    int count;
    {
      uint32_t low;
      FSE_NC_PEEK(low, nb_bits - 1);
      if ((int)low < max) {
        count = (int)low;
        bit_pos += nb_bits - 1;
      }
      else {
        uint32_t full;
        FSE_NC_PEEK(full, nb_bits);
        bit_pos += nb_bits;
        count = (int)full;
        if (count >= threshold) {
          count -= max;
        }
      }
    }

    count--; // a stored 0 means "probability below 1", written as -1

    int used = (count < 0) ? -count : count;
    if (used > remaining) {
      return GCOMP_ERR_CORRUPT;
    }
    remaining -= used;

    norm_counts[symbol++] = (int16_t)count;
    previous0 = (count == 0);

    while (remaining < threshold) {
      if (nb_bits == 0) {
        return GCOMP_ERR_CORRUPT;
      }
      nb_bits--;
      threshold >>= 1;
    }
  }

#undef FSE_NC_PEEK

  if (remaining != 1 || symbol == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  *max_symbol_out = symbol - 1;
  *table_log_out = accuracy_log;
  *bytes_read_out = (bit_pos + 7) / 8;

  return GCOMP_OK;
}

/**
 * @brief Write FSE table header (inverse of read_table_header).
 *
 * Encodes norm_counts into the same bit format the decoder expects.
 */
unsigned zstd_fse_optimal_table_log(
    unsigned max_table_log, size_t num_values, unsigned max_symbol) {
  // Mirrors the reference heuristic: a table much larger than the data it
  // describes spends more on its own description than it saves, and a table
  // smaller than the alphabet cannot give every present symbol a slot.
  unsigned table_log = max_table_log;

  if (num_values > 2) {
    unsigned hb = 0;
    size_t v = num_values - 1;
    while (v > 1) {
      v >>= 1;
      hb++;
    }
    if (hb > 2 && table_log > hb - 2) {
      table_log = hb - 2;
    }
  }

  // Every symbol up to max_symbol needs at least one slot.
  unsigned min_bits = 1;
  while ((1u << min_bits) < (unsigned)(max_symbol + 1)) {
    min_bits++;
  }
  min_bits++; // headroom so the distribution is not forced flat
  if (table_log < min_bits) {
    table_log = min_bits;
  }

  if (table_log < FSE_MIN_TABLE_LOG) {
    table_log = FSE_MIN_TABLE_LOG;
  }
  if (table_log > max_table_log) {
    table_log = max_table_log;
  }
  return table_log;
}

gcomp_status_t zstd_fse_normalize_counts(const uint32_t * freq,
    unsigned max_symbol, uint64_t total, unsigned table_log,
    int16_t * norm_out) {
  if (!freq || !norm_out || total == 0 || max_symbol > FSE_MAX_SYMBOL_VALUE) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (table_log < FSE_MIN_TABLE_LOG || table_log > FSE_MAX_ACCURACY_LOG) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // RFC 8878 section 4.1.1: a normalized count is the number of table slots a
  // symbol owns, and they sum to exactly 2^Accuracy_Log.  The special value -1
  // means "probability below one slot" and still consumes a slot.
  //
  // (Not to be confused with a Huffman weight, where the slot count is
  // 2^(weight-1).  Normalizing as though these were weights produces a
  // distribution that does not sum to the table size, and a table description
  // no decoder can use.)
  const int32_t table_size = (int32_t)(1u << table_log);
  int32_t remaining = table_size;

  unsigned largest = 0;
  uint32_t largest_freq = 0;
  unsigned present = 0;

  for (unsigned sym = 0; sym <= max_symbol; sym++) {
    norm_out[sym] = 0;
    if (freq[sym] == 0) {
      continue;
    }
    present++;

    uint64_t scaled =
        ((uint64_t)freq[sym] * (uint64_t)table_size + total / 2) / total;
    int16_t n = (scaled == 0) ? (int16_t)-1 : (int16_t)scaled;
    norm_out[sym] = n;
    remaining -= (n < 0) ? 1 : n;

    if (freq[sym] > largest_freq) {
      largest_freq = freq[sym];
      largest = sym;
    }
  }

  if (present == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if ((int32_t)present > table_size) {
    // The alphabet does not fit: the caller picked too small a table_log.
    return GCOMP_ERR_LIMIT;
  }

  // Reclaim any over-allocation one slot at a time from whichever symbol
  // currently holds the most, which is the least distorted by losing one.
  while (remaining < 0) {
    unsigned best = max_symbol + 1;
    int16_t best_n = 1;
    for (unsigned sym = 0; sym <= max_symbol; sym++) {
      if (norm_out[sym] > best_n) {
        best_n = norm_out[sym];
        best = sym;
      }
    }
    if (best > max_symbol) {
      return GCOMP_ERR_CORRUPT;
    }
    norm_out[best]--;
    remaining++;
  }

  // Hand any slack to the most frequent symbol.
  if (remaining > 0) {
    if (norm_out[largest] < 0) {
      // It held one slot as a below-one probability; it now holds that slot
      // plus the slack.
      norm_out[largest] = (int16_t)(1 + remaining);
    }
    else {
      norm_out[largest] = (int16_t)(norm_out[largest] + remaining);
    }
    remaining = 0;
  }

  return GCOMP_OK;
}

gcomp_status_t zstd_fse_write_table_header(uint8_t * dst, size_t dst_cap,
    const int16_t * norm_counts, unsigned max_symbol, unsigned table_log,
    size_t * bytes_written_out) {
  if (!dst || !norm_counts || !bytes_written_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (table_log < FSE_MIN_TABLE_LOG || table_log > FSE_MAX_ACCURACY_LOG) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (max_symbol > FSE_MAX_SYMBOL_VALUE) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_fwd_bit_writer_t bw;
  zstd_fwd_bit_writer_init(&bw, dst, dst_cap);

  // 4 bits: Accuracy_Log - 5
  gcomp_status_t status = zstd_fwd_bit_writer_write(
      &bw, (uint32_t)(table_log - FSE_MIN_TABLE_LOG), 4);
  if (status != GCOMP_OK) {
    return status;
  }

  // The exact inverse of zstd_fse_read_table_header(), sharing its
  // (threshold, nbBits) recurrence from RFC 8878 section 4.1.1.  This had
  // mirrored the older, incorrect reader instead -- `remaining` one low and
  // the field width recomputed each round -- so the two agreed with each
  // other and with no other implementation, and a zero run never emitted the
  // terminating group that ends it.
  int remaining = (1 << table_log) + 1;
  int threshold = 1 << table_log;
  unsigned nb_bits = table_log + 1;

  unsigned symbol = 0;
  bool previous0 = false;

  while (previous0 || remaining > 1) {
    if (previous0) {
      // Count the zeroes that follow the one already written, then emit them
      // as 2-bit groups: 0b11 means "three more, keep reading", and any other
      // value ends the run -- so a run that is a multiple of three still
      // needs a closing group of zero.
      unsigned run = 0;
      while (symbol + run <= max_symbol && norm_counts[symbol + run] == 0) {
        run++;
      }
      while (run >= 3) {
        status = zstd_fwd_bit_writer_write(&bw, 3, 2);
        if (status != GCOMP_OK) {
          return status;
        }
        symbol += 3;
        run -= 3;
      }
      status = zstd_fwd_bit_writer_write(&bw, run, 2);
      if (status != GCOMP_OK) {
        return status;
      }
      symbol += run;
      previous0 = false;
      continue;
    }

    if (symbol > max_symbol) {
      return GCOMP_ERR_CORRUPT; // counts do not add up to the table size
    }

    int count = (int)norm_counts[symbol];
    if (count < -1) {
      return GCOMP_ERR_INVALID_ARG;
    }

    // The reader recovers `count + 1`; pick the bit pattern that makes it do
    // so, matching its short/long field decision.
    unsigned value = (unsigned)(count + 1);
    int max = (2 * threshold - 1) - remaining;

    if (max > 0 && (int)value < max) {
      status = zstd_fwd_bit_writer_write(&bw, value, nb_bits - 1);
    }
    else if ((int)value < threshold) {
      // Low nb_bits-1 bits are >= max, and the full field is below threshold,
      // so the reader keeps it as-is.
      status = zstd_fwd_bit_writer_write(&bw, value, nb_bits);
    }
    else {
      // The reader subtracts max from any field at or above threshold.
      status = zstd_fwd_bit_writer_write(&bw, value + (unsigned)max, nb_bits);
    }
    if (status != GCOMP_OK) {
      return status;
    }

    remaining -= (count < 0) ? -count : count;
    if (remaining < 1) {
      return GCOMP_ERR_CORRUPT;
    }

    symbol++;
    previous0 = (count == 0);

    while (remaining < threshold) {
      if (nb_bits == 0) {
        return GCOMP_ERR_CORRUPT;
      }
      nb_bits--;
      threshold >>= 1;
    }
  }

  if (remaining != 1) {
    return GCOMP_ERR_CORRUPT;
  }

  status = zstd_fwd_bit_writer_flush(&bw);
  if (status != GCOMP_OK) {
    return status;
  }
  *bytes_written_out = bw.byte_pos;
  return GCOMP_OK;
}

/**
 * @brief Build FSE decoding table from normalized counts (for encoding use).
 */
gcomp_status_t zstd_fse_build_table_from_norm(const int16_t * norm_counts,
    unsigned max_symbol, unsigned table_log, zstd_fse_entry_t * table) {
  if (!norm_counts || !table) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return zstd_fse_build_table(norm_counts, max_symbol, table_log, table);
}

//
// Public API: FSE Decoding
//

gcomp_status_t zstd_fse_init_state(zstd_fse_state_t * state,
    const zstd_fse_entry_t * table, unsigned table_log,
    zstd_bit_reader_t * br) {
  if (!state || !table || !br) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Read initial state from bitstream
  state->state = zstd_bit_reader_read(br, table_log);
  state->table = table;
  state->table_log = table_log;

  return GCOMP_OK;
}

uint8_t zstd_fse_decode_symbol(
    zstd_fse_state_t * state, zstd_bit_reader_t * br) {
  const zstd_fse_entry_t * entry = &state->table[state->state];
  uint8_t symbol = entry->symbol;

  // Update state
  uint32_t bits = zstd_bit_reader_read(br, entry->nb_bits);
  state->state = entry->new_state + bits;

  return symbol;
}

uint8_t zstd_fse_peek_symbol(const zstd_fse_state_t * state) {
  return state->table[state->state].symbol;
}

//
// Public API: Build Complete FSE Table
//

gcomp_status_t zstd_fse_build_decoding_table(const uint8_t * src,
    size_t src_size, zstd_fse_entry_t * table, size_t table_capacity,
    unsigned * table_log_out, unsigned * max_symbol_out,
    size_t * bytes_read_out) {
  if (!src || !table || !table_log_out || !max_symbol_out || !bytes_read_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Read normalized counts from header
  int16_t norm_counts[FSE_MAX_SYMBOL_VALUE + 1];
  unsigned max_symbol;
  unsigned table_log;
  size_t header_size;

  gcomp_status_t status = zstd_fse_read_table_header(
      src, src_size, norm_counts, &max_symbol, &table_log, &header_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // Verify table capacity
  size_t required_size = (size_t)1 << table_log;
  if (table_capacity < required_size) {
    return GCOMP_ERR_LIMIT;
  }

  // Build the decoding table
  status = zstd_fse_build_table(norm_counts, max_symbol, table_log, table);
  if (status != GCOMP_OK) {
    return status;
  }

  *table_log_out = table_log;
  *max_symbol_out = max_symbol;
  *bytes_read_out = header_size;

  return GCOMP_OK;
}

//
// Predefined FSE Tables
//

// Predefined tables for sequence decoding
// These are the default FSE tables when mode = Predefined

// Literal Length predefined distribution (accuracy log = 6)
const int16_t zstd_ll_predefined_norm[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1,
    -1, -1};

// Match Length predefined distribution (accuracy log = 6)
const int16_t zstd_ml_predefined_norm[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};

// Offset predefined distribution (accuracy log = 5)
const int16_t zstd_of_predefined_norm[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

gcomp_status_t zstd_fse_build_predefined_ll_table(
    zstd_fse_entry_t * table, size_t table_capacity) {
  if (!table || table_capacity < 64) {
    return GCOMP_ERR_INVALID_ARG;
  }

  return zstd_fse_build_table(zstd_ll_predefined_norm, 35, 6, table);
}

gcomp_status_t zstd_fse_build_predefined_ml_table(
    zstd_fse_entry_t * table, size_t table_capacity) {
  if (!table || table_capacity < 64) {
    return GCOMP_ERR_INVALID_ARG;
  }

  return zstd_fse_build_table(zstd_ml_predefined_norm, 52, 6, table);
}

gcomp_status_t zstd_fse_build_predefined_of_table(
    zstd_fse_entry_t * table, size_t table_capacity) {
  if (!table || table_capacity < 32) {
    return GCOMP_ERR_INVALID_ARG;
  }

  return zstd_fse_build_table(zstd_of_predefined_norm, 28, 5, table);
}
