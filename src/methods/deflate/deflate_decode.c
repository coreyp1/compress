/**
 * @file deflate_decode.c
 *
 * Streaming DEFLATE (RFC 1951) decoder for the Ghoti.io Compress library.
 *
 * Implements all DEFLATE block types (stored, fixed Huffman, dynamic Huffman)
 * with a 32KiB (max) sliding window. Designed to work with the library's
 * update/finish streaming semantics: partial input and partial output buffers
 * are supported by retaining internal state across calls.
 *
 * ## Safety Limits
 *
 * The decoder enforces several safety limits to protect against malicious
 * input:
 *
 * - **max_output_bytes**: Caps total decompressed output. Checked before each
 *   byte is emitted via `deflate_check_output_limit()`.
 *
 * - **max_memory_bytes**: Caps working memory (state, window, Huffman tables).
 *   Checked at initialization and when building dynamic Huffman tables.
 *
 * - **max_expansion_ratio**: Caps the output/input ratio to protect against
 *   decompression bombs. Both `total_input_bytes` and `total_output_bytes` are
 *   tracked throughout decoding. The ratio check is integrated into
 *   `deflate_check_output_limit()` so it runs on every output operation.
 *
 * ## Input Tracking for Expansion Ratio
 *
 * Input bytes are tracked in two places:
 *
 * 1. `deflate_try_fill_bits()`: Increments `total_input_bytes` for each byte
 *    read into the bit buffer (used for Huffman-compressed blocks).
 *
 * 2. `deflate_copy_stored()`: Increments `total_input_bytes` for bytes copied
 *    directly from input in stored blocks (no bit-level processing).
 *
 * This ensures accurate tracking regardless of block type.
 *
 * ## Output Limit Check
 *
 * The `deflate_check_output_limit()` function performs both checks:
 * 1. Absolute output limit: `total_output_bytes + add <= max_output_bytes`
 * 2. Expansion ratio: `total_output_bytes + add <= max_expansion_ratio *
 * total_input_bytes`
 *
 * If either check fails, `GCOMP_ERR_LIMIT` is returned with error details set.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "deflate_internal.h"
#include "huffman.h"
#include <ghoti.io/compress/limits.h>
#include <stdint.h>
#include <string.h>

//
// Constants (RFC 1951)
//

#define DEFLATE_WINDOW_BITS_DEFAULT 15u
#define DEFLATE_WINDOW_BITS_MIN 8u
#define DEFLATE_WINDOW_BITS_MAX 15u

#define DEFLATE_MAX_LITLEN_SYMBOLS 288u
#define DEFLATE_MAX_DIST_SYMBOLS 32u

//
// Decoder state machine
//

typedef enum {
  DEFLATE_STAGE_BLOCK_HEADER = 0,
  DEFLATE_STAGE_STORED_LEN,
  DEFLATE_STAGE_STORED_COPY,
  DEFLATE_STAGE_DYNAMIC_HEADER,
  DEFLATE_STAGE_DYNAMIC_CODELEN,
  DEFLATE_STAGE_DYNAMIC_LENGTHS,
  DEFLATE_STAGE_HUFFMAN_DATA,
  DEFLATE_STAGE_DONE,
} gcomp_deflate_decoder_stage_t;

typedef struct gcomp_deflate_decoder_state_s {
  //
  // Allocator (for internal memory operations)
  //
  const gcomp_allocator_t * allocator;

  //
  // Bitstream state (LSB-first)
  //
  uint32_t bit_buffer;
  uint32_t bit_count;

  //
  // Unconsumed bytes tracking
  //
  // When the deflate stream ends, any full bytes remaining in the bit buffer
  // are saved here. Container formats like gzip can retrieve these bytes
  // to use them for their own trailer parsing.
  //
  uint8_t unconsumed_bytes[4]; ///< Saved unconsumed bytes (max 3 + safety)
  uint8_t unconsumed_count;    ///< Number of saved unconsumed bytes

  //
  // Limits and counters for safety checks
  //
  // These limits are read from options at creation time and remain constant.
  // The counters are updated throughout decoding and reset by reset().
  //
  uint64_t max_output_bytes;    ///< Max decompressed output (0 = unlimited)
  uint64_t max_window_bytes;    ///< Max LZ77 window size
  uint64_t max_memory_bytes;    ///< Max working memory (0 = unlimited)
  uint64_t max_expansion_ratio; ///< Max output/input ratio (0 = unlimited)
  uint64_t total_output_bytes;  ///< Decompressed bytes produced so far
  uint64_t total_input_bytes;   ///< Compressed bytes consumed so far

  //
  // Memory tracking
  //
  gcomp_memory_tracker_t mem_tracker;

  //
  // Sliding window
  //
  uint8_t * window;
  size_t window_size;
  size_t window_pos;
  size_t window_filled;

  //
  // Block state
  //
  gcomp_deflate_decoder_stage_t stage;
  uint32_t last_block;
  uint32_t block_type;

  //
  // Stored blocks
  //
  uint32_t stored_remaining;

  //
  // Huffman tables
  //
  gcomp_deflate_huffman_decode_table_t fixed_litlen;
  gcomp_deflate_huffman_decode_table_t fixed_dist;
  int fixed_ready;

  gcomp_deflate_huffman_decode_table_t dyn_litlen;
  gcomp_deflate_huffman_decode_table_t dyn_dist;
  int dyn_ready;

  const gcomp_deflate_huffman_decode_table_t * cur_litlen;
  const gcomp_deflate_huffman_decode_table_t * cur_dist;

  //
  // Pending match copy
  //
  uint32_t match_remaining;
  uint32_t match_distance;

  //
  // Pending literal byte
  // When we decode a literal but the output buffer is full, save it here.
  //
  int pending_literal_valid;
  uint8_t pending_literal_value;

  //
  // Pending length/distance decode state
  // When we've decoded a length code but need more bits for the distance,
  // we save the state here so we can resume on the next update() call.
  //
  int pending_length_valid;      // Non-zero if we have a pending length
  uint32_t pending_length_value; // The decoded length (3..258)
  int pending_dist_valid;    // Non-zero if we have a pending distance symbol
  uint16_t pending_dist_sym; // The decoded distance symbol (0..29)

  //
  // Pending length extra bits state
  // When we've decoded a length symbol (257-285) but couldn't read the extra
  // bits, we save the symbol index here to resume on the next update() call.
  //
  int pending_length_sym_valid; // Non-zero if we have a pending length symbol
  uint8_t pending_length_sym;   // The length symbol index (0..28)

  //
  // Dynamic Huffman build scratch
  //
  // These fields track progress through the multi-step dynamic Huffman table
  // construction process. Because input may arrive in arbitrary chunks, the
  // decoder must be able to pause and resume at any point.
  //
  uint32_t dyn_hlit;          ///< HLIT: # of literal/length codes - 257
  uint32_t dyn_hdist;         ///< HDIST: # of distance codes - 1
  uint32_t dyn_hclen;         ///< HCLEN: # of code length codes - 4
  uint32_t dyn_clen_index;    ///< Progress through code length code lengths
  uint32_t dyn_lengths_index; ///< Progress through lit/len + dist lengths
  uint32_t dyn_lengths_total; ///< Total lengths to decode (dyn_hlit + dyn_hdist)
  uint32_t dyn_prev_len;      ///< Previous length (for repeat code 16)

  // Streaming state for repeat codes (symbols 16, 17, 18)
  //
  // When decoding the code length sequence, symbols 16/17/18 require extra
  // bits after the symbol. If we decode the symbol but don't have enough
  // input for the extra bits, we must save the symbol and resume later.
  //
  // Without this, the decoder would decode a NEW symbol on resume, causing
  // silent stream corruption that manifests as invalid Huffman tables.
  //
  uint8_t dyn_pending_repeat_sym; ///< Saved repeat code (16, 17, or 18)
  int dyn_pending_repeat_valid;   ///< 1 if pending repeat needs processing

  uint8_t dyn_clen_lengths[19];
  uint8_t dyn_litlen_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint8_t dyn_dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];

  gcomp_deflate_huffman_decode_table_t dyn_clen_table;
  int dyn_clen_ready;
} gcomp_deflate_decoder_state_t;

//
// Memory tracking helpers
//

/**
 * @brief Calculate the dynamic memory used by a Huffman decode table.
 *
 * This only counts the long_table allocation, not the embedded arrays.
 */
