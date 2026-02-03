/**
 * @file zstd_huf.c
 *
 * Huffman encoder and decoder for the Ghoti.io Compress library.
 *
 * ## Algorithm Overview
 *
 * Huffman coding assigns variable-length codes to symbols based on their
 * frequency. More frequent symbols get shorter codes. This file implements
 * both encoding (for compression) and decoding (for decompression).
 *
 * ## Zstd Huffman Specifics
 *
 * In Zstd:
 * - Huffman is used only for literals (not sequences)
 * - Tree weights: direct (header_byte >= 128, 4-bit packed) or FSE-compressed
 *   (header_byte < 128 = compressed size; used when >127 symbols)
 * - Maximum code length is 11 bits
 * - 4-stream mode for literals section >= 1024 bytes (encoder and decoder)
 *
 * ## Decoding
 *
 * The decoding table has 2^maxBits entries. Each entry contains:
 * - symbol: The decoded symbol (0-255)
 * - nb_bits: Number of bits consumed
 *
 * Decoding uses a reverse bit reader since Zstd Huffman streams are written
 * backwards with a marker bit at the end.
 *
 * ## Encoding
 *
 * The encoder implements:
 *
 * 1. **Tree Building** (two-queue algorithm):
 *    - Sort symbols by frequency
 *    - Repeatedly combine two lowest-frequency nodes
 *    - O(n log n) complexity for n symbols
 *
 * 2. **Code Length Limiting**:
 *    - Zstd limits codes to 11 bits maximum
 *    - If tree produces longer codes, redistribute bit lengths
 *    - Ensures decodable with fixed-size lookup table
 *
 * 3. **Canonical Code Generation**:
 *    - Sort symbols by (bit_length, symbol_value)
 *    - Assign codes sequentially within each bit length
 *    - Enables compact weight representation
 *
 * 4. **Weight Encoding** (RFC 8878 Section 4.2.1.2):
 *    - weight = maxBits + 1 - nb_bits (0 = symbol not present)
 *    - Direct (1..128 symbols): header_byte = 127 + num_weights (>= 128),
 *      then 4-bit weights packed high nibble first.
 *    - FSE-compressed (>127 symbols): header_byte = total compressed size
 *      (< 128), then FSE table header + backward FSE bitstream (see below).
 *
 * 5. **Bitstream Encoding**:
 *    - Write symbols forward using their Huffman codes
 *    - Add marker bit (1) at the end
 *    - Reverse byte order for backward reading by decoder
 *
 * ## Performance Considerations
 *
 * - Tree building uses insertion sort (O(n²)) which is acceptable for
 *   the maximum 256 symbols. A heap would be faster for larger alphabets.
 * - Code length limiting uses a simple greedy approach rather than the
 *   optimal package-merge algorithm. This may produce slightly longer
 *   codes in rare cases but is much simpler.
 *
 * ## FSE Weight Encoding (>127 symbols)
 *
 * When the Huffman table has more than 127 symbols, weights are too large
 * for the direct 4-bit representation (which is limited to 128 symbols by
 * header_byte - 127). Per RFC 8878 we use FSE-compressed weights:
 *
 * 1. Build a histogram of weight values (0..12, since max code length is 11).
 * 2. Normalize to FSE norm_counts (table_log = 6, sum = 64) with scaling
 *    and adjustment so sum(2^(norm-1)) + num(-1) = 64.
 * 3. Write FSE table header at output+1 (byte 0 reserved for header_byte).
 * 4. Build FSE decoding table from norm_counts
 * (zstd_fse_build_table_from_norm).
 * 5. Encode the weight sequence in REVERSE order using the FSE state machine
 *    (same as sequences): last weight first, output state-update bits, then
 *    initial state (table_log bits), then marker bit. Bitstream is backward.
 * 6. Set header_byte = 1 + header_size + bitstream_size (must be < 128).
 *
 * If compressed size would be >= 128 bytes, we return GCOMP_ERR_LIMIT so the
 * caller (literals encoder) can fall back to raw literals.
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
// Huffman Constants
//

#define HUF_MAX_BITS 11            ///< Maximum Huffman code length
#define HUF_MAX_SYMBOL_VALUE 255   ///< Maximum symbol value (byte)
#define HUF_MAX_TABLE_SIZE 2048    ///< Maximum table size (2^11)
#define HUF_WEIGHTS_COMPRESSED 128 ///< Threshold for FSE-compressed weights

//
// Forward Bit Reader (for Huffman weights header)
//

typedef struct {
  const uint8_t * src;
  size_t src_size;
  size_t byte_pos;
  uint64_t bit_container;
  unsigned bits_available;
} zstd_huf_fwd_reader_t;

static gcomp_status_t zstd_huf_fwd_reader_init(
    zstd_huf_fwd_reader_t * br, const uint8_t * src, size_t src_size) {
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

static gcomp_status_t zstd_huf_fwd_reader_ensure(
    zstd_huf_fwd_reader_t * br, unsigned nb_bits) {
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

static uint32_t zstd_huf_fwd_reader_read(
    zstd_huf_fwd_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  uint32_t result = (uint32_t)(br->bit_container & ((1ULL << nb_bits) - 1));
  br->bit_container >>= nb_bits;
  br->bits_available -= nb_bits;

  return result;
}

//
// Reverse Bit Reader (for Huffman streams)
//

typedef struct {
  const uint8_t * src;
  size_t src_size;
  int64_t bit_pos; ///< Current bit position (counts down from end)
  uint64_t bit_container;
} zstd_huf_rev_reader_t;

static gcomp_status_t zstd_huf_rev_reader_init(
    zstd_huf_rev_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src || src_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  br->src = src;
  br->src_size = src_size;

  // Find the highest set bit in the last byte (initialization marker)
  uint8_t last_byte = src[src_size - 1];
  if (last_byte == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  unsigned marker_bit = 7;
  while (marker_bit > 0 && ((last_byte >> marker_bit) & 1) == 0) {
    marker_bit--;
  }

  // Initialize bit position (bits from start of stream)
  br->bit_pos = (int64_t)(src_size * 8 - (8 - marker_bit));

  // Load initial container
  br->bit_container = 0;
  size_t start_byte = (br->bit_pos >= 64) ? (br->bit_pos / 8 - 7) : 0;
  size_t end_byte = br->bit_pos / 8;

  for (size_t i = start_byte; i <= end_byte && i < src_size; i++) {
    br->bit_container |= (uint64_t)src[i] << ((i - start_byte) * 8);
  }

  return GCOMP_OK;
}

static void zstd_huf_rev_reader_reload(zstd_huf_rev_reader_t * br) {
  // Reload bits if needed
  size_t byte_idx = br->bit_pos / 8;
  size_t start_byte = (byte_idx >= 7) ? (byte_idx - 7) : 0;

  br->bit_container = 0;
  for (size_t i = start_byte; i <= byte_idx && i < br->src_size; i++) {
    br->bit_container |= (uint64_t)br->src[i] << ((i - start_byte) * 8);
  }
}

static uint32_t zstd_huf_rev_reader_peek(
    zstd_huf_rev_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0 || br->bit_pos < (int64_t)nb_bits) {
    return 0;
  }

  size_t byte_idx = (size_t)(br->bit_pos / 8);

  // Bits we want start at bit_pos - nb_bits + 1
  int64_t start_bit = br->bit_pos - nb_bits;
  size_t start_byte = (size_t)(start_bit / 8);

  if (start_byte != byte_idx - 7 && start_byte != byte_idx) {
    zstd_huf_rev_reader_reload(br);
  }

  size_t container_start = (byte_idx >= 7) ? (byte_idx - 7) : 0;
  unsigned container_bit =
      (unsigned)((br->bit_pos - nb_bits + 1) - (int64_t)(container_start * 8));

  return (uint32_t)(br->bit_container >> container_bit) & ((1U << nb_bits) - 1);
}

static void zstd_huf_rev_reader_consume(
    zstd_huf_rev_reader_t * br, unsigned nb_bits) {
  br->bit_pos -= nb_bits;
}

//
// Weight Reading
//

/**
 * @brief Read Huffman weights from direct representation.
 *
 * When header byte >= 128, weights are stored directly as 4-bit values.
 * Number_Of_Symbols = header_byte - 127, stored as 4-bit nibbles.
 */
