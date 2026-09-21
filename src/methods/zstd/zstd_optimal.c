/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file zstd_optimal.c
 *
 * The optimal parse for the Zstandard encoder in the Ghoti.io Compress
 * library.
 *
 * WHAT A PARSE IS CHOOSING
 * ========================
 *
 * A match finder answers "what can I match here".  A parse answers the
 * harder question: which of those matches to actually emit.  The two are not
 * the same, because a match taken at one position blocks every match that
 * starts inside it, and the block that yields the fewest bits overall is
 * often not the block of longest matches.
 *
 * zstd_matchfinder.c parses greedily, with a look-ahead of one byte at a
 * time (`lazy_depth`).  That look-ahead is myopic in a way that cannot be
 * fixed by making it deeper: it compares two candidates by a rule of thumb,
 * and it has no way to know what either choice costs three sequences later.
 *
 * This file replaces the rule of thumb with a shortest-path search.  Think
 * of the block as a graph: one vertex per byte position, one edge for "write
 * this byte as a literal", and one edge per (length, offset) pair for "emit
 * this match".  Give every edge the number of bits writing it would cost,
 * and the cheapest parse of the block is the shortest path from the first
 * position to the last.  The graph has no cycles and every edge goes
 * forward, so one sweep left to right settles it -- when the sweep reaches a
 * position, every edge into that position has already been relaxed, and its
 * cost is final.
 *
 * WHAT AN EDGE COSTS
 * ==================
 *
 * The bits are not known until the block is written, and the block cannot be
 * written until the parse has chosen it, so the costs here are an estimate.
 * It is the entropy of the symbol under the statistics of what has been
 * encoded recently: a code that has been coming up often is cheap, one that
 * has not is dear, which is what an entropy coder will charge for it.
 *
 * Prices are in 256ths of a bit, because the difference between a literal
 * costing 4.6 bits and 4.7 bits decides parses and integers of bits do not
 * carry it.
 *
 * Three things are priced exactly rather than estimated, because the format
 * fixes them:
 *
 * - The raw extra bits after a literal length, match length or offset code
 *   (RFC 8878 section 3.1.1.3.2.1).  These are written uncompressed, so
 *   their count is their cost.
 * - Which offset code a distance falls under, and so how many extra bits it
 *   drags with it (section 3.1.1.3.2.1.1).  This is why a nearer match can
 *   beat a longer one.
 * - The repeat offsets.  A distance that matches one of the three offsets
 *   most recently used is written as the code 1, 2 or 3 instead of as a
 *   distance, which is a large saving, and the parse tracks the three along
 *   every path so it knows when it has one.  Which of the three a code names
 *   shifts when a sequence has no literals before it, and a parse that
 *   chooses by cost puts matches end to end constantly, so that shifted form
 *   is the common case here rather than a corner of the specification.
 *
 * WHERE THE STATISTICS COME FROM
 * ==============================
 *
 * The first block has nothing behind it, so literals are priced from a
 * histogram of the block's own bytes -- an over-estimate, since the bytes
 * inside matches will not end up as literals, but a far better one than
 * assuming eight bits each -- and the three sequence alphabets are primed
 * from the distributions the specification fixes for Predefined mode
 * (section 3.1.1.3.2.2).
 *
 * From there the model counts what it emits, as it emits it, and the prices
 * are rebuilt at every sweep boundary.  So a block is priced not only by the
 * blocks before it but by its own first half, which matters for a file that
 * changes character part way through.  Every count is halved at the start of
 * each block, which is what makes this a moving average rather than a
 * running total: the block being parsed counts fully, the one before it half
 * as much, the one before that a quarter.
 *
 * THE TWO BOUNDS
 * ==============
 *
 * A shortest-path sweep over a whole block would need a vertex table the
 * size of the block, and a position whose match runs for thousands of bytes
 * would have thousands of edges leaving it.  Two bounds keep both finite,
 * and both are visible in the output only as a parse that is very slightly
 * worse than the true optimum:
 *
 * - The sweep runs in segments of `opt_segment` positions.  At the end of
 *   one the best path to that exact position is taken and the next segment
 *   starts there.  A boundary costs at most the few bits by which landing
 *   there is worse than landing nearby, once every few thousand bytes.
 *
 * - A match at least `nice_length` long, or one that would reach past the
 *   end of the sweep, ends the sweep at once and is taken as it stands.  The
 *   first half of that is the rule the match finder uses to stop searching,
 *   applied to parsing, and it is what stops a run of identical bytes from
 *   costing time quadratic in its length.  The second half is what keeps a
 *   long match from being lost at a boundary rather than merely cut short by
 *   one; without it, raising nice_length -- which should search harder --
 *   made the output larger.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#ifdef GCOMP_TEST_BUILD
#include <assert.h>
#endif
#include "../../core/bitcost.h"
#include "zstd_internal.h"
#include "zstd_repcodes.h"
#include "zstd_ldm.h"
#include "zstd_matchfinder_private.h"
#include "zstd_sequences_private.h"
#include <string.h>

//
// Prices
//

// Prices are in 256ths of a bit; see ../../core/bitcost.h, which both this
// parse and the deflate one cost their symbols with.
#define ZSTD_OPT_PRICE_SHIFT GCOMP_BITCOST_SHIFT
#define ZSTD_OPT_PRICE_ONE GCOMP_BITCOST_ONE

/// No path reaches here yet.  Large enough that adding any single edge to it
/// cannot wrap: the dearest edge is well under a thousand bits.
#define ZSTD_OPT_PRICE_INF 0xF0000000u