static size_t huffman_table_dynamic_memory(
    const gcomp_deflate_huffman_decode_table_t * table) {
  if (!table || !table->long_table) {
    return 0;
  }
  return table->long_table_count * sizeof(gcomp_deflate_huffman_fast_entry_t);
}

/**
 * @brief Track Huffman table memory after building.
 */
static void track_huffman_table_alloc(gcomp_deflate_decoder_state_t * st,
    const gcomp_deflate_huffman_decode_table_t * table) {
  if (!st || !table) {
    return;
  }
  size_t mem = huffman_table_dynamic_memory(table);
  if (mem > 0) {
    gcomp_memory_track_alloc(&st->mem_tracker, mem);
  }
}

/**
 * @brief Untrack Huffman table memory before cleanup.
 */
static void track_huffman_table_free(gcomp_deflate_decoder_state_t * st,
    const gcomp_deflate_huffman_decode_table_t * table) {
  if (!st || !table) {
    return;
  }
  size_t mem = huffman_table_dynamic_memory(table);
  if (mem > 0) {
    gcomp_memory_track_free(&st->mem_tracker, mem);
  }
}

//
// Bit helpers: streaming bit reads from gcomp_buffer_t
//

static int deflate_try_fill_bits(gcomp_deflate_decoder_state_t * st,
    gcomp_buffer_t * input, uint32_t want_bits) {
  if (!st || !input) {
    return 0;
  }

  const uint8_t * src = (const uint8_t *)input->data;
  while (st->bit_count < want_bits) {
    if (input->used >= input->size) {
      return 0;
    }
    uint8_t byte = src ? src[input->used] : 0u;
    st->bit_buffer |= ((uint32_t)byte) << st->bit_count;
    st->bit_count += 8u;
    input->used += 1u;
    st->total_input_bytes += 1u;
  }

  return 1;
}

static int deflate_try_peek_bits(gcomp_deflate_decoder_state_t * st,
    gcomp_buffer_t * input, uint32_t nbits, uint32_t * out) {
  if (!st || !input || !out || nbits == 0u || nbits > 24u) {
    return 0;
  }

  if (!deflate_try_fill_bits(st, input, nbits)) {
    return 0;
  }

  uint32_t mask = (1u << nbits) - 1u;
  *out = st->bit_buffer & mask;
  return 1;
}

static int deflate_try_read_bits(gcomp_deflate_decoder_state_t * st,
    gcomp_buffer_t * input, uint32_t nbits, uint32_t * out) {
  if (!st || !input || !out || nbits == 0u || nbits > 32u) {
    return 0;
  }

  if (!deflate_try_fill_bits(st, input, nbits)) {
    return 0;
  }

  // Avoid undefined behavior: (1u << 32) is UB in C.
  uint32_t mask = (nbits == 32u) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
  *out = st->bit_buffer & mask;
  if (nbits == 32u) {
    st->bit_buffer = 0;
  }
  else {
    st->bit_buffer >>= nbits;
  }
  st->bit_count -= nbits;
  return 1;
}

static void deflate_align_to_byte(gcomp_deflate_decoder_state_t * st) {
  if (!st) {
    return;
  }
  uint32_t skip = st->bit_count % 8u;
  if (skip != 0u) {
    st->bit_buffer >>= skip;
    st->bit_count -= skip;
  }
}

//
// Bit reversal (needed because DEFLATE transmits Huffman codes LSB-first)
//

static uint32_t reverse_bits(uint32_t v, uint32_t nbits) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < nbits; i++) {
    r = (r << 1u) | (v & 1u);
    v >>= 1u;
  }
  return r;
}

//
// Output helpers (window + limits)
//

/**
 * @brief Check if emitting `add` more output bytes would exceed any limit.
 *
 * This function performs two checks before allowing output:
 *
 * 1. **Absolute output limit**: Ensures `total_output_bytes + add` does not
 *    exceed `max_output_bytes`. This caps the total decompressed size.
 *
 * 2. **Expansion ratio limit**: Ensures `(total_output_bytes + add) /
 * total_input_bytes` does not exceed `max_expansion_ratio`. This catches
 * decompression bombs where a tiny input expands to massive output.
 *
 * The expansion ratio check is performed via
 * `gcomp_limits_check_expansion_ratio()` which handles edge cases like zero
 * input and arithmetic overflow.
 *
 * @param st Decoder state (contains limits and counters)
 * @param add Number of bytes about to be emitted
 * @return GCOMP_OK if within limits, GCOMP_ERR_LIMIT if either limit exceeded
 */
static gcomp_status_t deflate_check_output_limit(
    gcomp_deflate_decoder_state_t * st, size_t add) {
  if (!st) {
    return GCOMP_ERR_INTERNAL;
  }

  // Check for counter overflow (extremely unlikely but defensive)
  if (add > 0 && st->total_output_bytes > UINT64_MAX - (uint64_t)add) {
    return GCOMP_ERR_LIMIT;
  }

  uint64_t next = st->total_output_bytes + (uint64_t)add;

  // Check absolute output limit
  gcomp_status_t status =
      gcomp_limits_check_output((size_t)next, st->max_output_bytes);
  if (status != GCOMP_OK) {
    return status;
  }

  // Check expansion ratio limit (decompression bomb protection)
  return gcomp_limits_check_expansion_ratio(
      st->total_input_bytes, next, st->max_expansion_ratio);
}

static void deflate_window_put(gcomp_deflate_decoder_state_t * st, uint8_t b) {
  if (!st || !st->window || st->window_size == 0) {
    return;
  }

  st->window[st->window_pos] = b;
  st->window_pos = (st->window_pos + 1u) % st->window_size;
  if (st->window_filled < st->window_size) {
    st->window_filled += 1u;
  }
}

static int deflate_out_available(const gcomp_buffer_t * output) {
  if (!output) {
    return 0;
  }
  return (output->used < output->size) ? 1 : 0;
}