static gcomp_status_t zstd_huf_read_weights_direct(const uint8_t * src,
    size_t header_byte, uint8_t * weights, unsigned * num_symbols_out,
    size_t * bytes_read_out) {
  // header_byte = number of symbols - 1
  unsigned num_symbols = (unsigned)header_byte + 1;
  size_t bytes_needed = (num_symbols + 1) / 2;

  if (num_symbols > HUF_MAX_SYMBOL_VALUE + 1) {
    return GCOMP_ERR_CORRUPT;
  }

  // Read 4-bit weights
  for (unsigned i = 0; i < num_symbols; i++) {
    unsigned byte_idx = i / 2;
    if (i % 2 == 0) {
      weights[i] = src[byte_idx] >> 4;
    }
    else {
      weights[i] = src[byte_idx] & 0x0F;
    }
  }

  *num_symbols_out = num_symbols;
  *bytes_read_out = bytes_needed;

  return GCOMP_OK;
}

/**
 * @brief FSE bit reader for Huffman weights (backward reading with marker).
 *
 * FSE bitstreams for Huffman weights are read backwards from the end,
 * with a marker bit (highest set bit in last byte) indicating the start.
 */
typedef struct {
  const uint8_t * src;
  size_t src_size;
  size_t byte_pos;     ///< Current byte position (reads backward)
  uint64_t container;  ///< Bit accumulator
  unsigned bits_avail; ///< Bits available in container
} zstd_huf_fse_reader_t;

