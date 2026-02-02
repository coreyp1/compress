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
 * ## Predefined Tables
 *
 * Zstd defines predefined FSE tables for sequences that provide good
 * compression for typical data without needing to transmit custom tables.
 * The predefined distributions are fixed by the specification.
 *
 * Reference: RFC 8878 (https://www.rfc-editor.org/rfc/rfc8878.html)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

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

  zstd_fwd_bit_reader_t br;
  gcomp_status_t status = zstd_fwd_bit_reader_init(&br, src, src_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // Read accuracy log (4 bits) + 5 = actual log
  status = zstd_fwd_bit_reader_ensure(&br, 4);
  if (status != GCOMP_OK) {
    return status;
  }

  unsigned accuracy_log = zstd_fwd_bit_reader_read(&br, 4) + FSE_MIN_TABLE_LOG;
  if (accuracy_log > FSE_MAX_ACCURACY_LOG) {
    return GCOMP_ERR_CORRUPT;
  }

  int remaining = 1 << accuracy_log;
  unsigned symbol = 0;
  unsigned max_symbol = 0;

  // Initialize counts to 0
  memset(norm_counts, 0, (FSE_MAX_SYMBOL_VALUE + 1) * sizeof(int16_t));

  while (remaining > 0 && symbol <= FSE_MAX_SYMBOL_VALUE) {
    // Determine number of bits needed to represent remaining
    unsigned threshold = remaining + 1;
    unsigned nb_bits;
    if (threshold > 1) {
      nb_bits = 32 - __builtin_clz(threshold - 1);
    }
    else {
      nb_bits = 1;
    }

    status = zstd_fwd_bit_reader_ensure(&br, nb_bits);
    if (status != GCOMP_OK) {
      return status;
    }

    // Read using variable-length encoding
    unsigned low_bits = nb_bits - 1;
    unsigned low_value = zstd_fwd_bit_reader_read(&br, low_bits);

    // Calculate the cutoff
    unsigned small_max = (1U << nb_bits) - 1 - threshold;

    int count;
    if (low_value < small_max) {
      count = (int)low_value;
    }
    else {
      status = zstd_fwd_bit_reader_ensure(&br, 1);
      if (status != GCOMP_OK) {
        return status;
      }
      unsigned extra_bit = zstd_fwd_bit_reader_read(&br, 1);
      count = (int)(low_value + (extra_bit << low_bits) - small_max);
    }

    // Convert to probability: count - 1
    // count = 0 means prob = -1 (less than 1)
    // count = 1 means prob = 0 (symbol not present)
    count--;

    if (count == -1) {
      // Less than 1 probability
      remaining--;
    }
    else if (count >= 0) {
      remaining -= count;
      if (remaining < 0) {
        return GCOMP_ERR_CORRUPT;
      }
    }

    norm_counts[symbol] = (int16_t)count;
    if (count != 0) {
      max_symbol = symbol;
    }

    symbol++;

    // Check for repeat zeroes
    if (count == 0) {
      status = zstd_fwd_bit_reader_ensure(&br, 2);
      // It's okay if we don't have 2 bits at end
      if (status == GCOMP_OK) {
        unsigned repeat = zstd_fwd_bit_reader_read(&br, 2);
        while (repeat == 3 && symbol <= FSE_MAX_SYMBOL_VALUE) {
          norm_counts[symbol++] = 0;
          norm_counts[symbol++] = 0;
          norm_counts[symbol++] = 0;
          status = zstd_fwd_bit_reader_ensure(&br, 2);
          if (status != GCOMP_OK) {
            break;
          }
          repeat = zstd_fwd_bit_reader_read(&br, 2);
        }
        if (repeat > 0 && repeat < 3) {
          while (repeat > 0 && symbol <= FSE_MAX_SYMBOL_VALUE) {
            norm_counts[symbol++] = 0;
            repeat--;
          }
        }
      }
    }
  }

  if (remaining != 0) {
    return GCOMP_ERR_CORRUPT;
  }

  *max_symbol_out = max_symbol;
  *table_log_out = accuracy_log;
  *bytes_read_out = br.byte_pos;

  return GCOMP_OK;
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
static const int16_t zstd_ll_predefined_norm[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1,
    -1, -1};

// Match Length predefined distribution (accuracy log = 6)
static const int16_t zstd_ml_predefined_norm[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};

// Offset predefined distribution (accuracy log = 5)
static const int16_t zstd_of_predefined_norm[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2,
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
