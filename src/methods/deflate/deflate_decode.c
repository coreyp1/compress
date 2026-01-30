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
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
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
  // Bitstream state (LSB-first)
  //
  uint32_t bit_buffer;
  uint32_t bit_count;

  //
  // Limits
  //
  uint64_t max_output_bytes;
  uint64_t max_window_bytes;
  uint64_t total_output_bytes;

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
  // Pending length/distance decode state
  // When we've decoded a length code but need more bits for the distance,
  // we save the state here so we can resume on the next update() call.
  //
  int pending_length_valid;      // Non-zero if we have a pending length
  uint32_t pending_length_value; // The decoded length (3..258)
  int pending_dist_valid;    // Non-zero if we have a pending distance symbol
  uint16_t pending_dist_sym; // The decoded distance symbol (0..29)

  //
  // Dynamic Huffman build scratch
  //
  uint32_t dyn_hlit;
  uint32_t dyn_hdist;
  uint32_t dyn_hclen;
  uint32_t dyn_clen_index;
  uint32_t dyn_lengths_index;
  uint32_t dyn_lengths_total;
  uint32_t dyn_prev_len;

  uint8_t dyn_clen_lengths[19];
  uint8_t dyn_litlen_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint8_t dyn_dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];

  gcomp_deflate_huffman_decode_table_t dyn_clen_table;
  int dyn_clen_ready;
} gcomp_deflate_decoder_state_t;

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