// Both bounds are set per level -- `opt_segment` and `opt_budget` in the
// effort table -- because both buy ratio with time, which is what a level
// is.  On 9 MB of manuals, C source and XML, moving one and holding the
// other:
//
//   shortenings per position 0       8      32     128     512
//   output bytes       1419279 1406833 1397301 1394698 1394370
//
// which flattens out, so the top level is not simply given the largest that
// fits: past a point it buys hundredths of a percent for multiples of the
// time.
//
// The sweep length does NOT simply want to be as large as it fits, and the
// reason is worth stating.  It is also the interval at which prices are
// rebuilt, so a longer sweep both sees further -- better -- and prices what
// it sees by staler counts -- worse.  At level 22:
//
//   positions per sweep   1024    2048    4096    8192   16384   65536
//   output bytes       1388460 1384745 1383033 1382364 1382211 1383055
//
// The two effects cross at about 16384, and going to 65536 from there is
// worse on both axes: larger output, and slower for having done more work
// to get it.

/// Literal and match lengths below this have their price in a table rather
/// than worked out from their code.  Match length codes are one-to-one with
/// lengths up to 34 and coarsen from there; this covers well past the point
/// where a length is common.
#define ZSTD_OPT_DIRECT_LEN 256u

/**
 * @brief One position in the sweep.
 *
 * `price`, `mlen`, `offset`, `litlen` and `rep` describe the best path found
 * so far that ENDS here: what it cost, what its last edge was, how many
 * literals are pending on it, and where its three repeat offsets stand.
 *
 * `out_mlen` and `out_off` are filled afterwards, by walking the finished
 * path backwards from its end, and describe the edge the chosen path LEAVES
 * by.  Having both directions is what lets the sweep run backwards and the
 * emit run forwards.
 */
typedef struct {
  uint32_t price;    ///< Bits*256 to encode everything before this position.
  uint32_t mlen;     ///< Arriving edge's match length; 0 for a literal.
  uint32_t offset;   ///< Arriving edge's distance back.
  uint32_t litlen;   ///< Literals pending, not yet charged to a sequence.
  uint32_t rep[3];   ///< The three repeat offsets along this path.
  uint32_t out_mlen; ///< Leaving edge's match length; 0 for a literal.
  uint32_t out_off;  ///< Leaving edge's distance back.
} zstd_opt_node_t;

/**
 * @brief The cost model and the sweep's table, kept across blocks.
 *
 * The frequencies are what makes it adaptive: they carry from block to block
 * so that a file's own statistics, rather than a guess, price its later
 * blocks.  They are counts, never zero, so that every symbol has a finite
 * price -- a symbol priced at infinity is one the parse would never emit,
 * and a symbol that has not come up yet is not impossible, merely unseen.
 */
struct zstd_opt_state_s {
  uint32_t lit_price[256];
  uint32_t ll_price[ZSTD_SEQ_LL_CODES];
  uint32_t ml_price[ZSTD_SEQ_ML_CODES];
  uint32_t of_price[ZSTD_SEQ_OF_CODES];

  // The same two prices for every length that comes up often, worked out
  // once a block instead of per relaxation.  A sweep asks for a match length
  // price at every length it relaxes, which is hundreds of times per
  // position, and turning a length into its code is a run of comparisons:
  // that conversion alone was 17% of encoding.
  uint32_t ll_price_len[ZSTD_OPT_DIRECT_LEN];
  uint32_t ml_price_len[ZSTD_OPT_DIRECT_LEN];

  uint32_t lit_freq[256];
  uint32_t ll_freq[ZSTD_SEQ_LL_CODES];
  uint32_t ml_freq[ZSTD_SEQ_ML_CODES];
  uint32_t of_freq[ZSTD_SEQ_OF_CODES];

  bool primed; ///< False until a block has supplied a literal histogram.

  zstd_opt_node_t * nodes;
  size_t node_cap; ///< Entries; one per position of a sweep, plus one.

  // Two-pass parsing, at the levels that ask for it.  The first pass is
  // priced by whatever came before this sweep; the second by what the first
  // pass found in it.  The match finder cannot be asked twice -- searching a
  // position is also what inserts it into the tree, and the tree requires
  // each position inserted exactly once and in order -- so the first pass
  // keeps what it found and the second reads it back.
  zstd_mf_candidate_t * cand_store; ///< node_cap * ZSTD_MF_MAX_CANDIDATES.
  uint32_t * cand_count;            ///< Candidates held for each position.
  bool two_pass;

  // The counts as they stood before a sweep folded its first pass in, so
  // that the emit can fold the real parse in from the same starting point.
  uint32_t save_lit_freq[256];
  uint32_t save_ll_freq[ZSTD_SEQ_LL_CODES];
  uint32_t save_ml_freq[ZSTD_SEQ_ML_CODES];
  uint32_t save_of_freq[ZSTD_SEQ_OF_CODES];
};

static inline uint32_t zstd_opt_ll_price_slow(
    const struct zstd_opt_state_s * st, uint32_t litlen) {
  unsigned code = zstd_enc_get_ll_code(litlen);
  return st->ll_price[code] +
      ((uint32_t)zstd_seq_ll_extra_bits[code] << ZSTD_OPT_PRICE_SHIFT);
}