static gcomp_status_t deflate_emit_byte(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * output, uint8_t b) {
  if (!st || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!deflate_out_available(output)) {
    // Output buffer full - save the literal for the next call
    st->pending_literal_valid = 1;
    st->pending_literal_value = b;
    return GCOMP_OK;
  }

  gcomp_status_t lim = deflate_check_output_limit(st, 1u);
  if (lim != GCOMP_OK) {
    return lim;
  }

  uint8_t * dst = (uint8_t *)output->data;
  if (dst) {
    dst[output->used] = b;
  }
  output->used += 1u;
  st->total_output_bytes += 1u;
  deflate_window_put(st, b);
  return GCOMP_OK;
}

static gcomp_status_t deflate_copy_stored(gcomp_deflate_decoder_state_t * st,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!st || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (st->stored_remaining == 0) {
    return GCOMP_OK;
  }

  size_t in_avail = input->size - input->used;
  size_t out_avail = output->size - output->used;
  size_t to_copy = st->stored_remaining;
  if (to_copy > in_avail) {
    to_copy = in_avail;
  }
  if (to_copy > out_avail) {
    to_copy = out_avail;
  }

  if (to_copy == 0) {
    return GCOMP_OK;
  }

  // Track input consumption before the output limit check (for accurate ratio)
  st->total_input_bytes += (uint64_t)to_copy;

  gcomp_status_t lim = deflate_check_output_limit(st, to_copy);
  if (lim != GCOMP_OK) {
    // Rollback input tracking since we're not actually consuming it
    st->total_input_bytes -= (uint64_t)to_copy;
    return lim;
  }

  const uint8_t * src = (const uint8_t *)input->data;
  uint8_t * dst = (uint8_t *)output->data;
  if (src && dst) {
    memcpy(dst + output->used, src + input->used, to_copy);
  }

  for (size_t i = 0; i < to_copy; i++) {
    uint8_t b = src ? src[input->used + i] : 0u;
    deflate_window_put(st, b);
  }

  input->used += to_copy;
  output->used += to_copy;
  st->total_output_bytes += (uint64_t)to_copy;
  st->stored_remaining -= (uint32_t)to_copy;
  return GCOMP_OK;
}

static gcomp_status_t deflate_copy_match(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * output) {
  if (!st || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  while (st->match_remaining > 0 && deflate_out_available(output)) {
    if (st->match_distance == 0 ||
        st->match_distance > (uint32_t)st->window_filled ||
        st->match_distance > (uint32_t)st->window_size) {
      return GCOMP_ERR_CORRUPT;
    }

    size_t src_pos =
        (st->window_pos + st->window_size - (size_t)st->match_distance) %
        st->window_size;
    uint8_t b = st->window ? st->window[src_pos] : 0u;

    gcomp_status_t st_out = deflate_emit_byte(st, output, b);
    if (st_out != GCOMP_OK) {
      return st_out;
    }

    st->match_remaining -= 1u;
  }

  return GCOMP_OK;
}

//
// Huffman decode helpers
//

/**
 * @brief Decode a Huffman symbol from the bit stream using two-level lookup.
 *
 * This function implements the fast Huffman decoding algorithm described in
 * huffman.h. The algorithm works as follows:
 *
 * 1. **Peek FAST_BITS** (9) bits from the bit buffer (LSB-first).
 * 2. **Reverse** the bits to convert from stream order to canonical code order.
 * 3. **Fast table lookup**: If fast_table[idx].nbits > 0, we found a short
 * code; emit the symbol and consume nbits bits. Done.
 * 4. **Long code path**: If nbits == 0, read long_extra_bits[idx] more bits,
 *    reverse all (FAST_BITS + extra) bits to get the full canonical code,
 *    extract the low bits, and look up in long_table[long_base[idx] + low].
 *    Emit the symbol and consume long_table entry's nbits bits.
 *
 * **Bit reversal rationale**: DEFLATE writes codes LSB-first, but canonical
 * Huffman codes are defined MSB-first. The fast table is indexed by the
 * canonical code (left-aligned in FAST_BITS). Reversing the peeked bits
 * converts the stream's LSB-first representation back to canonical form.
 *
 * **Partial input handling**: If we don't have enough bits for the code, we
 * return GCOMP_OK with *decoded_out = 0. The caller should provide more input
 * and retry.
 *
 * @param st          Decoder state (contains bit buffer).
 * @param input       Input buffer to refill bits from.
 * @param table       Huffman decode table (from huffman.c).
 * @param sym_out     Output: decoded symbol (valid only if *decoded_out == 1).
 * @param decoded_out Output: 1 if symbol was decoded, 0 if more input needed.
 * @return GCOMP_OK on success or need-more-input, GCOMP_ERR_CORRUPT if the
 *         bit pattern doesn't match any valid code.
 */
static gcomp_status_t deflate_huff_decode_symbol(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input,
    const gcomp_deflate_huffman_decode_table_t * table, uint16_t * sym_out,
    int * decoded_out) {
  if (!st || !input || !table || !sym_out || !decoded_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *decoded_out = 0;

  // Try to fill the bit buffer with FAST_BITS bits. This may not succeed if
  // input is exhausted, but we might still have enough bits for a short code.
  (void)deflate_try_fill_bits(st, input, GCOMP_DEFLATE_HUFFMAN_FAST_BITS);

  // If we have no bits at all, we need more input.
  if (st->bit_count == 0) {
    return GCOMP_OK;
  }

  // Peek whatever bits we have, padding with zeros if needed. The fast table
  // is designed so that short codes at index (code << (FAST_BITS - len)) work
  // correctly even with partial bits.
  uint32_t avail_bits = (st->bit_count > GCOMP_DEFLATE_HUFFMAN_FAST_BITS)
      ? GCOMP_DEFLATE_HUFFMAN_FAST_BITS
      : st->bit_count;
  uint32_t peek = st->bit_buffer & ((1u << avail_bits) - 1u);

  uint32_t idx = reverse_bits(peek, avail_bits);
  // Shift idx to align with FAST_BITS indexing.
  idx <<= (GCOMP_DEFLATE_HUFFMAN_FAST_BITS - avail_bits);

  gcomp_deflate_huffman_fast_entry_t fe = table->fast_table[idx];

  if (fe.nbits > 0) {
    // Check if we have enough bits to actually read this code.
    if (st->bit_count < fe.nbits) {
      return GCOMP_OK; // Need more input
    }
    st->bit_buffer >>= fe.nbits;
    st->bit_count -= fe.nbits;
    *sym_out = fe.symbol;
    *decoded_out = 1;
    return GCOMP_OK;
  }

  uint32_t extra = table->long_extra_bits[idx];
  if (extra == 0u || !table->long_table) {
    return GCOMP_ERR_CORRUPT;
  }

  uint32_t full_bits = GCOMP_DEFLATE_HUFFMAN_FAST_BITS + extra;
  uint32_t full_peek = 0;
  if (!deflate_try_peek_bits(st, input, full_bits, &full_peek)) {
    return GCOMP_OK;
  }

  uint32_t full_rev = reverse_bits(full_peek, full_bits);
  uint32_t low_mask = (1u << extra) - 1u;
  uint32_t low = full_rev & low_mask;
  size_t long_idx = (size_t)table->long_base[idx] + (size_t)low;
  if (long_idx >= table->long_table_count) {
    return GCOMP_ERR_CORRUPT;
  }

  gcomp_deflate_huffman_fast_entry_t le = table->long_table[long_idx];
  if (le.nbits == 0u) {
    return GCOMP_ERR_CORRUPT;
  }

  uint32_t dummy = 0;
  if (!deflate_try_read_bits(st, input, le.nbits, &dummy)) {
    return GCOMP_OK;
  }

  *sym_out = le.symbol;
  *decoded_out = 1;
  return GCOMP_OK;
}

//
// Fixed Huffman tables (RFC 1951, 3.2.6)
//

static gcomp_status_t deflate_build_fixed_tables(
    gcomp_deflate_decoder_state_t * st) {
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint8_t litlen_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint8_t dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];

  memset(litlen_lengths, 0, sizeof(litlen_lengths));
  for (uint32_t i = 0; i <= 143u; i++) {
    litlen_lengths[i] = 8u;
  }
  for (uint32_t i = 144u; i <= 255u; i++) {
    litlen_lengths[i] = 9u;
  }
  for (uint32_t i = 256u; i <= 279u; i++) {
    litlen_lengths[i] = 7u;
  }
  for (uint32_t i = 280u; i <= 287u; i++) {
    litlen_lengths[i] = 8u;
  }

  memset(dist_lengths, 0, sizeof(dist_lengths));
  for (uint32_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
    dist_lengths[i] = 5u;
  }

  gcomp_status_t a = gcomp_deflate_huffman_build_decode_table(st->allocator,
      litlen_lengths, DEFLATE_MAX_LITLEN_SYMBOLS, 15u, &st->fixed_litlen);
  if (a != GCOMP_OK) {
    return a;
  }

  gcomp_status_t b = gcomp_deflate_huffman_build_decode_table(st->allocator,
      dist_lengths, DEFLATE_MAX_DIST_SYMBOLS, 15u, &st->fixed_dist);
  if (b != GCOMP_OK) {
    gcomp_deflate_huffman_decode_table_cleanup(&st->fixed_litlen);
    return b;
  }

  st->fixed_ready = 1;
  return GCOMP_OK;
}

//
// Dynamic Huffman header parsing (RFC 1951, 3.2.7)
//

static const uint8_t k_code_length_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static gcomp_status_t deflate_dynamic_reset(
    gcomp_deflate_decoder_state_t * st) {
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }

  st->dyn_hlit = 0;
  st->dyn_hdist = 0;
  st->dyn_hclen = 0;
  st->dyn_clen_index = 0;
  st->dyn_lengths_index = 0;
  st->dyn_lengths_total = 0;
  st->dyn_prev_len = 0;
  st->dyn_pending_repeat_sym = 0;
  st->dyn_pending_repeat_valid = 0;

  memset(st->dyn_clen_lengths, 0, sizeof(st->dyn_clen_lengths));
  memset(st->dyn_litlen_lengths, 0, sizeof(st->dyn_litlen_lengths));
  memset(st->dyn_dist_lengths, 0, sizeof(st->dyn_dist_lengths));

  if (st->dyn_clen_ready) {
    track_huffman_table_free(st, &st->dyn_clen_table);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_clen_table);
    st->dyn_clen_ready = 0;
  }

  if (st->dyn_ready) {
    track_huffman_table_free(st, &st->dyn_litlen);
    track_huffman_table_free(st, &st->dyn_dist);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_dist);
    st->dyn_ready = 0;
  }

  return GCOMP_OK;
}