static gcomp_status_t zstd_huf_fse_reader_init(
    zstd_huf_fse_reader_t * br, const uint8_t * src, size_t src_size) {
  if (!br || !src || src_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  br->src = src;
  br->src_size = src_size;

  // Find marker bit in last byte
  uint8_t last_byte = src[src_size - 1];
  if (last_byte == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  unsigned marker_pos = 7;
  while (marker_pos > 0 && ((last_byte >> marker_pos) & 1) == 0) {
    marker_pos--;
  }

  // Load initial container (read from end backwards)
  br->container = 0;
  br->bits_avail = 0;

  // Load up to 8 bytes from the end
  size_t bytes_to_load = (src_size < 8) ? src_size : 8;
  br->byte_pos = src_size - bytes_to_load;

  for (size_t i = 0; i < bytes_to_load; i++) {
    br->container |= (uint64_t)src[br->byte_pos + i] << (i * 8);
  }

  // Calculate bits available (excluding marker)
  br->bits_avail = (unsigned)((src_size - br->byte_pos) * 8 - (8 - marker_pos));

  return GCOMP_OK;
}

static void zstd_huf_fse_reader_reload(zstd_huf_fse_reader_t * br) {
  // Reload when we've consumed full bytes
  while (br->bits_avail <= 56 && br->byte_pos > 0) {
    br->byte_pos--;
    br->container |= (uint64_t)br->src[br->byte_pos] << br->bits_avail;
    br->bits_avail += 8;
  }
}

static uint32_t zstd_huf_fse_reader_read(
    zstd_huf_fse_reader_t * br, unsigned nb_bits) {
  if (nb_bits == 0) {
    return 0;
  }

  zstd_huf_fse_reader_reload(br);

  if (br->bits_avail < nb_bits) {
    // Not enough bits - return what we have
    nb_bits = br->bits_avail;
  }

  uint32_t result = (uint32_t)(br->container & ((1ULL << nb_bits) - 1));
  br->container >>= nb_bits;
  br->bits_avail -= nb_bits;

  return result;
}

static bool zstd_huf_fse_reader_has_bits(
    const zstd_huf_fse_reader_t * br, unsigned nb_bits) {
  return br->bits_avail >= nb_bits || br->byte_pos > 0;
}

/**
 * @brief Read Huffman weights from FSE-compressed representation.
 *
 * When header byte < 128, weights are FSE-compressed.
 * Compressed size = header_byte (the raw value)
 *
 * The FSE bitstream contains weights until the sum of weight powers
 * reaches a power of 2 (indicating a complete Huffman tree).
 */
static gcomp_status_t zstd_huf_read_weights_fse(const uint8_t * src,
    size_t src_size, size_t compressed_size, uint8_t * weights,
    unsigned * num_symbols_out, size_t * bytes_read_out) {
  (void)src_size;

  if (compressed_size < 2) {
    // Need at least FSE header + some data
    return GCOMP_ERR_CORRUPT;
  }

  // Build FSE table for weights (max weight value ~12, accuracy log <= 6)
  zstd_fse_entry_t fse_table[64];
  unsigned table_log;
  unsigned max_symbol;
  size_t fse_header_size;

  gcomp_status_t status = zstd_fse_build_decoding_table(src, compressed_size,
      fse_table, 64, &table_log, &max_symbol, &fse_header_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // Initialize bit reader for the FSE bitstream (after header)
  const uint8_t * bitstream = src + fse_header_size;
  size_t bitstream_size = compressed_size - fse_header_size;

  if (bitstream_size == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  zstd_huf_fse_reader_t br;
  status = zstd_huf_fse_reader_init(&br, bitstream, bitstream_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // Read initial FSE state
  uint32_t state = zstd_huf_fse_reader_read(&br, table_log);

  // Decode weights until we have consumed the bitstream
  // Per RFC 8878: decode symbols until the stream runs out of bits

  unsigned num_symbols = 0;
  const unsigned max_symbols = HUF_MAX_SYMBOL_VALUE + 1; // 256

  // The FSE bitstream has a precise length. We decode until:
  // 1. We've processed all bits (reached the marker bit position)
  // 2. We've decoded the maximum number of symbols

  while (num_symbols < max_symbols && br.bits_avail > 0) {
    // Decode current symbol (weight value)
    if (state >= (1U << table_log)) {
      return GCOMP_ERR_CORRUPT;
    }

    const zstd_fse_entry_t * entry = &fse_table[state];
    uint8_t weight = entry->symbol;

    if (weight > HUF_MAX_BITS + 1) {
      return GCOMP_ERR_CORRUPT;
    }

    weights[num_symbols++] = weight;

    // Check if we can update FSE state (need enough bits)
    if (entry->nb_bits == 0) {
      // Zero bits to read - state stays at new_state base
      state = entry->new_state;
    }
    else if (br.bits_avail >= entry->nb_bits || br.byte_pos > 0) {
      // We have more bits to read or can reload
      uint32_t bits = zstd_huf_fse_reader_read(&br, entry->nb_bits);
      state = entry->new_state + bits;
    }
    else {
      // Not enough bits for state update - we've finished
      break;
    }
  }

  // Validate we got at least one symbol
  if (num_symbols == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  // Calculate weight sum for explicit symbols
  uint32_t weight_sum = 0;
  for (unsigned i = 0; i < num_symbols; i++) {
    if (weights[i] > 0) {
      weight_sum += 1U << (weights[i] - 1);
    }
  }

  if (weight_sum == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  // Per RFC 8878: There is an implicit last symbol whose weight fills the tree.
  // huf_table_log = highbit(weight_sum) + 1, so 2^huf_table_log > weight_sum
  // rest = 2^huf_table_log - weight_sum must be a power of 2
  uint32_t power = 1;
  while (power <= weight_sum) {
    power <<= 1;
  }

  uint32_t rest = power - weight_sum;
  // Check that rest is a power of 2
  if (rest == 0 || (rest & (rest - 1)) != 0) {
    return GCOMP_ERR_CORRUPT;
  }

  // Calculate last weight: lastWeight = highbit(rest) + 1
  unsigned last_weight = 0;
  uint32_t tmp = rest;
  while (tmp > 1) {
    tmp >>= 1;
    last_weight++;
  }
  last_weight++;

  // Add the implicit last symbol
  if (num_symbols >= HUF_MAX_SYMBOL_VALUE + 1) {
    return GCOMP_ERR_CORRUPT;
  }
  weights[num_symbols] = (uint8_t)last_weight;
  num_symbols++;

  *num_symbols_out = num_symbols;
  *bytes_read_out = compressed_size;

  return GCOMP_OK;
}

//
// Table Building
//

/**
 * @brief Build Huffman decoding table from weights.
 */
static gcomp_status_t zstd_huf_build_table_from_weights(const uint8_t * weights,
    unsigned num_symbols, zstd_huf_entry_t * table, size_t table_capacity,
    unsigned * max_bits_out) {
  if (!weights || !table || !max_bits_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Find max weight and calculate total weight
  unsigned max_weight = 0;
  uint32_t total_weight = 0;

  for (unsigned i = 0; i < num_symbols; i++) {
    if (weights[i] > max_weight) {
      max_weight = weights[i];
    }
    if (weights[i] > 0) {
      total_weight += 1U << (weights[i] - 1);
    }
  }

  if (max_weight > HUF_MAX_BITS) {
    return GCOMP_ERR_CORRUPT;
  }

  // Calculate max bits (table log)
  unsigned max_bits = 0;
  uint32_t power = 1;
  while (power < total_weight) {
    power <<= 1;
    max_bits++;
  }

  if (max_bits > HUF_MAX_BITS) {
    return GCOMP_ERR_CORRUPT;
  }

  size_t table_size = (size_t)1 << max_bits;
  if (table_capacity < table_size) {
    return GCOMP_ERR_LIMIT;
  }

  // Derive number of bits from weights
  // nb_bits[symbol] = max_bits + 1 - weight[symbol]
  // (weight 0 means symbol not present)

  // Track next code for each bit length
  uint32_t rank_val[HUF_MAX_BITS + 2] = {0};
  for (unsigned i = 0; i < num_symbols; i++) {
    if (weights[i] > 0) {
      unsigned nb_bits = max_bits + 1 - weights[i];
      rank_val[nb_bits]++;
    }
  }

  // Calculate starting codes for each rank
  uint32_t rank_start[HUF_MAX_BITS + 2] = {0};
  uint32_t next_code = 0;
  for (unsigned bits = 1; bits <= max_bits; bits++) {
    rank_start[bits] = next_code;
    next_code += rank_val[bits] << (max_bits - bits);
    rank_val[bits] = rank_start[bits];
  }

  // Fill table
  for (unsigned symbol = 0; symbol < num_symbols; symbol++) {
    if (weights[symbol] > 0) {
      unsigned nb_bits = max_bits + 1 - weights[symbol];
      uint32_t code = rank_val[nb_bits]++;
      uint32_t length = 1U << (max_bits - nb_bits);

      for (uint32_t j = 0; j < length; j++) {
        table[code + j].symbol = (uint8_t)symbol;
        table[code + j].nb_bits = (uint8_t)nb_bits;
      }
    }
  }

  *max_bits_out = max_bits;
  return GCOMP_OK;
}

//
// Public API
//

gcomp_status_t zstd_huf_read_table(const uint8_t * src, size_t src_size,
    zstd_huf_entry_t * table, size_t table_capacity, unsigned * max_bits_out,
    size_t * bytes_read_out) {
  if (!src || !table || !max_bits_out || !bytes_read_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (src_size < 1) {
    return GCOMP_ERR_CORRUPT;
  }

  // Read header byte
  uint8_t header_byte = src[0];
  size_t header_consumed = 1;

  uint8_t weights[HUF_MAX_SYMBOL_VALUE + 1] = {0};
  unsigned num_symbols;
  size_t weights_size;
  gcomp_status_t status;

  if (header_byte >= HUF_WEIGHTS_COMPRESSED) {
    // Direct representation (header_byte >= 128)
    // Per RFC 8878: Number_Of_Symbols = header_byte - 127
    unsigned direct_num_symbols = header_byte - 127;
    if (src_size < 1 + (size_t)(direct_num_symbols + 1) / 2) {
      return GCOMP_ERR_CORRUPT;
    }

    // zstd_huf_read_weights_direct expects (num_symbols - 1) as second param
    status = zstd_huf_read_weights_direct(
        src + 1, direct_num_symbols - 1, weights, &num_symbols, &weights_size);
  }
  else {
    // FSE-compressed weights (header_byte < 128)
    // Per RFC 8878: header_byte is the compressed size in bytes
    size_t compressed_size = header_byte;
    if (compressed_size == 0 || src_size < 1 + compressed_size) {
      return GCOMP_ERR_CORRUPT;
    }

    status = zstd_huf_read_weights_fse(src + 1, src_size - 1, compressed_size,
        weights, &num_symbols, &weights_size);
  }

  if (status != GCOMP_OK) {
    return status;
  }

  // Build decoding table from weights
  status = zstd_huf_build_table_from_weights(
      weights, num_symbols, table, table_capacity, max_bits_out);
  if (status != GCOMP_OK) {
    return status;
  }

  *bytes_read_out = header_consumed + weights_size;
  return GCOMP_OK;
}

gcomp_status_t zstd_huf_decode_1stream(const zstd_huf_entry_t * table,
    unsigned max_bits, const uint8_t * src, size_t src_size, uint8_t * dst,
    size_t dst_size, size_t * decoded_size_out) {
  if (!table || !src || !dst || !decoded_size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (src_size == 0) {
    *decoded_size_out = 0;
    return GCOMP_OK;
  }

  zstd_huf_rev_reader_t br;
  gcomp_status_t status = zstd_huf_rev_reader_init(&br, src, src_size);
  if (status != GCOMP_OK) {
    return status;
  }

  size_t decoded = 0;
  uint32_t mask = (1U << max_bits) - 1;

  while (br.bit_pos >= (int64_t)max_bits && decoded < dst_size) {
    uint32_t val = zstd_huf_rev_reader_peek(&br, max_bits);
    val &= mask;

    const zstd_huf_entry_t * entry = &table[val];
    dst[decoded++] = entry->symbol;
    zstd_huf_rev_reader_consume(&br, entry->nb_bits);
  }

  // Handle remaining bits if any
  while (br.bit_pos > 0 && decoded < dst_size) {
    unsigned available = (unsigned)br.bit_pos;
    if (available > max_bits) {
      available = max_bits;
    }

    uint32_t val = zstd_huf_rev_reader_peek(&br, available);
    val <<= (max_bits - available);
    val &= mask;

    const zstd_huf_entry_t * entry = &table[val];
    if (entry->nb_bits <= available) {
      dst[decoded++] = entry->symbol;
      zstd_huf_rev_reader_consume(&br, entry->nb_bits);
    }
    else {
      break;
    }
  }

  *decoded_size_out = decoded;
  return GCOMP_OK;
}

gcomp_status_t zstd_huf_decode_4streams(const zstd_huf_entry_t * table,
    unsigned max_bits, const uint8_t * src, size_t src_size,
    const uint32_t * jump_table, uint8_t * dst, size_t dst_size,
    size_t * decoded_size_out) {
  if (!table || !src || !jump_table || !dst || !decoded_size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // 4-stream mode: split literals into 4 streams for parallel decoding
  // jump_table contains 3 offsets (6 bytes) for streams 2, 3, 4

  uint32_t stream1_start = 0;
  uint32_t stream2_start = jump_table[0];
  uint32_t stream3_start = jump_table[1];
  uint32_t stream4_start = jump_table[2];

  if (stream2_start > src_size || stream3_start > src_size ||
      stream4_start > src_size) {
    return GCOMP_ERR_CORRUPT;
  }

  // Calculate stream sizes
  size_t stream1_size = stream2_start;
  size_t stream2_size = stream3_start - stream2_start;
  size_t stream3_size = stream4_start - stream3_start;
  size_t stream4_size = src_size - stream4_start;

  // Calculate expected output per stream (each stream decodes 1/4 of output)
  size_t per_stream = (dst_size + 3) / 4;

  size_t decoded1, decoded2, decoded3, decoded4;
  gcomp_status_t status;

  // Decode stream 1
  status = zstd_huf_decode_1stream(table, max_bits, src + stream1_start,
      stream1_size, dst, per_stream, &decoded1);
  if (status != GCOMP_OK) {
    return status;
  }

  // Decode stream 2
  status = zstd_huf_decode_1stream(table, max_bits, src + stream2_start,
      stream2_size, dst + per_stream, per_stream, &decoded2);
  if (status != GCOMP_OK) {
    return status;
  }

  // Decode stream 3
  status = zstd_huf_decode_1stream(table, max_bits, src + stream3_start,
      stream3_size, dst + per_stream * 2, per_stream, &decoded3);
  if (status != GCOMP_OK) {
    return status;
  }

  // Decode stream 4
  status = zstd_huf_decode_1stream(table, max_bits, src + stream4_start,
      stream4_size, dst + per_stream * 3, dst_size - per_stream * 3, &decoded4);
  if (status != GCOMP_OK) {
    return status;
  }

  *decoded_size_out = decoded1 + decoded2 + decoded3 + decoded4;
  return GCOMP_OK;
}

//============================================================================
// Huffman Encoding
//============================================================================
//
// The Huffman encoder creates variable-length codes for literal bytes based
// on their frequency in the input. The encoding process is:
//
// 1. COUNT FREQUENCIES
//    Count occurrences of each byte value (0-255) in the literals.
//
// 2. BUILD HUFFMAN TREE
//    Use the classic two-queue algorithm:
//    - Create leaf node for each symbol with non-zero frequency
//    - Sort leaves by frequency (ascending)
//    - Repeatedly merge two lowest-frequency nodes into internal node
//    - Tree depth at each leaf = code length for that symbol
//
// 3. LIMIT CODE LENGTHS
//    Zstd limits Huffman codes to 11 bits maximum. If the tree produces
//    longer codes (highly skewed distributions), redistribute bit lengths
//    to stay within the limit while maintaining decodability.
//
// 4. GENERATE CANONICAL CODES
//    Sort symbols by (bit_length, symbol_value), then assign codes
//    sequentially. This canonical form allows compact representation
//    via weights rather than explicit tree structure.
//
// 5. CALCULATE WEIGHTS
//    Convert bit lengths to weights: weight = maxBits + 1 - nb_bits
//    Weight 0 means symbol is not present in the data.
//
// 6. WRITE WEIGHTS HEADER
//    For <= 127 symbols, use direct 4-bit representation:
//    - Header byte = num_symbols - 1 (< 128)
//    - Weights packed as nibbles: high nibble first
//
// 7. ENCODE LITERALS
//    Write each literal's Huffman code to a forward bitstream, then
//    add a marker bit (1) and reverse the byte order. The decoder
//    reads backwards, using the marker to find the start position.
//
// Example encoding:
//   Input:  "AAAAABBC"
//   Freqs:  A=5, B=2, C=1
//   Tree:   A gets 1-bit code (frequent), B and C get 2-bit codes
//   Codes:  A=0, B=10, C=11 (canonical assignment)
//   Bits:   0 0 0 0 0 10 10 11 [marker=1]
//   Output: Reversed bytes with marker at end
//
//============================================================================

//
// Huffman Tree Node (for building)
//

typedef struct {
  uint32_t freq;   ///< Symbol frequency
  int16_t parent;  ///< Parent node index (-1 if root)
  int16_t left;    ///< Left child (-1 if leaf)
  int16_t right;   ///< Right child (-1 if leaf)
  uint16_t symbol; ///< Symbol value (for leaves)
  uint8_t depth;   ///< Depth in tree (= code length)
} zstd_huf_node_t;

/**
 * @brief Build Huffman tree using standard algorithm.
 *
 * Creates a binary tree where more frequent symbols have shorter paths.
 *
 * @param freq Symbol frequencies (256 entries)
 * @param nodes Node array (must have space for 511 nodes: 256 leaves + 255
 * internal)
 * @param num_symbols_out Output: number of symbols with non-zero frequency
 * @param root_out Output: index of root node
 * @return GCOMP_OK on success
 */
static gcomp_status_t zstd_huf_build_tree(const uint32_t * freq,
    zstd_huf_node_t * nodes, unsigned * num_symbols_out, int * root_out) {
  // Initialize leaf nodes for symbols with non-zero frequency
  unsigned num_leaves = 0;
  for (unsigned i = 0; i < 256; i++) {
    if (freq[i] > 0) {
      nodes[num_leaves].freq = freq[i];
      nodes[num_leaves].symbol = (uint16_t)i;
      nodes[num_leaves].parent = -1;
      nodes[num_leaves].left = -1;
      nodes[num_leaves].right = -1;
      nodes[num_leaves].depth = 0;
      num_leaves++;
    }
  }

  if (num_leaves == 0) {
    *num_symbols_out = 0;
    *root_out = -1;
    return GCOMP_OK;
  }

  if (num_leaves == 1) {
    // Single symbol: give it depth 1
    nodes[0].depth = 1;
    *num_symbols_out = 1;
    *root_out = 0;
    return GCOMP_OK;
  }

  *num_symbols_out = num_leaves;

  // Sort leaves by frequency (simple insertion sort - fine for 256 elements)
  for (unsigned i = 1; i < num_leaves; i++) {
    zstd_huf_node_t temp = nodes[i];
    unsigned j = i;
    while (j > 0 && nodes[j - 1].freq > temp.freq) {
      nodes[j] = nodes[j - 1];
      j--;
    }
    nodes[j] = temp;
  }

  // Build tree using two-queue algorithm
  // Queue 1: sorted leaves (front at index q1_front)
  // Queue 2: internal nodes (at indices num_leaves and beyond)
  unsigned q1_front = 0;
  unsigned q2_front = num_leaves;
  unsigned q2_back = num_leaves;

  // Build internal nodes
  while ((q1_front < num_leaves ? 1 : 0) + (q2_back - q2_front) > 1) {
    // Get first minimum node
    int left;
    if (q1_front < num_leaves &&
        (q2_front >= q2_back || nodes[q1_front].freq <= nodes[q2_front].freq)) {
      left = (int)q1_front++;
    }
    else {
      left = (int)q2_front++;
    }

    // Get second minimum node
    int right;
    if (q1_front < num_leaves &&
        (q2_front >= q2_back || nodes[q1_front].freq <= nodes[q2_front].freq)) {
      right = (int)q1_front++;
    }
    else {
      right = (int)q2_front++;
    }

    // Create internal node
    nodes[q2_back].freq = nodes[left].freq + nodes[right].freq;
    nodes[q2_back].left = (int16_t)left;
    nodes[q2_back].right = (int16_t)right;
    nodes[q2_back].parent = -1;
    nodes[q2_back].symbol = 0xFFFF; // Internal node marker
    nodes[q2_back].depth = 0;

    nodes[left].parent = (int16_t)q2_back;
    nodes[right].parent = (int16_t)q2_back;

    q2_back++;
  }

  // Root is the last internal node (or the single remaining node)
  int root;
  if (q1_front < num_leaves) {
    root = (int)q1_front;
  }
  else {
    root = (int)q2_front;
  }

  // Calculate depths using BFS from root
  nodes[root].depth = 0;

  // Process nodes in reverse order (parents before children in q2)
  for (int i = (int)q2_back - 1; i >= (int)num_leaves; i--) {
    uint8_t parent_depth = nodes[i].depth;
    if (nodes[i].left >= 0) {
      nodes[nodes[i].left].depth = parent_depth + 1;
    }
    if (nodes[i].right >= 0) {
      nodes[nodes[i].right].depth = parent_depth + 1;
    }
  }

  *root_out = root;
  return GCOMP_OK;
}

/**
 * @brief Limit code lengths to maximum allowed (11 bits for zstd).
 *
 * Uses the package-merge algorithm concept to redistribute bit lengths.
 *
 * @param depths Array of code lengths for each symbol (modified in place)
 * @param freq Original frequencies
 * @param num_symbols Number of symbols
 * @param max_bits Maximum allowed code length
 */
static void zstd_huf_limit_depths(uint8_t * depths, const uint32_t * freq,
    unsigned num_symbols, unsigned max_bits) {
  // Check if any depth exceeds max
  bool need_limit = false;
  for (unsigned i = 0; i < num_symbols; i++) {
    if (depths[i] > max_bits) {
      need_limit = true;
      break;
    }
  }

  if (!need_limit) {
    return;
  }

  // Simple approach: cap depths and redistribute
  // This isn't optimal but produces valid codes
  uint32_t total = 0;
  for (unsigned i = 0; i < num_symbols; i++) {
    if (depths[i] > max_bits) {
      depths[i] = (uint8_t)max_bits;
    }
    total += 1U << (max_bits - depths[i]);
  }

  // If total > 2^max_bits, we need to increase some depths
  uint32_t max_total = 1U << max_bits;
  while (total > max_total) {
    // Find symbol with smallest depth and increase it
    unsigned min_idx = 0;
    uint8_t min_depth = depths[0];
    for (unsigned i = 1; i < num_symbols; i++) {
      if (depths[i] < min_depth) {
        min_depth = depths[i];
        min_idx = i;
      }
    }

    if (min_depth >= max_bits) {
      break; // Can't increase further
    }

    total -= 1U << (max_bits - depths[min_idx]);
    depths[min_idx]++;
    total += 1U << (max_bits - depths[min_idx]);
  }

  // If total < 2^max_bits, decrease some depths (make codes shorter)
  // This redistributes unused code space
  while (total < max_total) {
    // Find symbol with largest depth (shortest code = biggest contribution)
    unsigned max_idx = 0;
    uint8_t max_depth = 0;
    for (unsigned i = 0; i < num_symbols; i++) {
      if (depths[i] > max_depth) {
        max_depth = depths[i];
        max_idx = i;
      }
    }

    if (max_depth <= 1) {
      break;
    }

    // Check if we can decrease this depth
    uint32_t gain = 1U << (max_bits - max_depth + 1);
    uint32_t loss = 1U << (max_bits - max_depth);
    if (total - loss + gain <= max_total) {
      total = total - loss + gain;
      depths[max_idx]--;
    }
    else {
      break;
    }
  }

  (void)freq; // freq could be used for better redistribution
}

/**
 * @brief Generate canonical Huffman codes from sorted symbols and depths.
 *
 * @param symbols Symbol array (sorted by depth, then by symbol)
 * @param depths Depth/bit-length for each symbol
 * @param num_symbols Number of symbols
 * @param table Output encoding table
 */
static void zstd_huf_generate_codes(const uint16_t * symbols,
    const uint8_t * depths, unsigned num_symbols,
    zstd_huf_enc_table_t * table) {
  if (num_symbols == 0) {
    table->max_bits = 0;
    table->num_symbols = 0;
    return;
  }

  // Find max depth
  unsigned max_depth = 0;
  for (unsigned i = 0; i < num_symbols; i++) {
    if (depths[i] > max_depth) {
      max_depth = depths[i];
    }
  }
  table->max_bits = max_depth;
  table->num_symbols = num_symbols;

  // Count symbols per depth
  unsigned count[HUF_MAX_BITS + 1] = {0};
  for (unsigned i = 0; i < num_symbols; i++) {
    count[depths[i]]++;
  }

  // Generate starting code for each depth (canonical Huffman)
  uint16_t next_code[HUF_MAX_BITS + 1] = {0};
  uint16_t code = 0;
  for (unsigned bits = 1; bits <= max_depth; bits++) {
    code = (uint16_t)((code + count[bits - 1]) << 1);
    next_code[bits] = code;
  }

  // Assign codes to symbols
  // Initialize all entries as unused
  for (unsigned i = 0; i < 256; i++) {
    table->symbols[i].code = 0;
    table->symbols[i].nb_bits = 0;
  }

  for (unsigned i = 0; i < num_symbols; i++) {
    uint16_t sym = symbols[i];
    uint8_t nbits = depths[i];
    table->symbols[sym].code = next_code[nbits]++;
    table->symbols[sym].nb_bits = nbits;
  }

  // Calculate weights: weight = maxBits + 1 - nb_bits (0 if unused)
  for (unsigned i = 0; i < 256; i++) {
    if (table->symbols[i].nb_bits > 0) {
      table->weights[i] = (uint8_t)(max_depth + 1 - table->symbols[i].nb_bits);
    }
    else {
      table->weights[i] = 0;
    }
  }
}

gcomp_status_t zstd_huf_build_enc_table(
    const uint32_t * freq, zstd_huf_enc_table_t * table) {
  if (!freq || !table) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Clear table
  memset(table, 0, sizeof(*table));

  // Build Huffman tree
  zstd_huf_node_t nodes[511]; // 256 leaves + 255 internal nodes max
  unsigned num_symbols;
  int root;

  gcomp_status_t status = zstd_huf_build_tree(freq, nodes, &num_symbols, &root);
  if (status != GCOMP_OK) {
    return status;
  }

  if (num_symbols == 0) {
    table->max_bits = 0;
    table->num_symbols = 0;
    return GCOMP_OK;
  }

  // Extract depths and symbols from tree
  uint16_t symbols[256];
  uint8_t depths[256];

  unsigned sym_idx = 0;
  for (unsigned i = 0; i < num_symbols; i++) {
    if (nodes[i].symbol < 256) { // Leaf node
      symbols[sym_idx] = nodes[i].symbol;
      depths[sym_idx] = nodes[i].depth;
      sym_idx++;
    }
  }
  num_symbols = sym_idx;

  // Limit depths to 11 bits (zstd maximum)
  zstd_huf_limit_depths(depths, freq, num_symbols, HUF_MAX_BITS);

  // Sort symbols by (depth, symbol) for canonical code generation
  for (unsigned i = 1; i < num_symbols; i++) {
    uint16_t sym = symbols[i];
    uint8_t depth = depths[i];
    unsigned j = i;
    while (j > 0 &&
        (depths[j - 1] > depth ||
            (depths[j - 1] == depth && symbols[j - 1] > sym))) {
      symbols[j] = symbols[j - 1];
      depths[j] = depths[j - 1];
      j--;
    }
    symbols[j] = sym;
    depths[j] = depth;
  }

  // Generate canonical codes
  zstd_huf_generate_codes(symbols, depths, num_symbols, table);

  return GCOMP_OK;
}

//
// FSE weight encoding: backward bit writer and helpers
//

#define HUF_FSE_WEIGHT_MAX_SYMBOL (HUF_MAX_BITS + 1) /* 0..12 */
#define HUF_FSE_WEIGHT_TABLE_LOG 6
#define HUF_FSE_WEIGHT_TABLE_SIZE (1U << HUF_FSE_WEIGHT_TABLE_LOG)

typedef struct {
  uint8_t * buf;
  size_t buf_size;
  size_t byte_pos;
  uint64_t bit_container;
  unsigned bits_used;
} zstd_huf_fse_bw_t;

static void zstd_huf_fse_bw_init(
    zstd_huf_fse_bw_t * bw, uint8_t * buf, size_t buf_size) {
  bw->buf = buf;
  bw->buf_size = buf_size;
  bw->byte_pos = 0;
  bw->bit_container = 0;
  bw->bits_used = 0;
}

static void zstd_huf_fse_bw_flush(zstd_huf_fse_bw_t * bw) {
  while (bw->bits_used >= 8 && bw->byte_pos < bw->buf_size) {
    bw->buf[bw->byte_pos++] = (uint8_t)(bw->bit_container & 0xFF);
    bw->bit_container >>= 8;
    bw->bits_used -= 8;
  }
}

static void zstd_huf_fse_bw_add_bits(
    zstd_huf_fse_bw_t * bw, uint32_t value, unsigned nb_bits) {
  if (nb_bits == 0) {
    return;
  }
  if (bw->bits_used + nb_bits > 56) {
    zstd_huf_fse_bw_flush(bw);
  }
  bw->bit_container |= ((uint64_t)value << bw->bits_used);
  bw->bits_used += nb_bits;
}

static size_t zstd_huf_fse_bw_close(zstd_huf_fse_bw_t * bw) {
  bw->bit_container |= (1ULL << bw->bits_used);
  bw->bits_used++;
  unsigned bytes_needed = (bw->bits_used + 7) / 8;
  for (unsigned i = 0; i < bytes_needed && bw->byte_pos < bw->buf_size; i++) {
    bw->buf[bw->byte_pos++] = (uint8_t)(bw->bit_container & 0xFF);
    bw->bit_container >>= 8;
  }
  return bw->byte_pos;
}

static uint16_t zstd_huf_fse_find_state_for_symbol(
    const zstd_fse_entry_t * table, size_t table_size, uint8_t symbol) {
  for (size_t i = 0; i < table_size; i++) {
    if (table[i].symbol == symbol) {
      return (uint16_t)i;
    }
  }
  return 0;
}

static uint16_t zstd_huf_fse_find_encode_state(const zstd_fse_entry_t * table,
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
  *bits_out = 0;
  *nb_bits_out = 0;
  return 0xFFFF;
}

/**
 * @brief Write Huffman weights using FSE compression (>127 symbols).
 *
 * Per RFC 8878: header_byte < 128 means FSE-compressed; header_byte =
 * compressed size in bytes. If compressed size would be >= 128, returns
 * GCOMP_ERR_LIMIT (caller may fall back to raw literals).
 */
static gcomp_status_t zstd_huf_write_weights_fse(
    const zstd_huf_enc_table_t * table, unsigned num_weights, uint8_t * output,
    size_t output_cap, size_t * output_len_out) {
  if (!table || !output || !output_len_out || num_weights <= 128) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // 1. Build weight value histogram (0..12)
  unsigned counts[HUF_FSE_WEIGHT_MAX_SYMBOL + 1];
  memset(counts, 0, sizeof(counts));
  for (unsigned i = 0; i < num_weights; i++) {
    uint8_t w = table->weights[i];
    if (w <= HUF_FSE_WEIGHT_MAX_SYMBOL) {
      counts[w]++;
    }
  }

  unsigned total_count = 0;
  for (unsigned i = 0; i <= HUF_FSE_WEIGHT_MAX_SYMBOL; i++) {
    total_count += counts[i];
  }
  if (total_count == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  // 2. Normalize to FSE norm_counts (table_log=6, sum = 64)
  int16_t norm_counts[HUF_FSE_WEIGHT_MAX_SYMBOL + 2]; /* 0..12 + 1 */
  memset(norm_counts, 0, sizeof(norm_counts));
  unsigned table_size = HUF_FSE_WEIGHT_TABLE_SIZE;

  for (unsigned i = 0; i <= HUF_FSE_WEIGHT_MAX_SYMBOL; i++) {
    if (counts[i] == 0) {
      continue;
    }
    // Share of table: (count * 64) / total_count. Norm such that 2^(n-1) ≈
    // share.
    unsigned share = (counts[i] * table_size) / total_count;
    if (share == 0) {
      norm_counts[i] = -1; /* less-than-one probability */
    }
    else {
      unsigned n = 1;
      while ((1U << (n - 1)) < share && n < 12) {
        n++;
      }
      norm_counts[i] = (int16_t)(int)n;
    }
  }

  // Adjust so sum(2^(norm-1)) + num(-1) = 64
  int sum = 0;
  int num_neg1 = 0;
  for (unsigned i = 0; i <= HUF_FSE_WEIGHT_MAX_SYMBOL; i++) {
    if (norm_counts[i] == -1) {
      num_neg1++;
    }
    else if (norm_counts[i] > 0) {
      sum += 1 << (norm_counts[i] - 1);
    }
  }
  while (sum + num_neg1 < (int)table_size) {
    unsigned best = 0;
    int best_norm = 0;
    for (unsigned i = 0; i <= HUF_FSE_WEIGHT_MAX_SYMBOL; i++) {
      if (norm_counts[i] > 0 && norm_counts[i] < 12 &&
          (best_norm == 0 || norm_counts[i] > best_norm)) {
        best = i;
        best_norm = norm_counts[i];
      }
    }
    if (best_norm == 0) {
      break;
    }
    sum -= 1 << (norm_counts[best] - 1);
    norm_counts[best]++;
    sum += 1 << (norm_counts[best] - 1);
  }
  while (sum + num_neg1 > (int)table_size) {
    unsigned best = 0;
    int best_norm = 0;
    for (unsigned i = 0; i <= HUF_FSE_WEIGHT_MAX_SYMBOL; i++) {
      if (norm_counts[i] > 1 &&
          (best_norm == 0 || norm_counts[i] > best_norm)) {
        best = i;
        best_norm = norm_counts[i];
      }
    }
    if (best_norm == 0) {
      break;
    }
    sum -= 1 << (norm_counts[best] - 1);
    norm_counts[best]--;
    sum += (norm_counts[best] > 0) ? (1 << (norm_counts[best] - 1)) : 0;
  }

  // 3. Write FSE table header at output+1 (output[0] reserved for header byte)
  if (output_cap < 2) {
    return GCOMP_ERR_LIMIT;
  }
  size_t header_size = 0;
  gcomp_status_t status = zstd_fse_write_table_header(output + 1,
      output_cap - 1, norm_counts, (unsigned)HUF_FSE_WEIGHT_MAX_SYMBOL,
      HUF_FSE_WEIGHT_TABLE_LOG, &header_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // 4. Build FSE decoding table
  zstd_fse_entry_t fse_table[HUF_FSE_WEIGHT_TABLE_SIZE];
  status = zstd_fse_build_table_from_norm(norm_counts,
      (unsigned)HUF_FSE_WEIGHT_MAX_SYMBOL, HUF_FSE_WEIGHT_TABLE_LOG, fse_table);
  if (status != GCOMP_OK) {
    return status;
  }

  // 5. Encode weight sequence in reverse order (backward bitstream)
  if (output_cap < 1 + header_size + 1) {
    return GCOMP_ERR_LIMIT;
  }
  zstd_huf_fse_bw_t bw;
  zstd_huf_fse_bw_init(
      &bw, output + 1 + header_size, output_cap - 1 - header_size);

  uint16_t enc_state = 0;
  for (unsigned idx = num_weights; idx > 0; idx--) {
    unsigned i = idx - 1;
    uint8_t weight = table->weights[i];
    if (weight > HUF_FSE_WEIGHT_MAX_SYMBOL) {
      return GCOMP_ERR_CORRUPT;
    }

    if (i == num_weights - 1) {
      enc_state = zstd_huf_fse_find_state_for_symbol(
          fse_table, HUF_FSE_WEIGHT_TABLE_SIZE, weight);
    }
    else {
      uint8_t bits_out, nb_bits_out;
      uint16_t prev_state =
          zstd_huf_fse_find_encode_state(fse_table, HUF_FSE_WEIGHT_TABLE_SIZE,
              weight, enc_state, &bits_out, &nb_bits_out);
      if (prev_state == 0xFFFF) {
        return GCOMP_ERR_CORRUPT;
      }
      enc_state = prev_state;
      zstd_huf_fse_bw_add_bits(&bw, bits_out, nb_bits_out);
    }
  }

  zstd_huf_fse_bw_add_bits(&bw, enc_state, HUF_FSE_WEIGHT_TABLE_LOG);
  size_t bitstream_size = zstd_huf_fse_bw_close(&bw);

  size_t total_size = 1 + header_size + bitstream_size;
  if (total_size >= HUF_WEIGHTS_COMPRESSED) {
    return GCOMP_ERR_LIMIT; /* FSE header byte must be < 128 */
  }

  // 6. Write header byte = compressed size (per RFC 8878: header_byte < 128)
  output[0] = (uint8_t)total_size;
  *output_len_out = total_size;
  return GCOMP_OK;
}

gcomp_status_t zstd_huf_write_weights(const zstd_huf_enc_table_t * table,
    uint8_t * output, size_t output_cap, size_t * output_len_out) {
  if (!table || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Find last non-zero weight (number of symbols - 1)
  unsigned last_symbol = 0;
  for (unsigned i = 0; i < 256; i++) {
    if (table->weights[i] > 0) {
      last_symbol = i;
    }
  }

  // Number of symbols to write
  unsigned num_weights = last_symbol + 1;

  // Per RFC 8878: direct mode uses header_byte >= 128, Number_Of_Symbols =
  // header_byte - 127. FSE mode uses header_byte < 128, header_byte =
  // compressed size.
  if (num_weights == 0) {
    // No symbols - shouldn't happen for valid data
    if (output_cap < 1) {
      return GCOMP_ERR_LIMIT;
    }
    output[0] = 0;
    *output_len_out = 1;
    return GCOMP_OK;
  }

  if (num_weights <= 128) {
    // Direct representation: header_byte = 127 + num_weights
    size_t weights_size = (num_weights + 1) / 2;
    size_t total_size = 1 + weights_size;

    if (output_cap < total_size) {
      return GCOMP_ERR_LIMIT;
    }

    output[0] = (uint8_t)(127 + num_weights);

    // Write weights as 4-bit pairs (high nibble first)
    for (unsigned i = 0; i < num_weights; i += 2) {
      uint8_t w0 = table->weights[i];
      uint8_t w1 = (i + 1 < num_weights) ? table->weights[i + 1] : 0;
      output[1 + i / 2] = (uint8_t)((w0 << 4) | (w1 & 0x0F));
    }

    *output_len_out = total_size;
    return GCOMP_OK;
  }

  // num_weights > 128: use FSE-compressed weights (see below)
  {
    size_t fse_len = 0;
    gcomp_status_t status = zstd_huf_write_weights_fse(
        table, num_weights, output, output_cap, &fse_len);
    if (status != GCOMP_OK) {
      return status;
    }
    *output_len_out = fse_len;
    return GCOMP_OK;
  }
}

gcomp_status_t zstd_huf_encode_1stream(const zstd_huf_enc_table_t * table,
    const uint8_t * literals, size_t literals_size, uint8_t * output,
    size_t output_cap, size_t * output_len_out) {
  if (!table || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (literals_size == 0 || !literals) {
    // Empty input: just write marker byte
    if (output_cap < 1) {
      return GCOMP_ERR_LIMIT;
    }
    output[0] = 0x01; // Single marker bit
    *output_len_out = 1;
    return GCOMP_OK;
  }

  // Huffman bitstream is written backwards with a marker bit at the end.
  // We encode forward into a bit buffer, then reverse the byte order.
  // The marker bit (1) marks the start of data when reading backwards.

  // Estimate maximum output size (worst case: all symbols have max bits)
  size_t max_output = (literals_size * table->max_bits + 7) / 8 + 1;
  if (output_cap < max_output) {
    // May not be enough, but try anyway
  }

  // Bit buffer for encoding
  uint64_t bit_buffer = 0;
  unsigned bits_in_buffer = 0;
  size_t output_pos = 0;

  // Encode literals forward
  for (size_t i = 0; i < literals_size; i++) {
    uint8_t sym = literals[i];
    const zstd_huf_enc_entry_t * entry = &table->symbols[sym];

    if (entry->nb_bits == 0) {
      // Symbol not in table - shouldn't happen if table built from same data
      return GCOMP_ERR_CORRUPT;
    }

    // Add code bits to buffer (MSB first)
    bit_buffer |= (uint64_t)entry->code << bits_in_buffer;
    bits_in_buffer += entry->nb_bits;

    // Flush complete bytes
    while (bits_in_buffer >= 8) {
      if (output_pos >= output_cap) {
        return GCOMP_ERR_LIMIT;
      }
      output[output_pos++] = (uint8_t)(bit_buffer & 0xFF);
      bit_buffer >>= 8;
      bits_in_buffer -= 8;
    }
  }

  // Add marker bit (1) at the end
  bit_buffer |= (1ULL << bits_in_buffer);
  bits_in_buffer++;

  // Flush remaining bits (pad with zeros on the high side)
  while (bits_in_buffer > 0) {
    if (output_pos >= output_cap) {
      return GCOMP_ERR_LIMIT;
    }
    output[output_pos++] = (uint8_t)(bit_buffer & 0xFF);
    bit_buffer >>= 8;
    if (bits_in_buffer >= 8) {
      bits_in_buffer -= 8;
    }
    else {
      bits_in_buffer = 0;
    }
  }

  // Reverse byte order (decoder reads backwards)
  for (size_t i = 0; i < output_pos / 2; i++) {
    uint8_t tmp = output[i];
    output[i] = output[output_pos - 1 - i];
    output[output_pos - 1 - i] = tmp;
  }

  *output_len_out = output_pos;
  return GCOMP_OK;
}

#define HUF_4STREAM_THRESHOLD 1024

gcomp_status_t zstd_huf_encode_4streams(const zstd_huf_enc_table_t * table,
    const uint8_t * literals, size_t literals_size, uint8_t * output,
    size_t output_cap, size_t * output_len_out) {
  if (!table || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (literals_size == 0 || !literals) {
    if (output_cap < 7) {
      return GCOMP_ERR_LIMIT;
    }
    // Empty: jump table (0,0,0) + one marker byte
    output[0] = 0;
    output[1] = 0;
    output[2] = 0;
    output[3] = 0;
    output[4] = 0;
    output[5] = 0;
    output[6] = 0x01;
    *output_len_out = 7;
    return GCOMP_OK;
  }

  // Split literals into 4 roughly equal segments (decoder expects 1/4 each)
  size_t per_stream = (literals_size + 3) / 4;
  size_t s0 = per_stream;
  size_t s1 = per_stream * 2;
  size_t s2 = per_stream * 3;
  if (s1 > literals_size) {
    s1 = literals_size;
  }
  if (s2 > literals_size) {
    s2 = literals_size;
  }
  // Segment boundaries: [0, s0), [s0, s1), [s1, s2), [s2, literals_size)
  size_t seg0_len = s0;
  size_t seg1_len = s1 - s0;
  size_t seg2_len = s2 - s1;
  size_t seg3_len = literals_size - s2;

  // Need at least 6 bytes for jump table
  if (output_cap < 6) {
    return GCOMP_ERR_LIMIT;
  }

  size_t pos = 6; // Reserve space for jump table

  size_t stream1_size, stream2_size, stream3_size, stream4_size;
  gcomp_status_t status;

  status = zstd_huf_encode_1stream(
      table, literals, seg0_len, output + pos, output_cap - pos, &stream1_size);
  if (status != GCOMP_OK) {
    return status;
  }
  pos += stream1_size;

  status = zstd_huf_encode_1stream(table, literals + s0, seg1_len, output + pos,
      output_cap - pos, &stream2_size);
  if (status != GCOMP_OK) {
    return status;
  }
  pos += stream2_size;

  status = zstd_huf_encode_1stream(table, literals + s1, seg2_len, output + pos,
      output_cap - pos, &stream3_size);
  if (status != GCOMP_OK) {
    return status;
  }
  pos += stream3_size;

  status = zstd_huf_encode_1stream(table, literals + s2, seg3_len, output + pos,
      output_cap - pos, &stream4_size);
  if (status != GCOMP_OK) {
    return status;
  }
  pos += stream4_size;

  // Write jump table (3 × 2-byte LE offsets for start of streams 2, 3, 4)
  uint32_t j0 = (uint32_t)stream1_size;
  uint32_t j1 = (uint32_t)(stream1_size + stream2_size);
  uint32_t j2 = (uint32_t)(stream1_size + stream2_size + stream3_size);
  gcomp_write_le16(output + 0, (uint16_t)j0);
  gcomp_write_le16(output + 2, (uint16_t)j1);
  gcomp_write_le16(output + 4, (uint16_t)j2);

  *output_len_out = pos;
  return GCOMP_OK;
}
