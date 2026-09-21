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

#include <ghoti.io/compress/macros.h>
#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/endian.h"
#include <ghoti.io/cutil/safemath.h>
#include "../../core/stream_internal.h"
#include "deflate_internal.h"
#include "huffman.h"
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/deflate.h>
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

/**
 * Width of the decoder's bit buffer, in bytes.
 *
 * Everything sized by the buffer derives from this, so that widening it
 * cannot leave a companion array behind.
 */
#define DEFLATE_BIT_BUFFER_BYTES 8u

typedef struct gcomp_deflate_decoder_state_s {
  //
  // Allocator (for internal memory operations)
  //
  const gcomp_allocator_t * allocator;

  //
  // Bitstream state (LSB-first)
  //
  /**
   * Bits read but not yet consumed, least-significant first.
   *
   * Sixty-four rather than thirty-two so that a refill can take eight bytes
   * with one unaligned load instead of eight passes of a loop, which is what
   * `deflate_try_fill_bits` does whenever the input has that much left.  It
   * also holds the longest thing the decoder asks for -- a 15-bit code plus
   * its extra bits -- several times over, so the refill happens once per
   * several symbols rather than once per symbol.
   *
   * Every bit at or above @ref bit_count is zero.  The Huffman fast path
   * relies on that: it indexes its table with the low bits of this buffer
   * and lets a short buffer pad the index with zeros.
   */
  uint64_t bit_buffer;
  uint32_t bit_count;

  //
  // Unconsumed bytes tracking
  //
  // When the deflate stream ends, any full bytes remaining in the bit buffer
  // are saved here. Container formats like gzip can retrieve these bytes
  // to use them for their own trailer parsing.
  //
  /**
   * Whole bytes that were pulled into @ref bit_buffer but not used.
   *
   * Sized from the bit buffer rather than written as a number, because it is
   * the bit buffer's width that decides how many there can be.  It used to be
   * four, "max 3 + safety", which was right for a 32-bit buffer and silently
   * wrong the moment that widened: the count below was clamped to the array,
   * so the bytes past it were dropped rather than reported, and a gzip
   * trailer read that way went missing.
   */
  uint8_t unconsumed_bytes[DEFLATE_BIT_BUFFER_BYTES];
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
  // WHERE THE HISTORY LIVES
  // =======================
  //
  // RFC 1951 section 3.2.3 lets a match reach up to 32768 bytes back into
  // what has already been decoded, so the decoder must keep that much
  // history.  It used to keep it *only* here, in a circular buffer of its
  // own, which meant every decoded byte was written twice -- once into the
  // caller's output buffer and once into this window -- and every match byte
  // was copied twice for the same reason.  The second write was 28.5% of what
  // a gzip decode cost after the earlier passes had trimmed everything else.
  //
  // The bytes are already in the caller's output buffer, so the history is
  // now split in two and this buffer holds only the part that is not:
  //
  //   history = window[...] (older)  ++  output[out_base .. output->used)
  //
  // A match whose distance falls inside the second part -- which is almost
  // every match when the caller offers a whole file's worth of room -- is one
  // memcpy inside the output buffer and never touches this window at all.  A
  // match that reaches further back takes its first bytes from here and then
  // runs on into the output buffer.
  //
  // The caller's buffer does not survive the call, so at the end of every
  // update() the tail of what was written is copied in here (see
  // deflate_window_sync()).  That is at most `window_size` bytes once per
  // call, against the one-byte-at-a-time writes it replaces.
  //
  uint8_t * window;
  size_t window_size;
  /**
   * @brief window_size - 1, for wrapping indices into the circular window.
   *
   * window_size is `1 << win_bits` with win_bits bounded to 8..15 by RFC 1951
   * section 3.2.1, so it is always a power of two and wrapping is a mask.
   * The compiler cannot see that - window_size is a runtime value - so
   * `% window_size` compiled to a 64-bit `div`, one for every literal emitted
   * and two for every byte of every match.  deflate_copy_match() was 14% of a
   * gzip decode because of it.
   */
  size_t window_mask;
  size_t window_pos;
  /**
   * @brief How many bytes of history this buffer holds.
   *
   * No longer the whole history: the bytes written into the caller's buffer
   * during the current call are history too and are not here yet.  The total
   * a distance may reach back into is deflate_history_bytes().
   */
  size_t window_filled;
  /**
   * @brief Where in the caller's output buffer this call began writing.
   *
   * Everything from here to `output->used` was decoded by this call and so is
   * history the next match may reach into.  Everything before it belongs to
   * whoever owns the buffer -- a container format may have written its own
   * bytes there -- and is never read.
   *
   * Set at the top of every public entry point, because a caller may hand
   * over a different buffer, or the same one rewound, on every call.
   */
  size_t out_base;

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
    // Eight bytes in one load where they are there.  DEFLATE packs bits
    // least-significant first (RFC 1951 section 3.1.1), so a little-endian
    // 64-bit read puts the stream's next byte in the lowest eight bits,
    // which is exactly where the buffer wants it.
    if (src && st->bit_count <= 56u && input->size - input->used >= 8u) {
      uint64_t chunk = gcomp_read_le64(src + input->used);
      // Only whole bytes that fit above what is already held are taken; the
      // rest stay in the input.  `room` is 1 to 8, and the mask is what keeps
      // the promise that bits at or above `bit_count` are zero -- without it
      // a partial eighth byte would leave stray bits up there and the
      // Huffman fast path would index its table with them.
      uint32_t room = (64u - st->bit_count) >> 3;
      if (room < 8u) {
        chunk &= ((uint64_t)1u << (room * 8u)) - 1u;
      }
      st->bit_buffer |= chunk << st->bit_count;
      st->bit_count += room * 8u;
      input->used += room;
      st->total_input_bytes += room;
      continue;
    }
    if (input->used >= input->size) {
      return 0;
    }
    uint8_t byte = src ? src[input->used] : 0u;
    st->bit_buffer |= ((uint64_t)byte) << st->bit_count;
    st->bit_count += 8u;
    input->used += 1u;
    st->total_input_bytes += 1u;
  }

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

  // nbits is 32 or fewer and the buffer is 64 wide, so neither the mask nor
  // the shift is at risk of the undefined behaviour a 32-bit buffer had to
  // step around here.
  *out = (uint32_t)(st->bit_buffer & (((uint64_t)1u << nbits) - 1u));
  st->bit_buffer >>= nbits;
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