static gcomp_status_t deflate_dynamic_read_header(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input) {
  if (!st || !input) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Read all 14 bits (5+5+4) atomically to avoid partial-read state bugs.
  uint32_t header = 0;
  if (!deflate_try_read_bits(st, input, 14u, &header)) {
    return GCOMP_OK;
  }

  uint32_t hlit = header & 0x1Fu;
  uint32_t hdist = (header >> 5u) & 0x1Fu;
  uint32_t hclen = (header >> 10u) & 0x0Fu;

  st->dyn_hlit = hlit + 257u;
  st->dyn_hdist = hdist + 1u;
  st->dyn_hclen = hclen + 4u;

  if (st->dyn_hlit > 286u || st->dyn_hdist > 32u || st->dyn_hclen > 19u) {
    return GCOMP_ERR_CORRUPT;
  }

  st->dyn_clen_index = 0;
  return GCOMP_OK;
}

static gcomp_status_t deflate_dynamic_read_codelen_lengths(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input) {
  if (!st || !input) {
    return GCOMP_ERR_INVALID_ARG;
  }

  while (st->dyn_clen_index < st->dyn_hclen) {
    uint32_t v = 0;
    if (!deflate_try_read_bits(st, input, 3u, &v)) {
      return GCOMP_OK;
    }
    uint32_t sym = k_code_length_order[st->dyn_clen_index];
    st->dyn_clen_lengths[sym] = (uint8_t)v;
    st->dyn_clen_index += 1u;
  }

  gcomp_status_t st_build = gcomp_deflate_huffman_build_decode_table(
      st->allocator, st->dyn_clen_lengths, 19u, 7u, &st->dyn_clen_table);
  if (st_build != GCOMP_OK) {
    return st_build == GCOMP_ERR_CORRUPT ? GCOMP_ERR_CORRUPT : st_build;
  }

  // Track clen table memory allocation
  track_huffman_table_alloc(st, &st->dyn_clen_table);

  // Check memory limit after allocation
  gcomp_status_t mem_check =
      gcomp_memory_check_limit(&st->mem_tracker, st->max_memory_bytes);
  if (mem_check != GCOMP_OK) {
    track_huffman_table_free(st, &st->dyn_clen_table);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_clen_table);
    return mem_check;
  }

  st->dyn_clen_ready = 1;
  st->dyn_lengths_total = st->dyn_hlit + st->dyn_hdist;
  st->dyn_lengths_index = 0;
  st->dyn_prev_len = 0;
  return GCOMP_OK;
}

/**
 * @brief Decode literal/length and distance code lengths for dynamic Huffman.
 *
 * This function decodes the code length sequences that define the dynamic
 * Huffman tables. RFC 1951 uses a compact encoding with repeat codes:
 *
 * - Symbols 0-15: Literal code lengths (0 = unused symbol)
 * - Symbol 16: Repeat previous length 3-6 times (2 extra bits)
 * - Symbol 17: Repeat zero 3-10 times (3 extra bits)
 * - Symbol 18: Repeat zero 11-138 times (7 extra bits)
 *
 * ## Streaming State Preservation for Repeat Codes
 *
 * Repeat codes (16, 17, 18) are two-part symbols: first the symbol itself
 * is Huffman-decoded, then extra bits are read to determine the repeat count.
 * In streaming mode, the input buffer may run out between these two steps.
 *
 * **The Problem**: If we successfully decode symbol 16 but can't read its
 * 2 extra bits, we must preserve the symbol for the next update() call.
 * Simply returning GCOMP_OK would cause us to decode a NEW symbol next time,
 * corrupting the stream.
 *
 * **The Solution**: Use `dyn_pending_repeat_sym` and `dyn_pending_repeat_valid`
 * to save the repeat symbol when we can't read its extra bits:
 *
 * 1. Successfully decode symbol (e.g., sym=16)
 * 2. Attempt to read extra bits → not enough input
 * 3. Save: `dyn_pending_repeat_sym = 16`, `dyn_pending_repeat_valid = 1`
 * 4. Return GCOMP_OK (need more input)
 * 5. On next call, check `dyn_pending_repeat_valid` first
 * 6. If set, use saved symbol instead of decoding a new one
 * 7. Clear flag and attempt to read extra bits again
 *
 * This pattern is consistent with other pending state mechanisms in the
 * decoder (e.g., `pending_literal_valid`, `pending_length_valid`) and
 * ensures correct resumption across arbitrary input buffer boundaries.
 *
 * @param st Decoder state
 * @param input Input buffer (may be partially consumed)
 * @return GCOMP_OK if progress made (may need more input), error otherwise
 */