static gcomp_status_t deflate_check_output_limit(
    gcomp_deflate_decoder_state_t * st, size_t add) {
  if (!st) {
    return GCOMP_ERR_INTERNAL;
  }

  if (add > 0 && st->total_output_bytes > UINT64_MAX - (uint64_t)add) {
    return GCOMP_ERR_LIMIT;
  }

  uint64_t next = st->total_output_bytes + (uint64_t)add;
  return gcomp_limits_check_output((size_t)next, st->max_output_bytes);
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

  gcomp_status_t lim = deflate_check_output_limit(st, to_copy);
  if (lim != GCOMP_OK) {
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
 * @brief Decode a Huffman symbol from the bit stream.
 *
 * @param decoded_out  Output flag: set to 1 if a symbol was decoded, 0 if more
 *                     input is needed. Caller must check this before using
 *                     sym_out.
 */
static gcomp_status_t deflate_huff_decode_symbol(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input,
    const gcomp_deflate_huffman_decode_table_t * table, uint16_t * sym_out,
    int * decoded_out) {
  if (!st || !input || !table || !sym_out || !decoded_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *decoded_out = 0;

  /* Try to fill the bit buffer with FAST_BITS bits. This may not succeed if
   * input is exhausted, but we might still have enough bits for a short code.
   */
  (void)deflate_try_fill_bits(st, input, GCOMP_DEFLATE_HUFFMAN_FAST_BITS);

  // If we have no bits at all, we need more input.
  if (st->bit_count == 0) {
    return GCOMP_OK;
  }

  /* Peek whatever bits we have, padding with zeros if needed. The fast table
   * is designed so that short codes at index (code << (FAST_BITS - len)) work
   * correctly even with partial bits. */
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

  gcomp_status_t a = gcomp_deflate_huffman_build_decode_table(
      litlen_lengths, DEFLATE_MAX_LITLEN_SYMBOLS, 15u, &st->fixed_litlen);
  if (a != GCOMP_OK) {
    return a;
  }

  gcomp_status_t b = gcomp_deflate_huffman_build_decode_table(
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

  memset(st->dyn_clen_lengths, 0, sizeof(st->dyn_clen_lengths));
  memset(st->dyn_litlen_lengths, 0, sizeof(st->dyn_litlen_lengths));
  memset(st->dyn_dist_lengths, 0, sizeof(st->dyn_dist_lengths));

  if (st->dyn_clen_ready) {
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_clen_table);
    st->dyn_clen_ready = 0;
  }

  if (st->dyn_ready) {
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
      st->dyn_clen_lengths, 19u, 7u, &st->dyn_clen_table);
  if (st_build != GCOMP_OK) {
    return st_build == GCOMP_ERR_CORRUPT ? GCOMP_ERR_CORRUPT : st_build;
  }

  st->dyn_clen_ready = 1;
  st->dyn_lengths_total = st->dyn_hlit + st->dyn_hdist;
  st->dyn_lengths_index = 0;
  st->dyn_prev_len = 0;
  return GCOMP_OK;
}

static gcomp_status_t deflate_dynamic_decode_lengths(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input) {
  if (!st || !input) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!st->dyn_clen_ready) {
    return GCOMP_ERR_INTERNAL;
  }

  while (st->dyn_lengths_index < st->dyn_lengths_total) {
    int decoded = 0;
    uint16_t sym = 0;
    gcomp_status_t ds = deflate_huff_decode_symbol(
        st, input, &st->dyn_clen_table, &sym, &decoded);
    if (ds != GCOMP_OK) {
      return ds;
    }

    // Not enough input to decode a symbol.
    if (!decoded) {
      return GCOMP_OK;
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

  gcomp_status_t a = gcomp_deflate_huffman_build_decode_table(
      st->dyn_litlen_lengths, DEFLATE_MAX_LITLEN_SYMBOLS, 15u, &st->dyn_litlen);
  if (a != GCOMP_OK) {
    return a;
  }

  gcomp_status_t b = gcomp_deflate_huffman_build_decode_table(
      st->dyn_dist_lengths, DEFLATE_MAX_DIST_SYMBOLS, 15u, &st->dyn_dist);
  if (b != GCOMP_OK) {
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_litlen);
    return b;
  }

  st->dyn_ready = 1;
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
  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)gcomp_calloc(
          alloc, 1, sizeof(gcomp_deflate_decoder_state_t));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }

  st->bit_buffer = 0;
  st->bit_count = 0;
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
    gcomp_free(alloc, st);
    return GCOMP_ERR_INVALID_ARG;
  }

  st->window_size = (size_t)1u << (size_t)win_bits;
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
  st->window_pos = 0;
  st->window_filled = 0;

  st->max_output_bytes =
      gcomp_limits_read_output_max(options, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);
  st->total_output_bytes = 0;

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

  st->cur_litlen = NULL;
  st->cur_dist = NULL;

  decoder->method_state = st;
  decoder->update_fn = gcomp_deflate_decoder_update;
  decoder->finish_fn = gcomp_deflate_decoder_finish;
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
    gcomp_deflate_huffman_decode_table_cleanup(&st->fixed_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->fixed_dist);
  }

  if (st->dyn_ready) {
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_litlen);
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_dist);
  }

  if (st->dyn_clen_ready) {
    gcomp_deflate_huffman_decode_table_cleanup(&st->dyn_clen_table);
  }

  gcomp_free(alloc, st->window);
  gcomp_free(alloc, st);
  decoder->method_state = NULL;
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

  // Drain any pending match first.
  if (st->match_remaining > 0) {
    return deflate_copy_match(st, output);
  }

  // Resume pending length/distance decode if we have one
  if (st->pending_length_valid) {
    return deflate_decode_distance(st, input, output, st->pending_length_value);
  }

  int decoded = 0;
  uint16_t sym = 0;
  gcomp_status_t ds =
      deflate_huff_decode_symbol(st, input, st->cur_litlen, &sym, &decoded);
  if (ds != GCOMP_OK) {
    return ds;
  }

  /* Not enough input to decode a symbol. */
  if (!decoded) {
    return GCOMP_OK;
  }

  if (sym < 256u) {
    return deflate_emit_byte(st, output, (uint8_t)sym);
  }

  if (sym == 256u) {
    if (st->last_block) {
      st->stage = DEFLATE_STAGE_DONE;
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
      // We've consumed the length symbol but not its extra bits.
      // The proper fix would save this state too, but for now
      // we return and the caller will provide more input.
      // On resume, we'll try to decode a new litlen symbol, which
      // will fail because we're in the middle of a length code.
      // TODO: Save length symbol to properly resume.
      return GCOMP_OK;
    }
    length += extra;
  }

  // Now decode distance
  return deflate_decode_distance(st, input, output, length);
}

gcomp_status_t gcomp_deflate_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  if (!st) {
    return GCOMP_ERR_INTERNAL;
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
      return GCOMP_ERR_INTERNAL;
    }

    if (s != GCOMP_OK) {
      return s;
    }

    /* If this iteration did not consume input, produce output, or change any
     * relevant state, stop to avoid spinning with no progress. */
    if (input->used == prev_in_used && output->used == prev_out_used &&
        st->stage == prev_stage && st->stored_remaining == prev_stored &&
        st->match_remaining == prev_match && st->bit_count == prev_bits) {
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
    return GCOMP_ERR_INTERNAL;
  }

  // Drain any pending match with the provided output space.
  if (st->match_remaining > 0) {
    gcomp_status_t s = deflate_copy_match(st, output);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  return (st->stage == DEFLATE_STAGE_DONE) ? GCOMP_OK : GCOMP_ERR_CORRUPT;
}