static inline uint32_t zstd_opt_ml_price_slow(
    const struct zstd_opt_state_s * st, uint32_t mlen) {
  unsigned code = zstd_enc_get_ml_code(mlen);
  return st->ml_price[code] +
      ((uint32_t)zstd_seq_ml_extra_bits[code] << ZSTD_OPT_PRICE_SHIFT);
}

static void zstd_opt_rebuild_prices(struct zstd_opt_state_s * st) {
  gcomp_bitcost_from_freq(
      st->lit_freq, st->lit_price, 256, ZSTD_OPT_PRICE_ONE);
  gcomp_bitcost_from_freq(st->ll_freq, st->ll_price, ZSTD_SEQ_LL_CODES,
      ZSTD_OPT_PRICE_ONE / 8u);
  gcomp_bitcost_from_freq(st->ml_freq, st->ml_price, ZSTD_SEQ_ML_CODES,
      ZSTD_OPT_PRICE_ONE / 8u);
  gcomp_bitcost_from_freq(st->of_freq, st->of_price, ZSTD_SEQ_OF_CODES,
      ZSTD_OPT_PRICE_ONE / 8u);

  for (uint32_t n = 0; n < ZSTD_OPT_DIRECT_LEN; n++) {
    st->ll_price_len[n] = zstd_opt_ll_price_slow(st, n);
    st->ml_price_len[n] = zstd_opt_ml_price_slow(st, n);
  }
}

/**
 * @brief Cost of a literal length: its code, plus the raw bits after it.
 */
static inline uint32_t zstd_opt_ll_price(
    const struct zstd_opt_state_s * st, uint32_t litlen) {
  return (litlen < ZSTD_OPT_DIRECT_LEN) ? st->ll_price_len[litlen]
                                        : zstd_opt_ll_price_slow(st, litlen);
}

/**
 * @brief Cost of a match length: its code, plus the raw bits after it.
 */
static inline uint32_t zstd_opt_ml_price(
    const struct zstd_opt_state_s * st, uint32_t mlen) {
  return (mlen < ZSTD_OPT_DIRECT_LEN) ? st->ml_price_len[mlen]
                                      : zstd_opt_ml_price_slow(st, mlen);
}

/**
 * @brief Cost of an offset value, which carries its own code's worth of raw
 *        bits (RFC 8878 section 3.1.1.3.2.1.1).
 */
static inline uint32_t zstd_opt_of_price(
    const struct zstd_opt_state_s * st, uint32_t encoded_offset) {
  unsigned code = zstd_enc_get_of_code(encoded_offset);
  return st->of_price[code] + ((uint32_t)code << ZSTD_OPT_PRICE_SHIFT);
}

//
// Setup and teardown
//

size_t zstd_opt_memory_estimate(uint32_t opt_segment, unsigned two_pass) {
  // One entry per position of a sweep, plus the one past its end; see
  // zstd_opt_init(), which allocates exactly this.
  size_t node_cap = (size_t)opt_segment + 1u;
  size_t total =
      sizeof(struct zstd_opt_state_s) + node_cap * sizeof(zstd_opt_node_t);
  if (two_pass) {
    total += node_cap * ZSTD_MF_MAX_CANDIDATES * sizeof(zstd_mf_candidate_t) +
        node_cap * sizeof(uint32_t);
  }
  return total;
}

gcomp_status_t zstd_opt_init(zstd_match_finder_t * mf,
    const gcomp_allocator_t * alloc, gcomp_memory_tracker_t * mem_tracker) {
  struct zstd_opt_state_s * st =
      gcomp_calloc(alloc, 1, sizeof(struct zstd_opt_state_s));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }

  // One entry per position of a sweep, plus the one past its end.  A match
  // that would reach beyond it ends the sweep and is taken as it stands
  // instead of being relaxed, so nothing is ever written past this.
  st->node_cap = (size_t)mf->opt_segment + 1u;
  st->nodes = gcomp_calloc(alloc, st->node_cap, sizeof(zstd_opt_node_t));
  if (!st->nodes) {
    gcomp_free(alloc, st);
    return GCOMP_ERR_MEMORY;
  }

  st->two_pass = (mf->opt_two_pass != 0u);
  if (st->two_pass) {
    st->cand_store = gcomp_calloc(alloc,
        st->node_cap * ZSTD_MF_MAX_CANDIDATES, sizeof(zstd_mf_candidate_t));
    st->cand_count = gcomp_calloc(alloc, st->node_cap, sizeof(uint32_t));
    if (!st->cand_store || !st->cand_count) {
      gcomp_free(alloc, st->cand_store);
      gcomp_free(alloc, st->cand_count);
      gcomp_free(alloc, st->nodes);
      gcomp_free(alloc, st);
      return GCOMP_ERR_MEMORY;
    }
  }

  // Tracked once, after everything is allocated, and through the same
  // function the encoder projects with, so the three cannot disagree. It also
  // used to be tracked in two steps, and the failure path between them freed
  // the first without untracking it.
  if (mem_tracker) {
    gcomp_memory_track_alloc(mem_tracker,
        zstd_opt_memory_estimate(mf->opt_segment, mf->opt_two_pass));
  }

  mf->opt = st;
  zstd_opt_reset(mf);
  return GCOMP_OK;
}

