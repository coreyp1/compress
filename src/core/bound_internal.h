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
 * ## Why the window size comes into it
 *
 * A stored block is written back out of the sliding window, so the encoder can
 * only store a block whose bytes are all still in it.  That makes the window,
 * not RFC 1951's 65535-byte LEN field, the thing that limits how much input a
 * stored block can carry - and on data that will not compress, where the
 * encoder wants to store every block, it closes each one early to keep it
 * storable (`deflate_encode.c`, `out_of_window_reach`).
 *
 * So a small window means many small blocks and five bytes of overhead on each
 * one.  Measured on incompressible input, the overhead is about 0.02% at
 * `window_bits` 15, 2% at 9, and 21% at 8 - which is why this cannot be a
 * single ratio the way zlib's `deflateBound()` is.  (zlib does not have the
 * problem to solve: `deflateInit2()` quietly raises a `windowBits` of 8 to 9.)
 *
 * The divisor below says the encoder never cuts a storable block smaller than
 * a sixteenth of its window.  That is not the encoder's own arithmetic - it is
 * a floor underneath it, chosen with room to spare against what the smallest
 * windows actually produce, so that a change to the block-closing heuristic
 * cannot quietly invalidate the bound.  `tests/integration/test_bound.cpp`
 * sweeps every window size, level and input shape against it.
 */
static inline gcomp_status_t gcomp_bound_deflate_raw(
    size_t n, unsigned window_bits, size_t * bound_out) {
  const size_t kStoredOverhead = 5u;
  const size_t kMaxStored = 65535u;

  if (window_bits < 8u || window_bits > 15u) {
    window_bits = 15u;
  }
  size_t window_size = (size_t)1u << window_bits;

  // The floor under the encoder's block-closing rule; see above.
  size_t min_block = window_size / 16u;
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