static gcomp_status_t deflate_dynamic_decode_lengths(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input) {
  if (!st || !input) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!st->dyn_clen_ready) {
    return GCOMP_ERR_INTERNAL;
  }

  while (st->dyn_lengths_index < st->dyn_lengths_total) {
    uint16_t sym = 0;

    // Resume from a pending repeat code if we couldn't read its extra bits
    // on the previous call. See function documentation for details.
    if (st->dyn_pending_repeat_valid) {
      sym = st->dyn_pending_repeat_sym;
      st->dyn_pending_repeat_valid = 0;
    }
    else {
      // Decode a new symbol
      int decoded = 0;
      gcomp_status_t ds = deflate_huff_decode_symbol(
          st, input, &st->dyn_clen_table, &sym, &decoded);
      if (ds != GCOMP_OK) {
        return ds;
      }

      // Not enough input to decode a symbol.
      if (!decoded) {
        return GCOMP_OK;
      }
    }

    if (sym <= 15u) {
      uint8_t len = (uint8_t)sym;
      uint32_t idx = st->dyn_lengths_index;
      if (idx < st->dyn_hlit) {
        st->dyn_litlen_lengths[idx] = len;
      }
      else {
        st->dyn_dist_lengths[idx - st->dyn_hlit] = len;
      }
      st->dyn_prev_len = len;
      st->dyn_lengths_index += 1u;
      continue;
    }

    if (sym == 16u) {
      if (st->dyn_lengths_index == 0u) {
        return GCOMP_ERR_CORRUPT;
      }
      uint32_t extra = 0;
      if (!deflate_try_read_bits(st, input, 2u, &extra)) {
        // Not enough bits for extra data - save symbol and wait for more input
        st->dyn_pending_repeat_sym = (uint8_t)sym;
        st->dyn_pending_repeat_valid = 1;
        return GCOMP_OK;
      }
      uint32_t count = 3u + extra;
      if (st->dyn_lengths_index + count > st->dyn_lengths_total) {
        return GCOMP_ERR_CORRUPT;
      }
      for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = st->dyn_lengths_index;
        if (idx < st->dyn_hlit) {
          st->dyn_litlen_lengths[idx] = (uint8_t)st->dyn_prev_len;
        }
        else {
          st->dyn_dist_lengths[idx - st->dyn_hlit] = (uint8_t)st->dyn_prev_len;
        }
        st->dyn_lengths_index += 1u;
      }
      continue;
    }

    if (sym == 17u || sym == 18u) {
      uint32_t extra_bits = (sym == 17u) ? 3u : 7u;
      uint32_t extra = 0;
      if (!deflate_try_read_bits(st, input, extra_bits, &extra)) {
        // Not enough bits for extra data - save symbol and wait for more input
        st->dyn_pending_repeat_sym = (uint8_t)sym;
        st->dyn_pending_repeat_valid = 1;
        return GCOMP_OK;
      }
      uint32_t base = (sym == 17u) ? 3u : 11u;
      uint32_t count = base + extra;
      if (st->dyn_lengths_index + count > st->dyn_lengths_total) {
        return GCOMP_ERR_CORRUPT;
      }
      for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = st->dyn_lengths_index;
        if (idx < st->dyn_hlit) {
          st->dyn_litlen_lengths[idx] = 0;
        }
        else {
          st->dyn_dist_lengths[idx - st->dyn_hlit] = 0;
        }
        st->dyn_lengths_index += 1u;
      }
      st->dyn_prev_len = 0;
      continue;
    }

    return GCOMP_ERR_CORRUPT;
  }

  // 256 (end-of-block) must exist.
  if (st->dyn_litlen_lengths[256] == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  // Note: Distance tree CAN be empty (all zero code lengths) if no distance
  // codes are used in the block. This occurs when the encoder outputs only
  // literals and no LZ77 matches (e.g., incompressible data or very short
  // inputs). RFC 1951 permits this: the distance tree is only accessed when
  // decoding a length code (257-285), and if no such codes appear in the
  // compressed data, an empty distance tree is valid. We only reject streams
  // where the lit/len tree is incomplete (missing end-of-block symbol 256).

  gcomp_status_t a = gcomp_deflate_huffman_build_decode_table(st->allocator,
      st->dyn_litlen_lengths, DEFLATE_MAX_LITLEN_SYMBOLS, 15u, &st->dyn_litlen);
  if (a != GCOMP_OK) {
    return a;
  }
  // Track litlen table memory allocation
  track_huffman_table_alloc(st, &st->dyn_litlen);

  gcomp_status_t b = gcomp_deflate_huffman_build_decode_table(st->allocator,
      st->dyn_dist_lengths, DEFLATE_MAX_DIST_SYMBOLS, 15u, &st->dyn_dist);
  if (b != GCOMP_OK) {
    track_huffman_table_free(st, &st->dyn_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_litlen);
    return b;
  }
  // Track dist table memory allocation
  track_huffman_table_alloc(st, &st->dyn_dist);

  // Check memory limit after allocations
  gcomp_status_t mem_check =
      gcomp_memory_check_limit(&st->mem_tracker, st->max_memory_bytes);
  if (mem_check != GCOMP_OK) {
    track_huffman_table_free(st, &st->dyn_litlen);
    track_huffman_table_free(st, &st->dyn_dist);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_dist);
    return mem_check;
  }

  st->dyn_ready = 1;
  // Clean up clen table (no longer needed)
  track_huffman_table_free(st, &st->dyn_clen_table);
  gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_clen_table);
  st->dyn_clen_ready = 0;
  return GCOMP_OK;
}

//
// Length/Distance decoding tables
//

static const uint16_t k_len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
    19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
    2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

static const uint16_t k_dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33,
    49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5,
    5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

//
// Public hooks (called from deflate_register.c)
//

