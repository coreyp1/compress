/**
 * @file zstd_repcodes.h
 *
 * The repeat-offset rules of RFC 8878 section 3.1.1.3.2.1.1, in one place.
 *
 * Both parses need them and both must agree with the encoder about them, so
 * they live here rather than once per parse.  The cost of getting that wrong
 * is not a smaller stream but a wrong one, and it does not show up in a round
 * trip: rotating the list wrongly for code 3 still decoded 400 KB of text
 * correctly, because the mistake only surfaces when a later sequence looks up
 * the offset the rotation misplaced.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_REPCODES_H
#define GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_REPCODES_H

#include <ghoti.io/compress/macros.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Repeat offsets
//
// These two must agree exactly with what zstd_opt_emit() below writes, or
// the parse would be pricing a stream other than the one it goes on to
// produce.  A test build checks that they do, at every sequence of every
// block; see the assertions in zstd_opt_emit().
//

/**
 * @brief How @p offset would be written, given the path's repeat offsets.
 *
 * A distance equal to one of the three most recently used is written as the
 * code 1, 2 or 3 rather than as a distance, which is a large saving.  Which
 * of the three a code names depends on whether the sequence has literals
 * before it: with none, the codes shift up by one and the third names the
 * most recent distance less one (RFC 8878 section 3.1.1.3.2.1.1).
 *
 * That shifted form is not a curiosity here.  A parse that chooses matches
 * by cost puts them end to end constantly: at level 19, 52% of the sequences
 * over 400 KB of manual pages have no literals before them, and 64% over C
 * source.  Declining the codes there, as the deferred parse still does,
 * meant writing a full distance for most of the block.  Taking them is worth
 * 0.44% of the output at level 22, and it is faster, because a cheaper
 * offset makes a longer match worth taking and there are fewer sequences.
 *
 * Anything else is the distance plus three, since the first three values are
 * spoken for.
 */
static inline uint32_t zstd_opt_encode_offset(
    const uint32_t * rep, uint32_t litlen, uint32_t offset) {
  if (litlen > 0u) {
    if (offset == rep[0]) {
      return 1u;
    }
    if (offset == rep[1]) {
      return 2u;
    }
    if (offset == rep[2]) {
      return 3u;
    }
  }
  else {
    if (offset == rep[1]) {
      return 1u;
    }
    if (offset == rep[2]) {
      return 2u;
    }
    // Code 3 with no literals means one less than the most recent distance,
    // and a distance of zero is not a distance.
    if (rep[0] > 1u && offset == rep[0] - 1u) {
      return 3u;
    }
  }
  return offset + 3u;
}

/**
 * @brief The three repeat offsets after a sequence written as @p encoded.
 *
 * Whichever of the three the sequence used comes to the front and everything
 * it passed drops one place; a distance that was not in the list at all
 * comes to the front and pushes the last one off.  Both cases are the same
 * rule, and it is written once here so that the parse's idea of the list and
 * the encoder's cannot drift apart.
 */
static inline void zstd_opt_rep_after(const uint32_t * in, uint32_t encoded,
    uint32_t litlen, uint32_t offset, uint32_t * out) {
  unsigned slot = 3u; // Not one of the three.
  if (encoded <= 3u) {
    slot = encoded - 1u + ((litlen == 0u) ? 1u : 0u);
  }

  switch (slot) {
    case 0u:
      out[0] = in[0];
      out[1] = in[1];
      out[2] = in[2];
      break;
    case 1u:
      out[0] = in[1];
      out[1] = in[0];
      out[2] = in[2];
      break;
    case 2u:
      out[0] = in[2];
      out[1] = in[0];
      out[2] = in[1];
      break;
    default:
      // A distance from outside the list -- either written in full, or the
      // "one less than the most recent" that code 3 means with no literals.
      out[0] = offset;
      out[1] = in[0];
      out[2] = in[1];
      break;
  }
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_REPCODES_H