void zstd_opt_destroy(zstd_match_finder_t * mf, const gcomp_allocator_t * alloc,
    gcomp_memory_tracker_t * mem_tracker) {
  struct zstd_opt_state_s * st = mf->opt;
  if (!st) {
    return;
  }
  if (mem_tracker) {
    gcomp_memory_track_free(mem_tracker,
        zstd_opt_memory_estimate(
            (uint32_t)(st->node_cap - 1u), st->two_pass ? 1u : 0u));
  }
  gcomp_free(alloc, st->cand_store);
  gcomp_free(alloc, st->cand_count);
  gcomp_free(alloc, st->nodes);
  gcomp_free(alloc, st);
  mf->opt = NULL;
}

void zstd_opt_reset(zstd_match_finder_t * mf) {
  struct zstd_opt_state_s * st = mf->opt;
  if (!st) {
    return;
  }

  // Back to the prior: what the specification says a typical block looks
  // like, and nothing said yet about literals.  A count of -1 in a
  // predefined distribution means "rarer than one state in the table", which
  // for pricing is the same as the rarest thing that does appear.
  for (size_t i = 0; i < 256; i++) {
    st->lit_freq[i] = 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_LL_CODES; i++) {
    int16_t v = zstd_ll_predefined_norm[i];
    st->ll_freq[i] = (v > 0) ? (uint32_t)v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_ML_CODES; i++) {
    int16_t v = zstd_ml_predefined_norm[i];
    st->ml_freq[i] = (v > 0) ? (uint32_t)v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_OF_CODES; i++) {
    // The predefined offset distribution stops at code 28; the codes above
    // it are legal but so rare the specification does not give them a share.
    int16_t v = (i < 29u) ? zstd_of_predefined_norm[i] : (int16_t)-1;
    st->of_freq[i] = (v > 0) ? (uint32_t)v : 1u;
  }
  st->primed = false;
  zstd_opt_rebuild_prices(st);
}

//
// The sweep
//

/**
 * @brief Halve every count, so that a block weighs against its predecessors.
 *
 * Called once at the start of a block, after which what the block emits is
 * counted into the same arrays as it goes.  That makes the model a moving
 * average rather than a running total: the block being parsed counts fully,
 * the one before it half as much, the one before that a quarter.  A file
 * whose character changes part way through is priced by the part it is in.
 *
 * Counting as it goes, rather than at the end, is also what lets the prices
 * follow a block from the inside: they are rebuilt at every sweep boundary,
 * so a block whose second half looks nothing like its first is not priced
 * throughout by its first.
 *
 * No count ever reaches zero.  A symbol that has not come up is not
 * impossible, only unseen, and a count of zero would price it at infinity --
 * a symbol the parse would refuse to emit however much it saved.
 */
static void zstd_opt_decay(struct zstd_opt_state_s * st) {
  for (size_t i = 0; i < 256; i++) {
    uint32_t v = st->lit_freq[i] >> 1;
    st->lit_freq[i] = v ? v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_LL_CODES; i++) {
    uint32_t v = st->ll_freq[i] >> 1;
    st->ll_freq[i] = v ? v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_ML_CODES; i++) {
    uint32_t v = st->ml_freq[i] >> 1;
    st->ml_freq[i] = v ? v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_OF_CODES; i++) {
    uint32_t v = st->of_freq[i] >> 1;
    st->of_freq[i] = v ? v : 1u;
  }
}

/**
 * @brief Offer one edge to the node it lands on, keeping it if it is cheaper.
 */
static inline void zstd_opt_relax(zstd_opt_node_t * to, uint32_t price,
    uint32_t mlen, uint32_t offset, uint32_t litlen, const uint32_t * rep) {
  if (price >= to->price) {
    return;
  }
  to->price = price;
  to->mlen = mlen;
  to->offset = offset;
  to->litlen = litlen;
  to->rep[0] = rep[0];
  to->rep[1] = rep[1];
  to->rep[2] = rep[2];
}

/**
 * @brief Relax every length of one match that the budget will pay for.
 *
 * The whole match, @p hi, is always relaxed.  Lengths from @p lo upwards are
 * relaxed while @p budget lasts: shortening a match is worth trying because
 * it moves where the next one may start, and that only changes a parse near
 * the short end.
 */
static inline void zstd_opt_relax_run(const struct zstd_opt_state_s * st,
    zstd_opt_node_t * nodes, size_t j, uint32_t lo, uint32_t hi,
    uint32_t here_price, const uint32_t * here_rep, uint32_t here_litlen,
    uint32_t offset, uint32_t * budget, size_t * dirty) {
  uint32_t enc = zstd_opt_encode_offset(here_rep, here_litlen, offset);
  uint32_t fixed = here_price + zstd_opt_of_price(st, enc) +
      zstd_opt_ll_price(st, 0u);
  uint32_t next_rep[3];
  zstd_opt_rep_after(here_rep, enc, here_litlen, offset, next_rep);

  uint32_t span = hi - lo;
  if (span > *budget) {
    span = *budget;
  }
  *budget -= span;

  for (uint32_t len = lo; len <= lo + span; len++) {
    zstd_opt_relax(&nodes[j + len], fixed + zstd_opt_ml_price(st, len), len,
        offset, 0u, next_rep);
  }
  if (lo + span < hi) {
    zstd_opt_relax(&nodes[j + hi], fixed + zstd_opt_ml_price(st, hi), hi,
        offset, 0u, next_rep);
  }
  if (j + hi > *dirty) {
    *dirty = j + hi;
  }
}

/**
 * @brief Record, at each position the path passes through, the edge it leaves
 *        by, so the path can then be walked forwards.
 *
 * @p j is where the sweep stopped, and the best path to exactly there is the
 * parse for this segment.
 */