gcomp_status_t gcomp_deflate_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  if (!registry || !decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  // Read memory limit early to check before allocations
  uint64_t max_mem =
      gcomp_limits_read_memory_max(options, GCOMP_DEFAULT_MAX_MEMORY_BYTES);

  // Calculate initial memory requirement: state struct + window
  uint64_t win_bits = DEFLATE_WINDOW_BITS_DEFAULT;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "deflate.window_bits", &v) ==
        GCOMP_OK) {
      win_bits = v;
    }
  }

  if (win_bits < DEFLATE_WINDOW_BITS_MIN ||
      win_bits > DEFLATE_WINDOW_BITS_MAX) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t window_size = (size_t)1u << (size_t)win_bits;
  size_t initial_mem = sizeof(gcomp_deflate_decoder_state_t) + window_size;

  // Check memory limit before allocating
  if (max_mem != 0 && initial_mem > max_mem) {
    return GCOMP_ERR_LIMIT;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)gcomp_calloc(
          alloc, 1, sizeof(gcomp_deflate_decoder_state_t));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }

  // Store allocator for internal use
  st->allocator = alloc;

  // Initialize memory tracker and track state struct allocation
  st->mem_tracker.current_bytes = 0;
  gcomp_memory_track_alloc(
      &st->mem_tracker, sizeof(gcomp_deflate_decoder_state_t));
  st->max_memory_bytes = max_mem;

  st->bit_buffer = 0;
  st->bit_count = 0;
  st->unconsumed_count = 0;
  memset(st->unconsumed_bytes, 0, sizeof(st->unconsumed_bytes));
  st->stage = DEFLATE_STAGE_BLOCK_HEADER;
  st->last_block = 0;
  st->block_type = 0;
  st->stored_remaining = 0;
  st->match_remaining = 0;
  st->match_distance = 0;
  st->pending_length_valid = 0;
  st->pending_length_value = 0;
  st->pending_dist_valid = 0;
  st->pending_dist_sym = 0;
  st->pending_length_sym_valid = 0;
  st->pending_length_sym = 0;

  st->window_size = window_size;
  st->max_window_bytes =
      gcomp_limits_read_window_max(options, (uint64_t)st->window_size);
  if (st->max_window_bytes != 0 &&
      (uint64_t)st->window_size > st->max_window_bytes) {
    gcomp_free(alloc, st);
    return GCOMP_ERR_LIMIT;
  }

  st->window = (uint8_t *)gcomp_malloc(alloc, st->window_size);
  if (!st->window) {
    gcomp_free(alloc, st);
    return GCOMP_ERR_MEMORY;
  }
  // Track window allocation
  gcomp_memory_track_alloc(&st->mem_tracker, st->window_size);

  st->window_pos = 0;
  st->window_filled = 0;

  st->max_output_bytes =
      gcomp_limits_read_output_max(options, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);
  st->max_expansion_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_DEFAULT_MAX_EXPANSION_RATIO);
  st->total_output_bytes = 0;
  st->total_input_bytes = 0;

  st->fixed_ready = 0;
  st->dyn_ready = 0;
  st->dyn_clen_ready = 0;
  memset(&st->fixed_litlen, 0, sizeof(st->fixed_litlen));
  memset(&st->fixed_dist, 0, sizeof(st->fixed_dist));
  memset(&st->dyn_litlen, 0, sizeof(st->dyn_litlen));
  memset(&st->dyn_dist, 0, sizeof(st->dyn_dist));
  memset(&st->dyn_clen_table, 0, sizeof(st->dyn_clen_table));

  gcomp_status_t ft = deflate_build_fixed_tables(st);
  if (ft != GCOMP_OK) {
    gcomp_free(alloc, st->window);
    gcomp_free(alloc, st);
    return ft;
  }
  // Track fixed Huffman table allocations
  track_huffman_table_alloc(st, &st->fixed_litlen);
  track_huffman_table_alloc(st, &st->fixed_dist);

  st->cur_litlen = NULL;
  st->cur_dist = NULL;

  decoder->method_state = st;
  decoder->update_fn = gcomp_deflate_decoder_update;
  decoder->finish_fn = gcomp_deflate_decoder_finish;
  decoder->reset_fn = gcomp_deflate_decoder_reset;
  return GCOMP_OK;
}

void gcomp_deflate_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder) {
    return;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  if (!st) {
    return;
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(decoder->registry);

  if (st->fixed_ready) {
    track_huffman_table_free(st, &st->fixed_litlen);
    track_huffman_table_free(st, &st->fixed_dist);
    gcomp_deflate_huffman_decode_table_cleanup(&st->fixed_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->fixed_dist);
  }

  if (st->dyn_ready) {
    track_huffman_table_free(st, &st->dyn_litlen);
    track_huffman_table_free(st, &st->dyn_dist);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_dist);
  }

  if (st->dyn_clen_ready) {
    track_huffman_table_free(st, &st->dyn_clen_table);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_clen_table);
  }

  gcomp_memory_track_free(&st->mem_tracker, st->window_size);
  gcomp_free(alloc, st->window);

  gcomp_memory_track_free(
      &st->mem_tracker, sizeof(gcomp_deflate_decoder_state_t));
  gcomp_free(alloc, st);
  decoder->method_state = NULL;
}

gcomp_status_t gcomp_deflate_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  if (!st) {
    return GCOMP_ERR_INTERNAL;
  }

  // Reset bit buffer state
  st->bit_buffer = 0;
  st->bit_count = 0;
  st->unconsumed_count = 0;

  // Reset state machine
  st->stage = DEFLATE_STAGE_BLOCK_HEADER;
  st->last_block = 0;
  st->block_type = 0;
  st->stored_remaining = 0;

  // Reset window state (keep buffer allocated)
  st->window_pos = 0;
  st->window_filled = 0;
  st->total_output_bytes = 0;
  st->total_input_bytes = 0;

  // Reset pending match/literal state
  st->match_remaining = 0;
  st->match_distance = 0;
  st->pending_literal_valid = 0;
  st->pending_literal_value = 0;
  st->pending_length_valid = 0;
  st->pending_length_value = 0;
  st->pending_dist_valid = 0;
  st->pending_dist_sym = 0;
  st->pending_length_sym_valid = 0;
  st->pending_length_sym = 0;

  // Clean up dynamic Huffman tables (keep fixed tables - they can be reused)
  if (st->dyn_ready) {
    track_huffman_table_free(st, &st->dyn_litlen);
    track_huffman_table_free(st, &st->dyn_dist);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_dist);
    memset(&st->dyn_litlen, 0, sizeof(st->dyn_litlen));
    memset(&st->dyn_dist, 0, sizeof(st->dyn_dist));
    st->dyn_ready = 0;
  }

  if (st->dyn_clen_ready) {
    track_huffman_table_free(st, &st->dyn_clen_table);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_clen_table);
    memset(&st->dyn_clen_table, 0, sizeof(st->dyn_clen_table));
    st->dyn_clen_ready = 0;
  }

  // Reset dynamic Huffman build scratch
  st->dyn_hlit = 0;
  st->dyn_hdist = 0;
  st->dyn_hclen = 0;
  st->dyn_clen_index = 0;
  st->dyn_lengths_index = 0;
  st->dyn_lengths_total = 0;
  st->dyn_prev_len = 0;
  st->dyn_pending_repeat_sym = 0;
  st->dyn_pending_repeat_valid = 0;

  // Clear current table pointers
  st->cur_litlen = NULL;
  st->cur_dist = NULL;

  return GCOMP_OK;
}

static gcomp_status_t deflate_process_block_header(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input) {
  uint32_t bfinal = 0;
  uint32_t btype = 0;

  if (!deflate_try_read_bits(st, input, 1u, &bfinal)) {
    return GCOMP_OK;
  }
  if (!deflate_try_read_bits(st, input, 2u, &btype)) {
    return GCOMP_OK;
  }

  st->last_block = bfinal;
  st->block_type = btype;

  if (btype == 0u) {
    deflate_align_to_byte(st);
    st->stage = DEFLATE_STAGE_STORED_LEN;
  }
  else if (btype == 1u) {
    st->cur_litlen = &st->fixed_litlen;
    st->cur_dist = &st->fixed_dist;
    st->stage = DEFLATE_STAGE_HUFFMAN_DATA;
  }
  else if (btype == 2u) {
    gcomp_status_t rs = deflate_dynamic_reset(st);
    if (rs != GCOMP_OK) {
      return rs;
    }
    st->stage = DEFLATE_STAGE_DYNAMIC_HEADER;
  }
  else {
    return GCOMP_ERR_CORRUPT;
  }

  return GCOMP_OK;
}

