/**
 * @file stepdown.h
 *
 * A tally of the times an encoder settled for a weaker encoding.
 *
 * WHY THIS EXISTS
 * ---------------
 * Every encoder here can produce something less compact than it meant to: a
 * stored block instead of a compressed one, a fixed Huffman code instead of a
 * code built for the block, literals stored instead of entropy-coded.  Two
 * quite different things arrive at that same line of code.
 *
 * One is a decision.  The encoder priced both codings and the weaker one was
 * smaller, which is the encoder working correctly and there is nothing to
 * report.  The other is a failure: a code the builder refused, memory that
 * could not be allocated, an output buffer that would not fit.  The data
 * still comes out correct - that is the point of a fallback - but it comes
 * out bigger, and nothing says so.
 *
 * Because both take the same path, a defect that makes the encoder fail on
 * every block looks exactly like an encoder deciding it cannot do better.
 *
 * SILENCE COUNTS AS FAILURE
 * -------------------------
 * A caller that reaches the weaker encoding should name a *chosen* reason to
 * say why, and leave the forced default in place otherwise.  Noting each
 * branch instead does not work: the branch that gets it wrong is the one
 * nobody thought to note, which is how the first version of this counter
 * missed the very defect it was written for.  See zstd_block_compress().
 * That has happened three times in this library:
 *
 *   - A distance code that was over-subscribed by construction made the
 *     canonical-code builder refuse it, so every block of an XML registry was
 *     written with the fixed code: 135,514 bytes where its own frequencies
 *     called for 113,262.
 *   - A block whose sequences matched everything has no literals, and a test
 *     for literals sent six of eight blocks of a file to stored blocks:
 *     189,076 bytes became 833,633.
 *   - The mirror of that, a test for sequences, had sent every match-less
 *     block to a stored block before it.
 *
 * All three passed the whole test suite.  A test cannot see a silent loss of
 * quality unless something counts it, so this counts it: tests assert that
 * nothing was *forced*, and leave what was *chosen* alone.
 *
 * Internal only - not part of the public API.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_STEPDOWN_H
#define GHOTI_IO_GCOMP_SRC_CORE_STEPDOWN_H

#include <ghoti.io/compress/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Why an encoder emitted something weaker than its best encoding.
 *
 * The chosen reasons come first and the forced ones after, so that "was
 * anything forced" is a range check.  Add chosen reasons before
 * @ref GCOMP_STEPDOWN_FIRST_FORCED and forced ones after it.
 */
typedef enum {
  /// The stored form was smaller than the compressed one.
  GCOMP_STEPDOWN_STORED_IS_SMALLER = 0,
  /// The fixed code was smaller than a code built for this block.
  GCOMP_STEPDOWN_FIXED_IS_SMALLER,
  /// Too few literals, or too uniform, for entropy coding to pay.
  GCOMP_STEPDOWN_LITERALS_NOT_WORTH_CODING,
  /// Too little input for the compressed form to be worth trying.
  GCOMP_STEPDOWN_TOO_SMALL_TO_TRY,

  /// A code the encoder built was refused as invalid.
  GCOMP_STEPDOWN_CODE_REJECTED,
  /// Scratch memory could not be allocated.
  GCOMP_STEPDOWN_NO_MEMORY,
  /// The output buffer could not hold the better encoding.
  GCOMP_STEPDOWN_NO_ROOM,
  /// Something else went wrong and the weaker encoding was used instead.
  GCOMP_STEPDOWN_ENCODE_FAILED,

  GCOMP_STEPDOWN_COUNT
} gcomp_stepdown_t;

/// First reason that means "this went wrong", rather than "this was cheaper".
#define GCOMP_STEPDOWN_FIRST_FORCED GCOMP_STEPDOWN_CODE_REJECTED

/**
 * @brief Counts, one per reason.
 *
 * Zero-initialised with the encoder state it lives in; nothing else is
 * needed to set it up.
 */
typedef struct {
  uint64_t counts[GCOMP_STEPDOWN_COUNT];
} gcomp_stepdown_tally_t;

/**
 * @brief Record one step-down.
 *
 * @param tally Tally to add to; NULL is ignored, so a caller without one
 *        need not guard the call.
 * @param reason Why the encoder settled for less.
 */
static inline void gcomp_stepdown_note(
    gcomp_stepdown_tally_t * tally, gcomp_stepdown_t reason) {
  if (tally && (size_t)reason < (size_t)GCOMP_STEPDOWN_COUNT) {
    tally->counts[reason]++;
  }
}

/**
 * @brief How many step-downs were forced rather than chosen.
 *
 * A healthy encoder returns zero here for any input.  A non-zero count means
 * output is larger than the encoder itself could have made it.
 *
 * @param tally Tally to read; NULL reads as zero.
 * @return Total of the forced reasons.
 */
static inline uint64_t gcomp_stepdown_forced_total(
    const gcomp_stepdown_tally_t * tally) {
  if (!tally) {
    return 0;
  }
  uint64_t total = 0;
  for (size_t i = (size_t)GCOMP_STEPDOWN_FIRST_FORCED;
      i < (size_t)GCOMP_STEPDOWN_COUNT; i++) {
    total += tally->counts[i];
  }
  return total;
}

/**
 * @brief How many step-downs happened at all, chosen or forced.
 *
 * @param tally Tally to read; NULL reads as zero.
 * @return Total across every reason.
 */
static inline uint64_t gcomp_stepdown_total(
    const gcomp_stepdown_tally_t * tally) {
  if (!tally) {
    return 0;
  }
  uint64_t total = 0;
  for (size_t i = 0; i < (size_t)GCOMP_STEPDOWN_COUNT; i++) {
    total += tally->counts[i];
  }
  return total;
}

/**
 * @brief A short name for a reason, for test failure messages.
 *
 * @param reason Reason to name.
 * @return A static string; "unknown" for a value out of range.
 */
static inline const char * gcomp_stepdown_name(gcomp_stepdown_t reason) {
  switch (reason) {
  case GCOMP_STEPDOWN_STORED_IS_SMALLER: return "stored is smaller";
  case GCOMP_STEPDOWN_FIXED_IS_SMALLER: return "fixed code is smaller";
  case GCOMP_STEPDOWN_LITERALS_NOT_WORTH_CODING:
    return "literals not worth coding";
  case GCOMP_STEPDOWN_TOO_SMALL_TO_TRY: return "too small to try";
  case GCOMP_STEPDOWN_CODE_REJECTED: return "code rejected";
  case GCOMP_STEPDOWN_NO_MEMORY: return "no memory";
  case GCOMP_STEPDOWN_NO_ROOM: return "no room in output";
  case GCOMP_STEPDOWN_ENCODE_FAILED: return "encode failed";
  default: return "unknown";
  }
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_STEPDOWN_H