/**
 * @brief How far back a match may legitimately reach, right now.
 *
 * The window holds the older half of the history and the caller's output
 * buffer holds the half this call has just written; see the window comment in
 * the state struct.  A distance is valid when it reaches no further than the
 * two together, and no further than the window the decoder was built with.
 *
 * That last clamp is not decoration.  RFC 1951 section 3.2.3 bounds a
 * distance by the window size, and `deflate.window_bits` is how a caller says
 * which window it is willing to pay for -- a zlib or gzip header states it,
 * and honouring it is what keeps a stream from needing more history than was
 * allocated for it.  Before the history was split this came for free, because
 * window_filled could not exceed the window; now the output buffer's share is
 * unbounded and a stream asking to reach 4 KB back into a 256-byte window
 * would be quietly obliged.  zlib refuses that too, and so does this.
 */
static size_t deflate_history_bytes(
    const gcomp_deflate_decoder_state_t * st, const gcomp_buffer_t * output) {
  size_t written = (output && output->used >= st->out_base)
      ? output->used - st->out_base
      : 0u;
  size_t have = st->window_filled + written;
  return (have > st->window_size) ? st->window_size : have;
}

/**
 * @brief Move the tail of what this call wrote into the window.
 *
 * The caller's output buffer is only ours for the duration of one call, so
 * before the call returns, whatever of it a later match might still need has
 * to be kept.  That is the last `window_size` bytes of it, and no more: a
 * distance cannot exceed the window (RFC 1951 section 3.2.3, and the decoder
 * refuses a longer one), so nothing older can ever be asked for again.
 *
 * Two memcpys at most -- one when the run fits between window_pos and the end
 * of the circular buffer, two when it wraps -- in place of the per-byte write
 * this replaces.  When the call wrote at least a whole window, the circular
 * arrangement is pointless for that copy, so the window is simply refilled
 * from the tail and the cursor put back to zero.
 *
 * Idempotent: it advances @c out_base to @c output->used, so calling it twice
 * copies nothing the second time.  That matters because finish() syncs before
 * handing the same buffer to update(), which syncs again on its way out.
 */
static void deflate_window_sync(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * output) {
  if (!st || !st->window || st->window_size == 0u || !output ||
      !output->data || output->used <= st->out_base) {
    if (st && output && output->used > st->out_base) {
      st->out_base = output->used;
    }
    return;
  }

  const uint8_t * src = (const uint8_t *)output->data + st->out_base;
  size_t written = output->used - st->out_base;
  const size_t window_size = st->window_size;

  if (written >= window_size) {
    memcpy(st->window, src + (written - window_size), window_size);
    st->window_pos = 0u;
    st->window_filled = window_size;
  }
  else {
    size_t first = window_size - st->window_pos;
    if (first > written) {
      first = written;
    }
    memcpy(st->window + st->window_pos, src, first);
    if (written > first) {
      memcpy(st->window, src + first, written - first);
    }
    st->window_pos = (st->window_pos + written) & st->window_mask;
    st->window_filled += written;
    if (st->window_filled > window_size) {
      st->window_filled = window_size;
    }
  }

  st->out_base = output->used;
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
  return GCOMP_OK;
}