static gcomp_status_t deflate_process_stored_len(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input) {
  // Read all 32 bits (LEN + NLEN) atomically to avoid partial-read bugs.
  uint32_t len_nlen = 0;
  if (!deflate_try_read_bits(st, input, 32u, &len_nlen)) {
    return GCOMP_OK;
  }

  uint32_t len = len_nlen & 0xFFFFu;
  uint32_t nlen = (len_nlen >> 16u) & 0xFFFFu;

  if (((len ^ 0xFFFFu) & 0xFFFFu) != nlen) {
    return GCOMP_ERR_CORRUPT;
  }

  st->stored_remaining = len;
  st->stage = DEFLATE_STAGE_STORED_COPY;
  return GCOMP_OK;
}

/**
 * @brief Helper to decode distance and set up match after length is known.
 *
 * This is called either with a freshly decoded length, or when resuming
 * from a pending length (where we had decoded the length but not the distance).
 *
 * @return GCOMP_OK if match is set up or we need more input; error otherwise.
 */
static gcomp_status_t deflate_decode_distance(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input,
    gcomp_buffer_t * output, uint32_t length) {
  uint16_t dist_sym = 0;

  // Check if we have a pending distance symbol (we decoded it before but
  // couldn't read its extra bits)
  if (st->pending_dist_valid) {
    dist_sym = st->pending_dist_sym;
  }
  else {
    // Need to decode the distance symbol
    int dist_decoded = 0;
    gcomp_status_t dd = deflate_huff_decode_symbol(
        st, input, st->cur_dist, &dist_sym, &dist_decoded);
    if (dd != GCOMP_OK) {
      return dd;
    }
    if (!dist_decoded) {
      // Save the length so we can resume on next call
      st->pending_length_valid = 1;
      st->pending_length_value = length;
      return GCOMP_OK;
    }
    if (dist_sym >= 30u) {
      return GCOMP_ERR_CORRUPT;
    }
  }

  uint32_t distance = k_dist_base[dist_sym];
  uint32_t de = k_dist_extra[dist_sym];
  if (de > 0) {
    uint32_t extra = 0;
    if (!deflate_try_read_bits(st, input, de, &extra)) {
      // Save both the length and distance symbol so we can resume
      st->pending_length_valid = 1;
      st->pending_length_value = length;
      st->pending_dist_valid = 1;
      st->pending_dist_sym = dist_sym;
      return GCOMP_OK;
    }
    distance += extra;
  }

  if (distance == 0 || distance > (uint32_t)st->window_filled) {
    return GCOMP_ERR_CORRUPT;
  }

  // Clear pending state since we successfully decoded
  st->pending_length_valid = 0;
  st->pending_dist_valid = 0;

  st->match_distance = distance;
  st->match_remaining = length;
  return deflate_copy_match(st, output);
}

static gcomp_status_t deflate_process_huffman_data(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
  if (!st->cur_litlen || !st->cur_dist) {
    return GCOMP_ERR_INTERNAL;
  }

  // Emit any pending literal byte first.
  if (st->pending_literal_valid) {
    if (!deflate_out_available(output)) {
      return GCOMP_OK; // Still no room, wait for more output space
    }
    gcomp_status_t lim = deflate_check_output_limit(st, 1u);
    if (lim != GCOMP_OK) {
      return lim;
    }
    uint8_t * dst = (uint8_t *)output->data;
    if (dst) {
      dst[output->used] = st->pending_literal_value;
    }
    output->used += 1u;
    st->total_output_bytes += 1u;
    deflate_window_put(st, st->pending_literal_value);
    st->pending_literal_valid = 0;
    // Continue to process more data
  }

  // Drain any pending match.
  if (st->match_remaining > 0) {
    return deflate_copy_match(st, output);
  }

  // Resume pending length/distance decode if we have one
  if (st->pending_length_valid) {
    return deflate_decode_distance(st, input, output, st->pending_length_value);
  }

  // Resume pending length symbol decode (waiting for extra bits)
  if (st->pending_length_sym_valid) {
    uint32_t len_sym = st->pending_length_sym;
    uint32_t length = k_len_base[len_sym];
    uint32_t le = k_len_extra[len_sym];
    // le must be > 0 since we only save state when extra bits are needed
    uint32_t extra = 0;
    if (!deflate_try_read_bits(st, input, le, &extra)) {
      // Still can't get extra bits - need more input
      return GCOMP_OK;
    }
    length += extra;
    st->pending_length_sym_valid = 0;
    return deflate_decode_distance(st, input, output, length);
  }

  int decoded = 0;
  uint16_t sym = 0;
  gcomp_status_t ds =
      deflate_huff_decode_symbol(st, input, st->cur_litlen, &sym, &decoded);
  if (ds != GCOMP_OK) {
    return ds;
  }

  // Not enough input to decode a symbol.
  if (!decoded) {
    return GCOMP_OK;
  }

  if (sym < 256u) {
    return deflate_emit_byte(st, output, (uint8_t)sym);
  }

  if (sym == 256u) {
    if (st->last_block) {
      st->stage = DEFLATE_STAGE_DONE;
      // Handle pre-read bytes from the bit buffer for container formats.
      // This is critical for formats like gzip that need to read data
      // (e.g., trailer) immediately after the deflate stream.
      //
      // We use floor division (bit_count / 8) because:
      // - Full bytes (8 bits each) in the buffer are pre-read trailer bytes
      // - Partial byte bits (bit_count % 8) are padding from deflate's last
      // byte
      //
      // Example: if bit_count = 10, we have 1 pre-read byte (8 bits) and
      // 2 padding bits from the last deflate byte.
      //
      // Strategy:
      // - In bulk mode (when bytes can be returned to input), return them.
      //   The container format reads from the input buffer.
      // - In streaming mode (bytes were consumed in previous calls), save
      //   them for retrieval via gcomp_deflate_decoder_get_unconsumed_data().
      //   The container format retrieves them explicitly.
      st->unconsumed_count = 0;
      if (st->bit_count >= 8) {
        uint32_t bytes_to_handle = st->bit_count / 8;
        if (bytes_to_handle > sizeof(st->unconsumed_bytes)) {
          bytes_to_handle = sizeof(st->unconsumed_bytes);
        }
        // Try to return bytes to input buffer (bulk mode)
        if (bytes_to_handle <= input->used) {
          // Bulk mode: bytes can be returned to input
          input->used -= bytes_to_handle;
          st->total_input_bytes -= bytes_to_handle;
          // Don't save to unconsumed_bytes - gzip will read from input
        }
        else {
          // Streaming mode: bytes were consumed in previous calls
          // Save them for explicit retrieval by the container format
          for (uint32_t i = 0; i < bytes_to_handle; i++) {
            st->unconsumed_bytes[i] = (uint8_t)(st->bit_buffer >> (i * 8));
          }
          st->unconsumed_count = (uint8_t)bytes_to_handle;
        }
      }
      // Clear the bit buffer (padding bits are discarded)
      st->bit_buffer = 0;
      st->bit_count = 0;
    }
    else {
      st->stage = DEFLATE_STAGE_BLOCK_HEADER;
    }
    return GCOMP_OK;
  }

  if (sym > 285u) {
    return GCOMP_ERR_CORRUPT;
  }

  // Length code 257..285
  uint32_t len_sym = sym - 257u;
  uint32_t length = k_len_base[len_sym];
  uint32_t le = k_len_extra[len_sym];
  if (le > 0) {
    uint32_t extra = 0;
    if (!deflate_try_read_bits(st, input, le, &extra)) {
      // Can't get length extra bits - need more input.
      // Save the length symbol so we can resume on the next update() call.
      st->pending_length_sym_valid = 1;
      st->pending_length_sym = (uint8_t)len_sym;
      return GCOMP_OK;
    }
    length += extra;
  }

  // Now decode distance
  return deflate_decode_distance(st, input, output, length);
}

