/**
 * @file bound_internal.h
 *
 * Worst-case encoded-size arithmetic, shared by the methods that need it.
 *
 * ## What a bound promises
 *
 * gcomp_encode_bound() promises that an output buffer of the size it reports
 * is enough for any input of the given length under the given options, so that
 * gcomp_encode_buffer() cannot return ::GCOMP_ERR_LIMIT.  That promise is only
 * as good as the encoder's willingness to fall back to a stored or raw block
 * when compression would expand the data, so the numbers below are one half of
 * the contract and `tests/integration/test_bound.cpp` is the other.
 *
 * The bound covers a whole stream: create, update as many times as you like,
 * finish.  It does **not** cover gcomp_encoder_flush(), which ends a block
 * early and pads to a byte boundary (see stream.h) and so can add to the
 * output as many times as it is called.
 *
 * ## Overflow
 *
 * Every step goes through cutil's checked arithmetic.  A bound that silently
 * wrapped would be worse than no bound at all: the caller would allocate a
 * small buffer and believe it was big enough.  Overflow is reported as
 * ::GCOMP_ERR_LIMIT - the size cannot be represented, so no buffer can be.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_BOUND_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_CORE_BOUND_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/cutil/safemath.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Number of blocks @p n bytes occupies at @p block_size each.
 *
 * At least one: every format writes something for an empty input, and a
 * block count of zero would hide that.
 */
static inline gcomp_status_t gcomp_bound_block_count(
    size_t n, size_t block_size, size_t * count_out) {
  if (block_size == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }
  size_t blocks = n / block_size;
  if (n % block_size != 0) {
    blocks++;
  }
  if (blocks == 0) {
    blocks = 1;
  }
  *count_out = blocks;
  return GCOMP_OK;
}

/**
 * @brief @p acc += @p addend, or ::GCOMP_ERR_LIMIT if that cannot be held.
 */
static inline gcomp_status_t gcomp_bound_add(size_t * acc, size_t addend) {
  if (!gcu_safe_add_size(*acc, addend, acc)) {
    return GCOMP_ERR_LIMIT;
  }
  return GCOMP_OK;
}

/**
 * @brief @p acc += @p a * @p b, or ::GCOMP_ERR_LIMIT if that cannot be held.
 */
static inline gcomp_status_t gcomp_bound_add_mul(
    size_t * acc, size_t a, size_t b) {
  size_t product;
  if (!gcu_safe_mul_size(a, b, &product)) {
    return GCOMP_ERR_LIMIT;
  }
  return gcomp_bound_add(acc, product);
}

/**
 * @brief Largest DEFLATE stream this encoder can produce for @p n bytes.
 *
 * The fallback every DEFLATE encoder has is the non-compressed block, RFC 1951
 * section 3.2.4: three bits of BFINAL and BTYPE, then "skip any remaining bits
 * in current partially processed byte", then LEN and NLEN as two bytes each,
 * then LEN literal bytes.  Worst case is five bytes of overhead per block -
 * one byte holding the three header bits and up to five bits of padding, and
 * four bytes of LEN/NLEN.
 *
 * ## How small a block can get
 *
 * Five bytes of overhead per block only matters if blocks can be short, and
 * what makes them short is the encoder closing one early.  Two things bound
 * that: the symbol buffer filling, and a stored block needing its bytes to
 * still be in the history it is written from.
 *
 * Both used to come from `window_size`, which made a small window mean many
 * small blocks - measured at 21% overhead at `window_bits` 8 - so this had to
 * be window-aware where zlib's `deflateBound()` is a single ratio.  Neither
 * does any more: the symbol buffer has a floor of its own
 * (`DEFLATE_SYM_BUF_MIN`) and a stored block is written from a ring kept for
 * the purpose (`DEFLATE_STORED_RING_SIZE`), both independent of the window.
 * Measured overhead is now 0.122% at every `window_bits` from 8 to 12 and
 * smaller above that.
 *
 * The divisor below says the encoder never cuts a storable block smaller than
 * a sixteenth of the smallest span it is allowed to work in.  That is not the
 * encoder's own arithmetic - it is a floor underneath it, with room to spare
 * against what is actually produced, so that a change to the block-closing
 * heuristic cannot quietly invalidate the bound.  The window is still a
 * parameter because it still caps the span from above at `window_bits` 13 and
 * up, where no ring is allocated and the window serves the block directly.
 * `tests/integration/test_bound.cpp` sweeps every window size, level and input
 * shape against it.
 */
static inline gcomp_status_t gcomp_bound_deflate_raw(
    size_t n, unsigned window_bits, size_t * bound_out) {
  const size_t kStoredOverhead = 5u;
  const size_t kMaxStored = 65535u;

  if (window_bits < 8u || window_bits > 15u) {
    window_bits = 15u;
  }
  size_t window_size = (size_t)1u << window_bits;

  // The floor under the encoder's block-closing rule; see above.  The span a
  // block may cover is at least DEFLATE_SYM_BUF_MIN whatever the window, so
  // the smallest window no longer means the smallest blocks.  Kept in step
  // with deflate_encode.c by tests/integration/test_bound.cpp, which sweeps
  // every window size rather than trusting this to stay true.
  const size_t kDeflateSymBufMin = 4096u;
  size_t span = window_size < kDeflateSymBufMin ? kDeflateSymBufMin : window_size;
  size_t min_block = span / 16u;
  if (min_block < 8u) {
    min_block = 8u;
  }
  if (min_block > kMaxStored) {
    min_block = kMaxStored;
  }

  size_t blocks;
  gcomp_status_t s = gcomp_bound_block_count(n, min_block, &blocks);
  if (s != GCOMP_OK) {
    return s;
  }
  // One more for the empty final block: an empty input still needs one, and an
  // encoder may close the stream with a block of its own rather than marking
  // the last block of data final.
  if (!gcu_safe_add_size(blocks, 1u, &blocks)) {
    return GCOMP_ERR_LIMIT;
  }

  size_t bound = n;
  s = gcomp_bound_add_mul(&bound, blocks, kStoredOverhead);
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

/**
 * @brief The window_bits these options select, or the default.
 *
 * Shared by deflate and by the two methods that wrap it, so that all three
 * bounds are computed against the same window the encoder will use.
 */
static inline unsigned gcomp_bound_deflate_window_bits(
    gcomp_options_t * options) {
  uint64_t wb = 15u;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "deflate.window_bits", &v) ==
            GCOMP_OK &&
        v >= 8u && v <= 15u) {
      wb = v;
    }
  }
  return (unsigned)wb;
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_BOUND_INTERNAL_H
