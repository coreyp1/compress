/**
 * @file zstd_huf.c
 *
 * Huffman decoder for the Ghoti.io Compress library.
 *
 * ## Algorithm Overview
 *
 * Huffman coding assigns variable-length codes to symbols based on their
 * frequency. More frequent symbols get shorter codes. Zstd uses a
 * table-based approach for fast decoding.
 *
 * ## Zstd Huffman Specifics
 *
 * In Zstd:
 * - Huffman is used only for literals (not sequences)
 * - Tree weights are encoded using FSE
 * - Maximum number of bits is 11
 * - 4-stream mode is used for large literal sections
 *
 * ## Decoding Table
 *
 * The decoding table has 2^maxBits entries. Each entry contains:
 * - symbol: The decoded symbol
 * - nb_bits: Number of bits consumed
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
 * When header byte < 128, weights are stored directly as 4-bit values.
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
 * @brief Read Huffman weights from FSE-compressed representation.
 *
 * When header byte >= 128, weights are FSE-compressed.
 * Compressed size = header_byte - 127
 */
static gcomp_status_t zstd_huf_read_weights_fse(const uint8_t * src,
    size_t src_size, size_t compressed_size, uint8_t * weights,
    unsigned * num_symbols_out, size_t * bytes_read_out) {
  (void)src_size;

  // Build FSE table for weights (max weight value is 12)
  zstd_fse_entry_t fse_table[64]; // accuracy log <= 6 for weights
  unsigned table_log;
  unsigned max_symbol;
  size_t fse_header_size;

  gcomp_status_t status = zstd_fse_build_decoding_table(src, compressed_size,
      fse_table, 64, &table_log, &max_symbol, &fse_header_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // TODO: Decode weights from FSE bitstream
  // For now, return unsupported until we have full FSE stream decoding
  (void)weights;
  (void)num_symbols_out;
  (void)bytes_read_out;
  (void)fse_table;
  (void)table_log;
  (void)fse_header_size;

  return GCOMP_ERR_UNSUPPORTED;
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

  if (header_byte < HUF_WEIGHTS_COMPRESSED) {
    // Direct representation
    if (src_size < 1 + (size_t)(header_byte + 2) / 2) {
      return GCOMP_ERR_CORRUPT;
    }

    status = zstd_huf_read_weights_direct(
        src + 1, header_byte, weights, &num_symbols, &weights_size);
  }
  else {
    // FSE-compressed weights
    size_t compressed_size = header_byte - 127;
    if (src_size < 1 + compressed_size) {
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