// Helper to get stage name for error messages
static const char * deflate_stage_name(gcomp_deflate_decoder_stage_t stage) {
  switch (stage) {
  case DEFLATE_STAGE_BLOCK_HEADER:
    return "block_header";
  case DEFLATE_STAGE_STORED_LEN:
    return "stored_len";
  case DEFLATE_STAGE_STORED_COPY:
    return "stored_copy";
  case DEFLATE_STAGE_DYNAMIC_HEADER:
    return "dynamic_header";
  case DEFLATE_STAGE_DYNAMIC_CODELEN:
    return "dynamic_codelen";
  case DEFLATE_STAGE_DYNAMIC_LENGTHS:
    return "dynamic_lengths";
  case DEFLATE_STAGE_HUFFMAN_DATA:
    return "huffman_data";
  case DEFLATE_STAGE_DONE:
    return "done";
  default:
    return "unknown";
  }
}

gcomp_status_t gcomp_deflate_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INTERNAL, "decoder state is NULL");
  }

  for (;;) {
    if (st->stage == DEFLATE_STAGE_DONE) {
      return GCOMP_OK;
    }

    // Snapshot state so we can detect lack of progress in this iteration.
    size_t prev_in_used = input->used;
    size_t prev_out_used = output->used;
    gcomp_deflate_decoder_stage_t prev_stage = st->stage;
    uint32_t prev_stored = st->stored_remaining;
    uint32_t prev_match = st->match_remaining;
    uint32_t prev_bits = st->bit_count;
    int prev_literal_valid = st->pending_literal_valid;

    gcomp_status_t s = GCOMP_OK;
    switch (st->stage) {
    case DEFLATE_STAGE_BLOCK_HEADER:
      s = deflate_process_block_header(st, input);
      break;
    case DEFLATE_STAGE_STORED_LEN:
      s = deflate_process_stored_len(st, input);
      break;
    case DEFLATE_STAGE_STORED_COPY:
      s = deflate_copy_stored(st, input, output);
      if (s == GCOMP_OK && st->stored_remaining == 0) {
        st->stage =
            st->last_block ? DEFLATE_STAGE_DONE : DEFLATE_STAGE_BLOCK_HEADER;
      }
      break;
    case DEFLATE_STAGE_DYNAMIC_HEADER:
      s = deflate_dynamic_read_header(st, input);
      if (s == GCOMP_OK && st->dyn_hclen != 0 && st->dyn_clen_index == 0) {
        st->stage = DEFLATE_STAGE_DYNAMIC_CODELEN;
      }
      break;
    case DEFLATE_STAGE_DYNAMIC_CODELEN:
      s = deflate_dynamic_read_codelen_lengths(st, input);
      if (s == GCOMP_OK && st->dyn_clen_ready) {
        st->stage = DEFLATE_STAGE_DYNAMIC_LENGTHS;
      }
      break;
    case DEFLATE_STAGE_DYNAMIC_LENGTHS:
      s = deflate_dynamic_decode_lengths(st, input);
      if (s == GCOMP_OK && st->dyn_ready) {
        st->cur_litlen = &st->dyn_litlen;
        st->cur_dist = &st->dyn_dist;
        st->stage = DEFLATE_STAGE_HUFFMAN_DATA;
      }
      break;
    case DEFLATE_STAGE_HUFFMAN_DATA:
      s = deflate_process_huffman_data(st, input, output);
      break;
    case DEFLATE_STAGE_DONE:
      return GCOMP_OK;
    default:
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INTERNAL, "invalid decoder stage %d", st->stage);
    }

    if (s != GCOMP_OK) {
      // Set error details based on error type and stage
      const char * stage_name = deflate_stage_name(prev_stage);
      switch (s) {
      case GCOMP_ERR_CORRUPT:
        return gcomp_decoder_set_error(decoder, s,
            "corrupt deflate stream at stage '%s' (output: %zu bytes)",
            stage_name, st->total_output_bytes);
      case GCOMP_ERR_LIMIT:
        return gcomp_decoder_set_error(decoder, s,
            "limit exceeded at stage '%s' (output: %zu/%zu bytes)", stage_name,
            st->total_output_bytes, (size_t)st->max_output_bytes);
      case GCOMP_ERR_MEMORY:
        return gcomp_decoder_set_error(
            decoder, s, "memory allocation failed at stage '%s'", stage_name);
      default:
        return gcomp_decoder_set_error(
            decoder, s, "error at stage '%s'", stage_name);
      }
    }

    // If this iteration did not consume input, produce output, or change any
    // relevant state, stop to avoid spinning with no progress.
    if (input->used == prev_in_used && output->used == prev_out_used &&
        st->stage == prev_stage && st->stored_remaining == prev_stored &&
        st->match_remaining == prev_match && st->bit_count == prev_bits &&
        st->pending_literal_valid == prev_literal_valid) {
      return GCOMP_OK;
    }
  }
}

gcomp_status_t gcomp_deflate_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INTERNAL, "decoder state is NULL");
  }

  // Drain any pending match with the provided output space.
  if (st->match_remaining > 0) {
    gcomp_status_t s = deflate_copy_match(st, output);
    if (s != GCOMP_OK) {
      return gcomp_decoder_set_error(decoder, s,
          "error draining pending match (%u bytes remaining)",
          st->match_remaining);
    }
  }

  if (st->stage != DEFLATE_STAGE_DONE) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "incomplete deflate stream (stage '%s', expected final block)",
        deflate_stage_name(st->stage));
  }

  return GCOMP_OK;
}

int gcomp_deflate_decoder_is_done(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return 0;
  }
  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  return st->stage == DEFLATE_STAGE_DONE;
}

uint32_t gcomp_deflate_decoder_get_unconsumed_bytes(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return 0;
  }
  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  // Return the count of saved unconsumed bytes
  return st->unconsumed_count;
}

uint32_t gcomp_deflate_decoder_get_unconsumed_data(
    gcomp_decoder_t * decoder, uint8_t * buf, uint32_t buf_size) {
  if (!decoder || !decoder->method_state || !buf || buf_size == 0) {
    return 0;
  }
  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;

  uint32_t to_copy = st->unconsumed_count;
  if (to_copy > buf_size) {
    to_copy = buf_size;
  }

  for (uint32_t i = 0; i < to_copy; i++) {
    buf[i] = st->unconsumed_bytes[i];
  }

  return to_copy;
}