static void zstd_opt_backtrack(zstd_opt_node_t * nodes, size_t j) {
  size_t k = j;
  while (k > 0) {
    uint32_t m = nodes[k].mlen;
    if (m == 0u) {
      nodes[k - 1u].out_mlen = 0u;
      k--;
    }
    else {
      nodes[k - m].out_mlen = m;
      nodes[k - m].out_off = nodes[k].offset;
      k -= m;
    }
  }
}

/**
 * @brief Count the symbols a backtracked path would emit, without emitting.
 *
 * The second pass of a two-pass sweep needs prices built from what the first
 * pass found in this segment, and prices are built from the counts.  So the
 * first pass's path is folded in here, the prices are rebuilt from it, and
 * the counts are put back before the real emit folds the second pass's path
 * in for good.
 *
 * It walks the path exactly as the emit does, including the same repeat
 * offset evolution, because a sequence's offset code -- and so which symbol
 * gets counted -- depends on the state the sequences before it left behind.
 * Anything less faithful would price the second pass against a stream that
 * does not exist.
 */
static void zstd_opt_fold_path(struct zstd_opt_state_s * st,
    const zstd_opt_node_t * nodes, size_t j, size_t base,
    const uint8_t * data, size_t lit_start, const uint32_t * rep_in,
    uint32_t forced_len, uint32_t forced_off) {
  uint32_t rep[3] = {rep_in[0], rep_in[1], rep_in[2]};
  size_t i = 0;

  while (i < j) {
    const uint32_t m = nodes[i].out_mlen;
    if (m == 0u) {
      i++;
      continue;
    }
    const size_t p = base + i;
    const uint32_t lit_len = (uint32_t)(p - lit_start);
    const uint32_t off = nodes[i].out_off;
    const uint32_t enc = zstd_opt_encode_offset(rep, lit_len, off);
    uint32_t next_rep[3];
    zstd_opt_rep_after(rep, enc, lit_len, off, next_rep);
    rep[0] = next_rep[0];
    rep[1] = next_rep[1];
    rep[2] = next_rep[2];

    for (uint32_t b = 0; b < lit_len; b++) {
      st->lit_freq[data[lit_start + b]]++;
    }
    st->ll_freq[zstd_enc_get_ll_code(lit_len)]++;
    st->ml_freq[zstd_enc_get_ml_code(m)]++;
    st->of_freq[zstd_enc_get_of_code(enc)]++;

    lit_start = p + m;
    i += m;
  }

  if (forced_len) {
    const size_t p = base + i;
    const uint32_t lit_len = (uint32_t)(p - lit_start);
    const uint32_t enc = zstd_opt_encode_offset(rep, lit_len, forced_off);
    for (uint32_t b = 0; b < lit_len; b++) {
      st->lit_freq[data[lit_start + b]]++;
    }
    st->ll_freq[zstd_enc_get_ll_code(lit_len)]++;
    st->ml_freq[zstd_enc_get_ml_code(forced_len)]++;
    st->of_freq[zstd_enc_get_of_code(enc)]++;
  }
}

