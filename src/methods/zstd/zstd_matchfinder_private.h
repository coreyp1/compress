/**
 * @file zstd_matchfinder_private.h
 *
 * Pieces of the Zstandard match finder that the optimal parse also needs.
 *
 * The greedy and deferred parses live in zstd_matchfinder.c alongside the
 * tables they walk.  The optimal parse is large enough to want its own file
 * and asks the same tables the same questions, so what the two share sits
 * here rather than being duplicated or exported through zstd_internal.h --
 * these are inner-loop operations and an out-of-line call would cost more
 * than the work they do.
 *
 * Included only by zstd_matchfinder.c and zstd_optimal.c.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_MATCHFINDER_PRIVATE_H
#define GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_MATCHFINDER_PRIVATE_H

#include <ghoti.io/compress/macros.h>

#include "../../core/endian.h"
#include "zstd_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MF_MIN_MATCH 3           ///< Minimum match length
#define MF_HASH_READ_SIZE 4      ///< Bytes read by hash function (must be >= 4)
#define MF_MAX_DISTANCE 0x7FFFFF ///< Maximum match distance (~8MB for blocks)

/// Matches one search may report.  A descent records one per improvement and
/// takes at most `search_depth` steps, and no level asks for more than this,
/// so the cap is a bound on the array rather than on the search.
#define ZSTD_MF_MAX_CANDIDATES 64

/**
 * @brief One match a search turned up: this many bytes, this far back.
 *
 * A search reports these in increasing length order, and no two have the
 * same length, so each one is the cheapest way the finder knows of covering
 * its own length -- which is what a parse choosing between them needs.
 */
typedef struct {
  uint32_t length; ///< Bytes that match.
  uint32_t offset; ///< Distance back to the match's source.
} zstd_mf_candidate_t;

/**
 * @brief Count how many bytes match at two positions.
 *
 * Eight bytes at a time while eight remain.  Both reads are in bounds: the
 * loop only runs while p1 + 8 is within p1_end, which is the end of the
 * buffer, and p2 is always behind p1 -- a match source is earlier than the
 * position matching it -- so p2 + 8 is further inside the buffer still.
 *
 * Where the two words differ, the first differing byte is the lowest
 * differing bit of their exclusive-or, divided by eight.  Reading both
 * little-endian puts the earliest byte in memory in the low bits, so this
 * counts forwards through memory on either byte order.
 *
 * This is the comparison that decides every candidate match, and one byte
 * per iteration made it 5% of encoding on its own.
 */
static inline size_t zstd_mf_count_match(
    const uint8_t * p1, const uint8_t * p2, const uint8_t * p1_end) {
  const uint8_t * anchor = p1;

  while (p1 + 8 <= p1_end) {
    uint64_t a = gcomp_read_le64(p1);
    uint64_t b = gcomp_read_le64(p2);
    if (a != b) {
      return (size_t)(p1 - anchor) +
          (size_t)((unsigned)__builtin_ctzll(a ^ b) >> 3);
    }
    p1 += 8;
    p2 += 8;
  }

  while (p1 < p1_end && *p1 == *p2) {
    p1++;
    p2++;
  }
  return (size_t)(p1 - anchor);
}

/**
 * @brief How far back a sequence may reach, for this finder.
 *
 * The window the frame header declares is a promise to the decoder about how
 * much history it must keep (RFC 8878 section 3.1.1.1.2); reaching past it
 * would produce a frame the decoder cannot read.  The tree has a second,
 * smaller limit of its own -- see the sizing in zstd_mf_init().
 */
static inline size_t zstd_mf_max_offset(const zstd_match_finder_t * mf) {
  size_t max_offset = mf->window_size;
  if (max_offset > MF_MAX_DISTANCE) {
    max_offset = MF_MAX_DISTANCE;
  }
  if (mf->use_bt && max_offset > mf->bt_size - 1u) {
    max_offset = mf->bt_size - 1u;
  }
  return max_offset;
}

/**
 * @brief Search and insert @p pos, reporting every match met on the way.
 *
 * The same descent zstd_mf_find_match() makes, reporting the whole improving
 * sequence rather than only its last member.  A parse that prices candidates
 * against one another needs the short-and-near ones as well as the long-and-
 * far one: a longer match through a more distant offset can cost more bits
 * than a shorter one close by.
 *
 * @param mf Match finder context.
 * @param data Window contents.
 * @param pos Position to search, which is also inserted.
 * @param data_size Total bytes in @p data.
 * @param out Receives the matches, in increasing length order.
 * @param out_cap Entries @p out can hold.
 * @return Matches written to @p out.
 */
size_t zstd_mf_find_matches(zstd_match_finder_t * mf, const uint8_t * data,
    size_t pos, size_t data_size, zstd_mf_candidate_t * out, size_t out_cap);

/**
 * @brief Insert @p pos into the finder's tables without reporting anything.
 */
void zstd_mf_insert_one(
    zstd_match_finder_t * mf, const uint8_t * data, size_t pos,
    size_t data_size);

/**
 * @brief Allocate the optimal parse's cost model and table.
 *
 * Reads mf->nice_length, which must already be set: it fixes how far a
 * match leaving the last position of a sweep can reach, and so how large
 * the table has to be.
 */
gcomp_status_t zstd_opt_init(zstd_match_finder_t * mf,
    const gcomp_allocator_t * alloc, gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief Release what zstd_opt_init() allocated.
 */
void zstd_opt_destroy(zstd_match_finder_t * mf, const gcomp_allocator_t * alloc,
    gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief Put the cost model back to its prior, forgetting the stream.
 */
void zstd_opt_reset(zstd_match_finder_t * mf);

/**
 * @brief log2(@p x) in 256ths of a bit, for x >= 1.
 *
 * Every price in the optimal parse is a difference of two of these, so it is
 * exposed for its own test: an error here does not fail, it quietly makes
 * every parse a little worse.
 */
uint32_t zstd_opt_log2(uint32_t x);

/**
 * @brief The optimal parse; see zstd_optimal.c.
 *
 * Signature matches zstd_mf_generate_sequences(), which dispatches to this
 * at the levels whose effort entry asks for it.
 */
gcomp_status_t zstd_opt_generate_sequences(zstd_match_finder_t * mf,
    const uint8_t * data, size_t data_size, size_t start_pos,
    zstd_sequence_t * sequences, size_t max_sequences,
    size_t * num_sequences_out, uint8_t * literals_out,
    size_t * literals_size_out, uint32_t * rep_offset_1,
    uint32_t * rep_offset_2, uint32_t * rep_offset_3);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_MATCHFINDER_PRIVATE_H */