/**
 * @brief Hand back whole bytes the bit buffer read past the end of the stream.
 *
 * The refill takes bytes in bulk, so when a deflate stream ends the buffer
 * usually holds some of whatever follows it -- for gzip and zlib, that is the
 * trailer, and losing it turns a good stream into a failed one.
 *
 * Whole bytes are what is handed back; the remaining `bit_count % 8` bits are
 * the encoder's padding to a byte boundary and are discarded.
 *
 * Two ways back, because there are two ways the bytes were taken:
 *
 * - If they came from the input buffer this call was given, rewind it.  The
 *   container reads them from there as if the decoder had never touched them.
 * - If they were taken during an earlier call, that buffer is gone, so they
 *   are kept for ::gcomp_deflate_decoder_get_unconsumed_data().
 *
 * This must run at every end of stream, not just the end of a Huffman block.
 * A stored block was the case that got missed: while the bit buffer was 32
 * bits wide it was always empty by then -- the block is byte-aligned and
 * LEN/NLEN took all 32 bits -- so a stored block ending a stream left nothing
 * to give back and nothing called this.  At 64 bits it leaves three bytes,
 * and every gzip stream whose last block was stored lost its trailer.
 */
static gcomp_status_t deflate_release_buffered_bytes(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input) {
  st->unconsumed_count = 0;
  if (st->bit_count >= 8u) {
    uint32_t bytes_to_handle = st->bit_count / 8u;
    // The buffer cannot hold more whole bytes than it is wide, so this cannot
    // fire.  It is an error rather than a clamp because clamping is what hid
    // the problem before: dropping a byte here loses a container's trailer
    // and reports success.
    if (bytes_to_handle > sizeof(st->unconsumed_bytes)) {
      return GCOMP_ERR_INTERNAL;
    }
    if (bytes_to_handle <= input->used) {
      input->used -= bytes_to_handle;
      st->total_input_bytes -= bytes_to_handle;
    }
    else {
      // The padding comes first and has to be shifted off.
      //
      // Bits are consumed from the bottom of the buffer, so what is left
      // after the end-of-block symbol is: the rest of the stream's final byte
      // -- the encoder's zero padding, `pad` bits of it -- and only then the
      // whole bytes of whatever follows.  Reading from bit zero reads the
      // padding as part of the first byte and every byte comes out shifted.
      //
      // The count above is right either way, since `pad` is under eight.  It
      // was only the extraction that was wrong, and only on this branch: the
      // other one hands whole bytes back to the input buffer, where the byte
      // boundary is the input's own and no shift arises.  Which branch runs
      // depends on whether the bytes came from the buffer this call was
      // given, so a gzip or zlib trailer came out as garbage exactly when the
      // stream happened to end on a call with nothing left to rewind -- and
      // then only when the final block was a Huffman one, because a stored
      // block is byte-aligned and leaves `pad` at zero.  The Adler-32 or CRC
      // read that way failed to match data that was in fact perfect.
      uint32_t pad = st->bit_count % 8u;
      for (uint32_t i = 0; i < bytes_to_handle; i++) {
        st->unconsumed_bytes[i] = (uint8_t)(st->bit_buffer >> (pad + i * 8u));
      }
      st->unconsumed_count = (uint8_t)bytes_to_handle;
    }
  }
  // The padding bits that remain are not part of anything.
  st->bit_buffer = 0;
  st->bit_count = 0;
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

  // Bytes the bit buffer read ahead come first.
  //
  // The refill takes whole bytes from the input in bulk, so when a stored
  // block begins the next bytes of the stream may be sitting in the bit
  // buffer rather than in `input`, and the bulk copy below -- which reads
  // straight from `input` -- would step over them.
  //
  // This could not arise while the buffer was 32 bits wide: the block is
  // aligned to a byte first, and reading LEN and NLEN took all 32 bits, so
  // the buffer was always empty by the time the copy ran.  A 64-bit buffer
  // has up to four whole bytes left at that point.
  //
  // `total_input_bytes` is not touched here: the refill counted these bytes
  // when it pulled them out of the input.
  //
  // Nothing writes to the window here.  A stored block's bytes are history
  // like any other, but they are in the caller's output buffer now and
  // deflate_window_sync() takes the tail of it when the call ends.
  {
    uint8_t * out_bytes = (uint8_t *)output->data;
    while (st->stored_remaining > 0u && st->bit_count >= 8u
        && output->used < output->size && out_bytes) {
      gcomp_status_t lim = deflate_check_output_limit(st, 1u);
      if (lim != GCOMP_OK) {
        return lim;
      }
      uint8_t b = (uint8_t)(st->bit_buffer & 0xFFu);
      st->bit_buffer >>= 8u;
      st->bit_count -= 8u;
      out_bytes[output->used++] = b;
      st->total_output_bytes += 1u;
      st->stored_remaining -= 1u;
    }
    if (st->stored_remaining == 0u) {
      return GCOMP_OK;
    }
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

  input->used += to_copy;
  output->used += to_copy;
  st->total_output_bytes += (uint64_t)to_copy;
  st->stored_remaining -= (uint32_t)to_copy;
  return GCOMP_OK;
}

/**
 * @brief Copy the pending match into the output.
 *
 * WHY THIS IS NOT A BYTE LOOP
 * ===========================
 *
 * It was, and it was 51.8% of a gzip decode.  Every byte of every match paid
 * for a distance validation, two null checks, a masked address computation, a
 * call to deflate_emit_byte(), an output-space test, an output limit check, an
 * expansion-ratio check carrying a 64-bit division, and a second masked store
 * into the window.  Roughly twenty operations to move one byte that a memcpy
 * moves in a fraction of one.
 *
 * None of that work is per-byte work.  The distance describes the match, the
 * null checks describe the decoder, and the limits describe a count.  So they
 * are done once per run and the bytes are moved with memcpy.
 *
 * WHY IT NO LONGER COPIES EVERYTHING TWICE
 * ========================================
 *
 * It used to read the match out of the decoder's own circular window and
 * write it to two places: the caller's output buffer, and back into the
 * window so that a later match could find it.  Two copies of every match
 * byte, and the second one -- the one nobody asked for -- was most of what
 * deflate_copy_match() still cost after the byte loop went.
 *
 * The bytes are already in the output buffer, so the copy back is only
 * needed for the part of the history that has left it, and that is done once
 * per call by deflate_window_sync() rather than once per match.  See the
 * window comment in the state struct for how the history is split.  Here that
 * split shows up as two sources:
 *
 * - `back == 0`: the whole run is inside what this call has already written.
 *   One memcpy inside the output buffer, no window at all.  This is the
 *   ordinary case -- a caller with room for the whole file never leaves it.
 *
 * - `back > 0`: the match starts `back` bytes before this call's output
 *   began, so those bytes come out of the window.  Each pass copies a little
 *   of that and shrinks `back` by exactly what it copied, so the run walks
 *   into the output buffer and the case above takes over.
 *
 * HOW LONG A RUN CAN BE
 * =====================
 *
 * A flat copy is only valid for as long as none of these happen partway
 * through: the match ends, the output fills, the source crosses out of the
 * window into the output buffer, or the circular window wraps.  Overlap is
 * not one of them any more.  The old code read and wrote one buffer, so it
 * had to stop every `distance` bytes; here the source and destination are
 * disjoint ranges of the output buffer, and the overlapping case -- a match
 * longer than its own distance, which is how DEFLATE spells a repeating
 * pattern -- is handled by growing the pattern rather than by stopping:
 *
 * - A distance of one is a run of one byte, which is what a row of identical
 *   pixels looks like after PNG filtering.  memset.
 *
 * - Otherwise the first `distance` bytes are copied, and then the copy
 *   doubles what it has: `distance`, 2*distance, 4*distance, and so on.  A
 *   three-byte pattern filling a 258-byte match is seven memcpys instead of
 *   the eighty-six the old three-at-a-time loop did.  Every one of them is
 *   non-overlapping by construction -- it never copies more than it has
 *   already written -- so it is a memcpy and not a memmove, and it is defined
 *   behaviour rather than a libc that happens to copy forward.
 */
/**
 * @brief Copy eight bytes, whatever their alignment.
 *
 * memcpy of a constant eight is the portable spelling of one unaligned load
 * and one unaligned store; every compiler this library is built with turns it
 * into exactly that, with no call.  Written out rather than left to a
 * `memcpy(dst, src, run)` with a runtime length, because that one really is a
 * call -- and at a mean match length of about nine bytes the call costs more
 * than the copying does.
 */
static inline void deflate_copy8(uint8_t * dst, const uint8_t * src) {
  uint64_t word;
  memcpy(&word, src, sizeof(word));
  memcpy(dst, &word, sizeof(word));
}

/**
 * @brief Copy @p run bytes from @p src to @p dst eight at a time.
 *
 * Writes up to seven bytes past @p run, so every caller must have checked
 * that there is that much room to spare.  Overrunning deliberately is what
 * makes it branchless: a match of nine bytes is two stores rather than a loop
 * that has to ask, twice, how much is left.
 *
 * Requires the two ranges to be at least eight bytes apart when they overlap,
 * which for a match means a distance of eight or more.
 */
static inline void deflate_copy_run8(
    uint8_t * dst, const uint8_t * src, size_t run) {
  size_t i = 0;
  do {
    deflate_copy8(dst + i, src + i);
    i += 8u;
  } while (i < run);
}

static gcomp_status_t deflate_copy_match(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * output) {
  if (!st || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->match_remaining == 0u) {
    return GCOMP_OK;
  }

  // The window is allocated when the decoder is created and update() refuses
  // an output buffer that is NULL with a non-zero size, so neither of these
  // can happen.  They are an error rather than a silent substitution of zero
  // bytes, which is what the old loop did: producing zeros for a match would
  // be corruption reported as success.
  //
  // The mark being behind the write cursor cannot happen either -- every
  // entry point sets it from output->used and output->used only grows
  // afterwards -- and it is checked for the same reason the others are.  What
  // is below it is `output->used - st->out_base` as an unsigned subtraction,
  // and where that goes wrong it does not go wrong quietly: the count wraps
  // to something near SIZE_MAX, every match then looks as though it is
  // entirely inside the output buffer, and the copy reads from before the
  // start of the caller's buffer.  One comparison to turn that into an error.
  if (!st->window || st->window_size == 0u || !output->data ||
      output->used < st->out_base) {
    return GCOMP_ERR_INTERNAL;
  }

  // Checked once, because the distance belongs to the match and not to the
  // byte.  A distance that reaches past what has been decoded, or past what
  // the window was sized to keep, is a corrupt stream and not a short read;
  // deflate_history_bytes() is the one place that decides which distances are
  // reachable, and both are its business.
  //
  // deflate_decode_distance() asks the same question, so that a bad distance
  // is refused before any of the match is emitted.  This one stays because
  // this function is also entered from finish() and from a match resumed
  // across calls, and because what is immediately below it is a memcpy from a
  // computed address.  A redundant comparison is the cheaper of the two
  // things that can be wrong here.
  if (st->match_distance == 0u ||
      (size_t)st->match_distance > deflate_history_bytes(st, output)) {
    return GCOMP_ERR_CORRUPT;
  }

  const uint8_t * const win = st->window;
  uint8_t * const out = (uint8_t *)output->data;
  const size_t window_size = st->window_size;
  const size_t distance = (size_t)st->match_distance;
  size_t written = output->used - st->out_base;

  while (st->match_remaining > 0u && output->used < output->size) {
    size_t run = (size_t)st->match_remaining;
    const size_t out_room = output->size - output->used;
    if (run > out_room) {
      run = out_room;
    }

    // How far the match reaches back beyond what this call has written, and
    // so how much of it has to come from the window.
    const size_t back = (distance > written) ? distance - written : 0u;

    if (back > 0u) {
      // The last `back` bytes before window_pos are the ones wanted; `back`
      // is at most window_filled, because the distance was checked against
      // the history above.
      const size_t src_pos = (st->window_pos + window_size - back) &
          st->window_mask;
      size_t avail = back;
      if (avail > window_size - src_pos) {
        avail = window_size - src_pos; // Stop at the circular buffer's seam.
      }
      if (run > avail) {
        run = avail;
      }
      if (run == 0u) {
        return GCOMP_ERR_INTERNAL; // Cannot happen; refuse to spin if it does.
      }
      gcomp_status_t lim = deflate_check_output_limit(st, run);
      if (lim != GCOMP_OK) {
        return lim;
      }
      // Both ends need seven bytes of slack for the wide copy: the output so
      // that the overrun stays inside the caller's buffer, and the window so
      // that the last load does not read past the allocation.  Neither is
      // usually short, and the ordinary memcpy is there for when one is.
      if (out_room - run >= 8u && window_size - src_pos - run >= 8u) {
        deflate_copy_run8(out + output->used, win + src_pos, run);
      }
      else {
        memcpy(out + output->used, win + src_pos, run);
      }
    }
    else {
      uint8_t * const dst = out + output->used;
      const uint8_t * const src = dst - distance;
      gcomp_status_t lim = deflate_check_output_limit(st, run);
      if (lim != GCOMP_OK) {
        return lim;
      }
      if (distance >= 8u && out_room - run >= 8u) {
        // The ordinary case, and the reason the history moved into the output
        // buffer at all.  Source and destination are eight or more bytes
        // apart in one flat buffer, so the run is a handful of 64-bit
        // load/store pairs with no call and nothing to decide per byte.
        deflate_copy_run8(dst, src, run);
      }
      else if (distance == 1u) {
        memset(dst, src[0], run);
      }
      else if (run <= distance) {
        memcpy(dst, src, run);
      }
      else {
        // A pattern shorter than the match.  Lay down one period and then
        // double what is there, so each copy is non-overlapping by
        // construction: it never copies more than it has already written.
        memcpy(dst, src, distance);
        size_t copied = distance;
        while (copied < run) {
          size_t n = run - copied;
          if (n > copied) {
            n = copied;
          }
          memcpy(dst + copied, dst, n);
          copied += n;
        }
      }
    }

    output->used += run;
    written += run;
    st->total_output_bytes += (uint64_t)run;
    st->match_remaining -= (uint32_t)run;
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

  // The table is indexed by the bits as they arrive, so the index is the low
  // FAST_BITS of the buffer and nothing has to be reversed or realigned here.
  //
  // Fewer bits than FAST_BITS needs no special case either.  Bits above
  // `bit_count` are zero -- the fill only ever ORs bytes in at `bit_count`,
  // and consuming shifts down -- so a short buffer indexes one of the slots
  // this code was replicated into, which holds the same entry.  The length
  // check below is what decides whether the code was really all there.
  uint32_t idx =
      (uint32_t)(st->bit_buffer & (GCOMP_DEFLATE_HUFFMAN_FAST_SIZE - 1u));

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

  // `extra` is the width of the sub-table's index, which is sized for the
  // longest code that lands in it - not for the code actually present.  So a
  // shorter code in that sub-table must still be decodable when fewer than
  // FAST_BITS + extra bits remain, and at the end of a stream that is the
  // normal case: the last symbol is followed only by the encoder's padding to
  // the next byte boundary, at most seven bits.
  //
  // Insisting on the full width here made the decoder stall on valid streams.
  // A 14-bit end-of-block code with a 15-bit sub-table and exactly 14 bits
  // left decoded every byte of the data and then reported the stream as
  // truncated - a stream zlib accepted and this library did not.
  //
  // Peek whatever there is and pad the tail with zeros, exactly as the fast
  // path above does; the code found that way is right whenever its own length
  // fits in what is available, and the length check below confirms that.
  uint32_t full_bits = GCOMP_DEFLATE_HUFFMAN_FAST_BITS + extra;
  (void)deflate_try_fill_bits(st, input, full_bits);
  if (st->bit_count < GCOMP_DEFLATE_HUFFMAN_FAST_BITS) {
    return GCOMP_OK; // Fewer bits than the prefix that got us here; wait.
  }
  // The sub-table index is the `extra` bits sitting directly above the
  // prefix, again as they arrive.  Bits past `bit_count` are zero, which
  // selects one of the slots a shorter code in this sub-table was replicated
  // into -- the reason the replication above fills the high bits rather than
  // the low ones.
  uint32_t low = (uint32_t)((st->bit_buffer >> GCOMP_DEFLATE_HUFFMAN_FAST_BITS)
      & (((uint64_t)1u << extra) - 1u));

  // Use safe math for index calculation to prevent overflow
  size_t long_idx;
  if (!gcu_safe_add_size((size_t)table->long_base[idx], (size_t)low, &long_idx)) {
    return GCOMP_ERR_CORRUPT;
  }
  if (long_idx >= table->long_table_count) {
    return GCOMP_ERR_CORRUPT;
  }

  gcomp_deflate_huffman_fast_entry_t le = table->long_table[long_idx];
  if (le.nbits == 0u) {
    return GCOMP_ERR_CORRUPT;
  }

  // Only now is the code's real length known; consume exactly that much, and
  // only if it is there.
  if (st->bit_count < le.nbits) {
    return GCOMP_OK; // Need more input
  }
  st->bit_buffer >>= le.nbits;
  st->bit_count -= le.nbits;

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

  // RFC 1951 3.2.7 gives the code length alphabet no incomplete-code
  // exception - 3.2.7's exception is about the distance alphabet - so this one
  // must be complete.
  gcomp_status_t clen_complete = gcomp_deflate_huffman_check_complete(
      st->dyn_clen_lengths, 19u, 7u, 0);
  if (clen_complete != GCOMP_OK) {
    return clen_complete;
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

  // RFC 1951 3.2.2 constructs a code from the lengths in a way that only
  // closes if the Kraft sum is 1, so an incomplete literal/length code
  // describes bit patterns it does not define. Building it anyway means
  // decoding whichever of those patterns the stream happens to contain and
  // reporting success; the holes are caught on use, but only if the stream
  // reaches one. zlib refuses such a stream outright ("invalid literal/lengths
  // set") and so does this now. 3.2.7's single-code exception is allowed for
  // both alphabets here: it is written for the distance code, and a
  // literal/length alphabet reduced to one one-bit code is the same shape.
  gcomp_status_t litlen_complete =
      gcomp_deflate_huffman_check_complete(st->dyn_litlen_lengths,
          DEFLATE_MAX_LITLEN_SYMBOLS, 15u, 1);
  if (litlen_complete != GCOMP_OK) {
    return litlen_complete;
  }
  gcomp_status_t dist_complete = gcomp_deflate_huffman_check_complete(
      st->dyn_dist_lengths, DEFLATE_MAX_DIST_SYMBOLS, 15u, 1);
  if (dist_complete != GCOMP_OK) {
    return dist_complete;
  }

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

  gcomp_status_t status = GCOMP_OK;
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
  st->window_mask = window_size - 1u;
  st->max_window_bytes =
      gcomp_limits_read_window_max(options, (uint64_t)st->window_size);
  if (st->max_window_bytes != 0 &&
      (uint64_t)st->window_size > st->max_window_bytes) {
    status = GCOMP_ERR_LIMIT;
    goto cleanup;
  }

  st->window = (uint8_t *)gcomp_malloc(alloc, st->window_size);
  if (!st->window) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  // Track window allocation
  gcomp_memory_track_alloc(&st->mem_tracker, st->window_size);

  st->window_pos = 0;
  st->window_filled = 0;
  st->out_base = 0;

  st->max_output_bytes =
      gcomp_limits_read_output_max(options, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);
  st->max_expansion_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_DEFLATE_MAX_EXPANSION_RATIO);
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

  status = deflate_build_fixed_tables(st);
  if (status != GCOMP_OK) {
    goto cleanup;
  }
  // Track fixed Huffman table allocations
  track_huffman_table_alloc(st, &st->fixed_litlen);
  track_huffman_table_alloc(st, &st->fixed_dist);

  st->cur_litlen = NULL;
  st->cur_dist = NULL;

  // Success path
  decoder->method_state = st;
  decoder->update_fn = gcomp_deflate_decoder_update;
  decoder->finish_fn = gcomp_deflate_decoder_finish;
  decoder->reset_fn = gcomp_deflate_decoder_reset;

  // A raw deflate stream has no way to say that it needs a dictionary, so the
  // caller has to.  The zlib decoder does not come through here: it learns
  // from FDICT and calls gcomp_deflate_decoder_set_dictionary() once it has
  // checked DICTID, which is the only way to know the dictionary is the right
  // one.  The state is attached first because that function reads it.
  if (options) {
    const void * dict = NULL;
    size_t dict_len = 0;
    if (gcomp_options_get_bytes(options, "deflate.dictionary", &dict,
            &dict_len) == GCOMP_OK &&
        dict && dict_len > 0) {
      status = gcomp_deflate_decoder_set_dictionary(decoder, dict, dict_len);
      if (status != GCOMP_OK) {
        decoder->method_state = NULL;
        decoder->update_fn = NULL;
        decoder->finish_fn = NULL;
        decoder->reset_fn = NULL;
        goto cleanup;
      }
    }
  }

  return GCOMP_OK;

cleanup:
  // Clean up all allocations on error (gcomp_free handles NULL safely)
  gcomp_free(alloc, st->window);
  gcomp_free(alloc, st);
  return status;
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

gcomp_status_t gcomp_deflate_decoder_set_dictionary(
    gcomp_decoder_t * decoder, const void * dict, size_t dict_len) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (!dict && dict_len > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  if (!st->window || st->window_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // The history a stream starts from cannot be changed once it has started.
  if (st->total_output_bytes != 0 || st->window_filled != 0) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_INVALID_ARG,
        "a preset dictionary must be set before decoding begins");
  }
  if (dict_len == 0) {
    return GCOMP_OK;
  }

  // Only the tail is reachable: RFC 1951 section 3.2.5 bounds a distance by
  // the window, so bytes further back than that could never be referenced.
  size_t take = dict_len;
  if (take > st->window_size) {
    take = st->window_size;
  }
  memcpy(st->window, (const uint8_t *)dict + (dict_len - take), take);

  // The window is circular: a dictionary that exactly fills it leaves the
  // write position back at zero.
  st->window_filled = take;
  st->window_pos = take & st->window_mask;
  return GCOMP_OK;
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
  st->out_base = 0;
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

  // Reaching further back than has been decoded is corruption.  The history
  // is not the window alone any more -- what this call has written into the
  // caller's buffer counts too -- so ask for the total rather than reading
  // window_filled, which is now only the older half of it.  deflate_copy_
  // match() checks the same thing, since it can also be entered from finish()
  // and from a resumed match; this one is here so that a bad distance is
  // refused before any of the match is emitted.
  if (distance == 0 || (size_t)distance > deflate_history_bytes(st, output)) {
    return GCOMP_ERR_CORRUPT;
  }

  // Clear pending state since we successfully decoded
  st->pending_length_valid = 0;
  st->pending_dist_valid = 0;

  st->match_distance = distance;
  st->match_remaining = length;
  return deflate_copy_match(st, output);
}

/**
 * @brief Decode one symbol's worth of a Huffman block.
 *
 * Returns after a single literal, a single length/distance pair, or the end
 * of the block; the loop that calls it is deflate_process_huffman_data().
 */
static gcomp_status_t deflate_huffman_step(
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
      gcomp_status_t rel = deflate_release_buffered_bytes(st, input);
      if (rel != GCOMP_OK) {
        return rel;
      }
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

/**
 * @brief Decode as much of a Huffman block as the buffers allow.
 *
 * WHY THIS LOOPS
 * ==============
 *
 * It used to decode exactly one symbol and return to gcomp_deflate_decoder_
 * update(), which meant every literal and every match paid for a full turn of
 * that function's loop: a snapshot of seven pieces of decoder state, a switch
 * on the stage, a call, and then seven comparisons to work out whether
 * anything had happened.  About thirty-four instructions of bookkeeping to
 * decode one symbol, and 23.6% of a decode.
 *
 * Staying here instead costs a comparison or two per symbol.  The outer loop
 * still exists and still does its check -- it has to, because a block can end
 * and the next one can be of a different type -- but it now runs once per
 * block rather than once per symbol.
 *
 * Stopping conditions: an error, the block ended (which changes the stage and
 * belongs to the caller), or a step that neither consumed input nor produced
 * output, which means it is waiting for one or the other.
 *
 * A full output buffer is deliberately NOT one of them, and must not be.  Not
 * every symbol needs output space: end-of-block needs none, and it is what
 * moves the stage to DONE.  Returning early because the output was full left
 * that symbol unread on a stream whose decoded size exactly filled the
 * caller's buffer -- so finish() found the stream unfinished and reported
 * GCOMP_ERR_LIMIT for ever.  Every PNG the image library wrote decoded into a
 * buffer of exactly the right size, and every one of them failed.
 *
 * A step that cannot make progress against a full buffer stashes its state and
 * changes nothing, which the progress check below catches on the next pass.
 */
static gcomp_status_t deflate_process_huffman_data(
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
  for (;;) {
    const size_t in_before = input->used;
    const size_t out_before = output->used;

    gcomp_status_t s = deflate_huffman_step(st, input, output);
    if (s != GCOMP_OK) {
      return s;
    }
    if (st->stage != DEFLATE_STAGE_HUFFMAN_DATA) {
      return GCOMP_OK; // The block ended; the caller decides what follows.
    }
    if (input->used == in_before && output->used == out_before) {
      return GCOMP_OK; // Waiting for more input, or for room to write.
    }
  }
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

/**
 * @brief The decoding itself, with the output buffer known to be ours.
 *
 * Split from gcomp_deflate_decoder_update() only so that the window can be
 * brought up to date on every way out of it, of which there are a dozen.
 * Doing that at each `return` is how a path gets missed, and a missed one is
 * silent: the stream keeps decoding and only a match that reaches back across
 * the gap goes wrong, which is input-dependent and may be far away.
 */
static gcomp_status_t deflate_decoder_decode(gcomp_decoder_t * decoder,
    gcomp_deflate_decoder_state_t * st, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
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
        if (st->last_block) {
          // Same as the end of a Huffman block: whatever the bit buffer read
          // past the end of the stream belongs to whoever wraps it.
          s = deflate_release_buffered_bytes(st, input);
          st->stage = DEFLATE_STAGE_DONE;
        }
        else {
          st->stage = DEFLATE_STAGE_BLOCK_HEADER;
        }
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

gcomp_status_t gcomp_deflate_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // Check data pointers if size > 0
  if ((input->size > 0 && !input->data) ||
      (output->size > 0 && !output->data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_decoder_state_t * st =
      (gcomp_deflate_decoder_state_t *)decoder->method_state;
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INTERNAL, "decoder state is NULL");
  }

  // This buffer is the window for the duration of this call.  Where it begins
  // is wherever the caller left it: a container format writes its own bytes
  // in front of ours, and nothing before this mark is ever read back.
  st->out_base = output->used;

  gcomp_status_t s = deflate_decoder_decode(decoder, st, input, output);

  // Keep whatever a later call may still need, on the error path as well as
  // the good one.  An error usually ends the stream, but "usually" is not a
  // reason to leave the window disagreeing with what was emitted.
  deflate_window_sync(st, output);
  return s;
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

  if (output->size > 0 && !output->data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Already complete: say so without writing anything, as the header promises.
  if (st->stage == DEFLATE_STAGE_DONE && st->match_remaining == 0u) {
    return GCOMP_OK;
  }

  // The same rule as update(): this buffer is the window for as long as the
  // call lasts, and what gets written into it has to reach the real window
  // before the call gives it back.
  //
  // Marked once, for the whole of finish, and that is deliberate.  Everything
  // the decoder still owes the caller comes out of state it already holds --
  // bits read out of the input but not yet turned into symbols, and a match
  // half copied when the last output buffer filled -- so finishing is the
  // draining of that match followed by one more turn of the loop update()
  // runs, with nothing new to read.  Without that second half, a caller who
  // called update() once per input chunk and then finish() -- which is the
  // obvious way to drive it, and the way the header's own example drives the
  // encoder -- was told the stream was corrupt whenever the last output
  // buffer had been too small to hold the tail.
  //
  // It used to reach that second half through the public update(), which
  // marks the buffer from wherever it finds it.  That put a seam in the
  // middle of one call: the bytes the match drained were behind the new mark
  // and so had to be in the window already, which meant a second sync,
  // between the two halves, whose absence nothing could be made to notice.
  // Calling the decoding directly keeps one mark and one sync over the whole
  // call, and there is no seam to get wrong.
  st->out_base = output->used;

  gcomp_status_t s = GCOMP_OK;
  if (st->match_remaining > 0u) {
    s = deflate_copy_match(st, output);
    if (s != GCOMP_OK) {
      deflate_window_sync(st, output);
      return gcomp_decoder_set_error(decoder, s,
          "error draining pending match (%u bytes remaining)",
          st->match_remaining);
    }
    if (st->match_remaining > 0u) {
      // The buffer filled part way through the match.  More to come.
      deflate_window_sync(st, output);
      return GCOMP_ERR_LIMIT;
    }
  }

  gcomp_buffer_t no_more_input = {NULL, 0, 0};
  s = deflate_decoder_decode(decoder, st, &no_more_input, output);
  deflate_window_sync(st, output);
  if (s != GCOMP_OK) {
    return s;
  }

  if (st->stage == DEFLATE_STAGE_DONE && st->match_remaining == 0u) {
    return GCOMP_OK;
  }

  // Not finished, and there are two very different reasons why.  This used to
  // report both of them as corruption.
  if (output->used >= output->size) {
    // The output buffer filled.  Drain it and call again.
    return GCOMP_ERR_LIMIT;
  }

  // Room to spare and it still could not finish: the input really did end
  // part way through the stream.
  return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
      "incomplete deflate stream (stage '%s', expected final block)",
      deflate_stage_name(st->stage));
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