gcomp_status_t zstd_opt_generate_sequences(zstd_match_finder_t * mf,
    const uint8_t * data, size_t data_size, size_t start_pos,
    zstd_sequence_t * sequences, size_t max_sequences,
    size_t * num_sequences_out, uint8_t * literals_out,
    size_t * literals_size_out, uint32_t * rep_offset_1,
    uint32_t * rep_offset_2, uint32_t * rep_offset_3) {
  struct zstd_opt_state_s * const st = mf->opt;
  zstd_opt_node_t * const nodes = st->nodes;
  const uint8_t * const limit = data + data_size;
  const size_t max_offset = zstd_mf_max_offset(mf);
  const uint32_t nice_length = mf->nice_length;
  const uint32_t segment = mf->opt_segment;

  // Literals have never been seen when the model is fresh, and assuming
  // every byte equally likely would price a text file's parse as if it were
  // noise.  The block's own bytes are not its literals -- what ends up
  // inside a match never reaches the literals section -- but they are the
  // same alphabet in very nearly the same proportions, and that is what the
  // price needs.
  // The long-distance sweep runs over the block before it is parsed, for the
  // same reason as in zstd_mf_generate_sequences(): its hash rolls one byte
  // at a time and needs to see every position in order, which a parse that
  // steps over the matches it takes does not do.
  if (mf->ldm) {
    gcomp_status_t ldm_status =
        zstd_ldm_scan(mf->ldm, data, start_pos, data_size, data_size);
    if (ldm_status != GCOMP_OK) {
      return ldm_status;
    }
  }

  zstd_opt_decay(st);
  if (!st->primed) {
    for (size_t i = start_pos; i < data_size; i++) {
      st->lit_freq[data[i]]++;
    }
    st->primed = true;
  }
  zstd_opt_rebuild_prices(st);

  size_t pos = start_pos;
  size_t lit_start = start_pos;
  size_t num_seq = 0;
  size_t lit_pos = 0;

  uint32_t rep[3];
  rep[0] = rep_offset_1 ? *rep_offset_1 : ZSTD_REP_OFFSET_1_INIT;
  rep[1] = rep_offset_2 ? *rep_offset_2 : ZSTD_REP_OFFSET_2_INIT;
  rep[2] = rep_offset_3 ? *rep_offset_3 : ZSTD_REP_OFFSET_3_INIT;

  // Highest node index the previous segment wrote a price into, so the next
  // one clears exactly that much and no more.
  size_t dirty = st->node_cap - 1u;

  while (pos < data_size && num_seq < max_sequences) {
    const size_t base = pos;

    size_t j = 0;
    uint32_t forced_len = 0;
    uint32_t forced_off = 0;

    // A sweep is priced by what came before it, which for the first sweep of
    // a block is the block before this one.  That is a guess about this
    // segment, and the levels at the top of the ladder can afford to check
    // it: parse the segment once, fold the parse that came out into the
    // counts, and parse it again against what the segment actually contains.
    //
    // The match finder cannot be asked the same question twice.  Searching a
    // position is also what inserts it into the tree, and the tree requires
    // every position inserted exactly once and in order -- ask again and the
    // second pass finds a tree whose head is a position later than the one
    // being searched, and the descent stops before it starts.  So the first
    // pass keeps every candidate it was given and the second reads them back
    // out of st->cand_store.
    const unsigned passes = st->two_pass ? 2u : 1u;
    if (passes > 1u) {
      memcpy(st->save_lit_freq, st->lit_freq, sizeof(st->lit_freq));
      memcpy(st->save_ll_freq, st->ll_freq, sizeof(st->ll_freq));
      memcpy(st->save_ml_freq, st->ml_freq, sizeof(st->ml_freq));
      memcpy(st->save_of_freq, st->of_freq, sizeof(st->of_freq));
    }

    for (unsigned pass = 0; pass < passes; pass++) {
    // Price this sweep by everything counted up to it, this block's own
    // sequences included.  See zstd_opt_decay().
    zstd_opt_rebuild_prices(st);

    for (size_t i = 1; i <= dirty; i++) {
      nodes[i].price = ZSTD_OPT_PRICE_INF;
    }
    dirty = 0;

    // The sweep starts where the last one ended, carrying the literal run
    // that is open across the boundary so that the run's length -- which is
    // what its code costs depend on -- stays right.
    {
      uint32_t litlen0 = (uint32_t)(pos - lit_start);
      nodes[0].price = zstd_opt_ll_price(st, litlen0);
      nodes[0].mlen = 0;
      nodes[0].offset = 0;
      nodes[0].litlen = litlen0;
      nodes[0].rep[0] = rep[0];
      nodes[0].rep[1] = rep[1];
      nodes[0].rep[2] = rep[2];
    }

    j = 0;
    forced_len = 0;
    forced_off = 0;

    while (j < segment && base + j < data_size) {
      const size_t p = base + j;

      // Everything that could reach this position has been relaxed, so its
      // price is settled and the path through it can be read off.
      const uint32_t here_price = nodes[j].price;
      const uint32_t here_litlen = nodes[j].litlen;
      uint32_t here_rep[3];
      here_rep[0] = nodes[j].rep[0];
      here_rep[1] = nodes[j].rep[1];
      here_rep[2] = nodes[j].rep[2];

      // The literal edge.  A pending run's length code is charged as though
      // a sequence followed immediately, so that two paths reaching the same
      // position are compared on equal terms; extending the run means taking
      // that charge off, adding the byte, and putting the new charge on.
      if (base + j + 1u <= data_size) {
        uint32_t next_price = here_price - zstd_opt_ll_price(st, here_litlen) +
            st->lit_price[data[p]] + zstd_opt_ll_price(st, here_litlen + 1u);
        zstd_opt_relax(
            &nodes[j + 1u], next_price, 0u, 0u, here_litlen + 1u, here_rep);
        if (j + 1u > dirty) {
          dirty = j + 1u;
        }
      }

      // Searching this position is also what inserts it into the tree, so it
      // happens once, here, whatever the parse goes on to decide -- and on a
      // second pass it must not happen at all, which is what cand_store is
      // for.
      zstd_mf_candidate_t cand_local[ZSTD_MF_MAX_CANDIDATES];
      zstd_mf_candidate_t * cand = cand_local;
      size_t ncand;
      if (pass == 0u) {
        ncand = zstd_mf_find_matches(
            mf, data, p, data_size, cand_local, ZSTD_MF_MAX_CANDIDATES);
        if (passes > 1u) {
          zstd_mf_candidate_t * slot =
              st->cand_store + (size_t)j * ZSTD_MF_MAX_CANDIDATES;
          memcpy(slot, cand_local, ncand * sizeof(zstd_mf_candidate_t));
          st->cand_count[j] = (uint32_t)ncand;
        }
      }
      else {
        cand = st->cand_store + (size_t)j * ZSTD_MF_MAX_CANDIDATES;
        ncand = st->cand_count[j];
      }

      // The distances a one-symbol code can name from here are worth trying
      // at every position, not only where the match finder happens to turn
      // one up: they cost a code and no distance, so a short match through
      // one can beat a longer match through a distance written out in full.
      //
      // Which three they are depends on whether a literal run is pending;
      // see zstd_opt_encode_offset().
      uint32_t probe_off[3];
      if (here_litlen > 0u) {
        probe_off[0] = here_rep[0];
        probe_off[1] = here_rep[1];
        probe_off[2] = here_rep[2];
      }
      else {
        probe_off[0] = here_rep[1];
        probe_off[1] = here_rep[2];
        probe_off[2] = (here_rep[0] > 1u) ? (here_rep[0] - 1u) : 0u;
      }

      uint32_t rep_len[3] = {0u, 0u, 0u};
      for (unsigned r = 0; r < 3u; r++) {
        uint32_t off = probe_off[r];
        if (off == 0u || (size_t)off > max_offset || (size_t)off > p) {
          continue;
        }
        size_t rl = zstd_mf_count_match(data + p, data + p - off, limit);
        if (rl >= MF_MIN_MATCH) {
          rep_len[r] = (uint32_t)rl;
        }
      }

      // A match that is long enough, or that would reach past the end of
      // this sweep, ends the sweep rather than being relaxed: see THE TWO
      // BOUNDS.  The second half of that rule matters as much as the first.
      // Without it a match running past the segment boundary is simply lost
      // -- the sweep would stop at the boundary and back-track through
      // whatever shorter matches happened to land on it -- and raising
      // nice_length, which should search harder, made the output larger
      // because it lost more of them.
      const uint32_t force_at =
          (nice_length < segment - (uint32_t)j)
          ? nice_length
          : segment - (uint32_t)j;
      uint32_t best_forced_price = 0;
      for (unsigned r = 0; r < 3u; r++) {
        if (rep_len[r] >= force_at) {
          uint32_t enc =
              zstd_opt_encode_offset(here_rep, here_litlen, probe_off[r]);
          uint32_t price =
              zstd_opt_of_price(st, enc) + zstd_opt_ml_price(st, rep_len[r]);
          if (!forced_len || price < best_forced_price) {
            best_forced_price = price;
            forced_len = rep_len[r];
            forced_off = probe_off[r];
          }
        }
      }
      for (size_t c = 0; c < ncand; c++) {
        if (cand[c].length >= force_at) {
          uint32_t enc =
              zstd_opt_encode_offset(here_rep, here_litlen, cand[c].offset);
          uint32_t price = zstd_opt_of_price(st, enc) +
              zstd_opt_ml_price(st, cand[c].length);
          if (!forced_len || price < best_forced_price) {
            best_forced_price = price;
            forced_len = cand[c].length;
            forced_off = cand[c].offset;
          }
        }
      }

      // A long-distance match, where the sweep found one starting here.  It
      // is priced against the others rather than taken outright, so that a
      // nearer match of the same length is still preferred -- a long offset
      // costs more bits and zstd_opt_of_price() knows by how much.  It is
      // offered only in the forcing test because it is at least
      // ldm_min_match bytes, far above any force_at this parse uses, so
      // relaxing it as an ordinary candidate could not change the answer.
      if (mf->ldm) {
        const zstd_ldm_match_t * lm = zstd_ldm_at(mf->ldm, p);
        if (lm && lm->length >= force_at && (size_t)lm->offset <= p) {
          uint32_t enc =
              zstd_opt_encode_offset(here_rep, here_litlen, lm->offset);
          uint32_t price =
              zstd_opt_of_price(st, enc) + zstd_opt_ml_price(st, lm->length);
          if (!forced_len || price < best_forced_price) {
            best_forced_price = price;
            forced_len = lm->length;
            forced_off = lm->offset;
          }
        }
      }
      if (forced_len) {
        break;
      }

      uint32_t budget = mf->opt_budget;

      // A repeat offset's whole length, and shortenings of it.
      for (unsigned r = 0; r < 3u; r++) {
        if (rep_len[r] == 0u) {
          continue;
        }
        zstd_opt_relax_run(st, nodes, j, MF_MIN_MATCH, rep_len[r], here_price,
            here_rep, here_litlen, probe_off[r], &budget, &dirty);
      }

      // The match finder's candidates, shortest first.  Each one covers the
      // lengths the one before it could not reach, so no length is priced
      // twice against a worse offset.
      uint32_t covered = MF_MIN_MATCH - 1u;
      for (size_t c = 0; c < ncand; c++) {
        if (cand[c].length <= covered) {
          continue;
        }
        zstd_opt_relax_run(st, nodes, j, covered + 1u, cand[c].length,
            here_price, here_rep, here_litlen, cand[c].offset, &budget,
            &dirty);
        covered = cand[c].length;
      }

      j++;
    }

    // Between the two passes: what the first one found becomes what the
    // second one is priced by.  The counts go back afterwards so that the
    // emit folds in the parse that is actually written, once.
    if (pass + 1u < passes) {
      zstd_opt_backtrack(nodes, j);

      // The counts the first pass inherited are halved before its own parse
      // is folded in, the same way a block start halves what came before it.
      // Without that the segment's sixteen thousand positions are weighed
      // against everything the block has counted so far and barely move the
      // prices: folding on top of the full history was worth 0.049% at level
      // 22 where halving first is worth 0.103%.
      for (size_t c = 0; c < 256u; c++) {
        st->lit_freq[c] = (st->lit_freq[c] + 1u) >> 1u;
      }
      for (size_t c = 0; c < ZSTD_SEQ_LL_CODES; c++) {
        st->ll_freq[c] = (st->ll_freq[c] + 1u) >> 1u;
      }
      for (size_t c = 0; c < ZSTD_SEQ_ML_CODES; c++) {
        st->ml_freq[c] = (st->ml_freq[c] + 1u) >> 1u;
      }
      for (size_t c = 0; c < ZSTD_SEQ_OF_CODES; c++) {
        st->of_freq[c] = (st->of_freq[c] + 1u) >> 1u;
      }

      zstd_opt_fold_path(
          st, nodes, j, base, data, lit_start, rep, forced_len, forced_off);
    }
    } // for each pass

    if (passes > 1u) {
      memcpy(st->lit_freq, st->save_lit_freq, sizeof(st->lit_freq));
      memcpy(st->ll_freq, st->save_ll_freq, sizeof(st->ll_freq));
      memcpy(st->ml_freq, st->save_ml_freq, sizeof(st->ml_freq));
      memcpy(st->of_freq, st->save_of_freq, sizeof(st->of_freq));
    }

    // `j` is where the sweep stopped, and the best path to exactly there is
    // the parse for this segment.  Walk it backwards, recording at each
    // position the edge the path leaves by, so it can then be walked
    // forwards to emit.
    zstd_opt_backtrack(nodes, j);

    // Emit forwards.
    size_t i = 0;
    while (i < j) {
      uint32_t m = nodes[i].out_mlen;
      if (m == 0u) {
        i++;
        continue;
      }
      if (num_seq >= max_sequences) {
        break;
      }
      const size_t p = base + i;
      const uint32_t lit_len = (uint32_t)(p - lit_start);
      const uint32_t off = nodes[i].out_off;
#ifdef GCOMP_TEST_BUILD
      // The parse priced this sequence against a repeat offset state it
      // predicted.  If the prediction and what is written here ever came
      // apart, every price after the first divergence would be for a
      // different stream than the one being produced.
      assert(nodes[i].rep[0] == rep[0] && nodes[i].rep[1] == rep[1] &&
          nodes[i].rep[2] == rep[2]);
      assert(nodes[i].litlen == lit_len);
      assert(off > 0 && (size_t)off <= p);
#endif
      uint32_t enc = zstd_opt_encode_offset(rep, lit_len, off);
      uint32_t next_rep[3];
      zstd_opt_rep_after(rep, enc, lit_len, off, next_rep);
      rep[0] = next_rep[0];
      rep[1] = next_rep[1];
      rep[2] = next_rep[2];

      memcpy(literals_out + lit_pos, data + lit_start, lit_len);
      lit_pos += lit_len;
      for (uint32_t b = 0; b < lit_len; b++) {
        st->lit_freq[data[lit_start + b]]++;
      }
      st->ll_freq[zstd_enc_get_ll_code(lit_len)]++;
      st->ml_freq[zstd_enc_get_ml_code(m)]++;
      st->of_freq[zstd_enc_get_of_code(enc)]++;

      sequences[num_seq].lit_length = lit_len;
      sequences[num_seq].match_offset = enc;
      sequences[num_seq].match_length = m;
      num_seq++;

      lit_start = p + m;
      i += m;
    }

    pos = base + i;
    if (i < j) {
      break; // Ran out of room for sequences; the rest becomes literals.
    }

    // The match that ended the sweep, if one did.  Its own position was
    // searched; the positions it covers were not, and must be put into the
    // tree before the next sweep reaches past them.
    if (forced_len && num_seq < max_sequences) {
      const uint32_t lit_len = (uint32_t)(pos - lit_start);
#ifdef GCOMP_TEST_BUILD
      // Same check as in the loop above: the sweep decided this match was
      // worth forcing under a repeat offset state it predicted, and this is
      // where that prediction has to hold.
      assert(nodes[j].rep[0] == rep[0] && nodes[j].rep[1] == rep[1] &&
          nodes[j].rep[2] == rep[2]);
      assert(nodes[j].litlen == lit_len);
      assert(forced_off > 0 && (size_t)forced_off <= pos);
      assert(pos + forced_len <= data_size);
#endif
      uint32_t enc = zstd_opt_encode_offset(rep, lit_len, forced_off);
      uint32_t next_rep[3];
      zstd_opt_rep_after(rep, enc, lit_len, forced_off, next_rep);
      rep[0] = next_rep[0];
      rep[1] = next_rep[1];
      rep[2] = next_rep[2];

      memcpy(literals_out + lit_pos, data + lit_start, lit_len);
      lit_pos += lit_len;
      for (uint32_t b = 0; b < lit_len; b++) {
        st->lit_freq[data[lit_start + b]]++;
      }
      st->ll_freq[zstd_enc_get_ll_code(lit_len)]++;
      st->ml_freq[zstd_enc_get_ml_code(forced_len)]++;
      st->of_freq[zstd_enc_get_of_code(enc)]++;

      sequences[num_seq].lit_length = lit_len;
      sequences[num_seq].match_offset = enc;
      sequences[num_seq].match_length = forced_len;
      num_seq++;

      size_t match_end = pos + forced_len;
      size_t fill_to = (forced_len >= nice_length) ? (pos + 1u) : match_end;
      for (size_t f = pos + 1u; f < fill_to; f++) {
        zstd_mf_insert_one(mf, data, f, data_size);
      }
      pos = match_end;
      lit_start = pos;
    }
    else if (pos == base) {
      // Nothing was emitted and nothing forced: the sweep covered no ground,
      // which can only happen at the very end of the buffer where no edge
      // leaves the first position.  The rest is literals.
      break;
    }
  }

  if (lit_start < data_size) {
    size_t remaining = data_size - lit_start;
    memcpy(literals_out + lit_pos, data + lit_start, remaining);
    lit_pos += remaining;
    for (size_t b = 0; b < remaining; b++) {
      st->lit_freq[data[lit_start + b]]++;
    }
  }

  *num_sequences_out = num_seq;
  *literals_size_out = lit_pos;
  if (rep_offset_1) {
    *rep_offset_1 = rep[0];
  }
  if (rep_offset_2) {
    *rep_offset_2 = rep[1];
  }
  if (rep_offset_3) {
    *rep_offset_3 = rep[2];
  }
  return GCOMP_OK;
}
