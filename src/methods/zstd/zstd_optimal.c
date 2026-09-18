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
 *   every path so it knows when it has one.
 *
 * WHERE THE STATISTICS COME FROM
 * ==============================
 *
 * The first block has nothing behind it, so literals are priced from a
 * histogram of the block's own bytes -- an over-estimate, since the bytes
 * inside matches will not end up as literals, but a far better one than
 * assuming eight bits each -- and the three sequence alphabets are primed
 * from the distributions the specification fixes for Predefined mode
 * (section 3.1.1.3.2.2).  After each block the counts of what was actually
 * emitted are folded in, halving what was there before, so the model tracks
 * the file as it goes and the most recent block weighs the most.
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
 * - A match at least `nice_length` long ends the segment at once and is
 *   taken as it stands.  This is the same rule the match finder uses to stop
 *   searching, applied to parsing, and it is what stops a run of identical
 *   bytes from costing time quadratic in its length.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#ifdef GCOMP_TEST_BUILD
#include <assert.h>
#endif
#include "zstd_internal.h"
#include "zstd_matchfinder_private.h"
#include "zstd_sequences_private.h"
#include <string.h>

//
// Prices
//

/// Fractional bits a price carries.  A price of ZSTD_OPT_PRICE_ONE is one bit.
#define ZSTD_OPT_PRICE_SHIFT 8
#define ZSTD_OPT_PRICE_ONE (1u << ZSTD_OPT_PRICE_SHIFT)

/// No path reaches here yet.  Large enough that adding any single edge to it
/// cannot wrap: the dearest edge is well under a thousand bits.
#define ZSTD_OPT_PRICE_INF 0xF0000000u

// Both bounds are set per level -- `opt_segment` and `opt_budget` in the
// effort table -- because both buy ratio with time, which is what a level
// is.  On 9 MB of manuals, C source and XML at level 19, holding one at its
// default and moving the other:
//
//   positions per sweep    256    1024    4096   16384   65536
//   output bytes       1422403 1401094 1394698 1393091 1391564
//
//   shortenings per position 0       8      32     128     512
//   output bytes       1419279 1406833 1397301 1394698 1394370
//
// Both flatten out, which is why the top level is not simply given the
// largest of each that fits: past a point, more sweeping buys hundredths of
// a percent for whole multiples of the time.

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
};

/**
 * @brief log2(@p x) in 256ths, for x >= 1.
 *
 * The whole part is the position of the top set bit.  The fraction comes out
 * one bit at a time by repeatedly squaring what is left: squaring doubles a
 * logarithm, so whether the square has reached 2 is exactly the next bit of
 * the answer.  Eight rounds give eight fractional bits.
 *
 * The mantissa is held with 31 fraction bits, so the square fits a 64-bit
 * product with nothing to spare and nothing lost.
 */
uint32_t zstd_opt_log2(uint32_t x) {
  if (x < 1u) {
    x = 1u;
  }
  unsigned hb = 31u - (unsigned)__builtin_clz(x);
  uint32_t result = (uint32_t)hb << ZSTD_OPT_PRICE_SHIFT;
  uint64_t m = ((uint64_t)x << 31) >> hb; // 1.0 <= m < 2.0, 31 fraction bits
  for (unsigned i = 0; i < ZSTD_OPT_PRICE_SHIFT; i++) {
    m = (m * m) >> 31;
    if (m >= ((uint64_t)1 << 32)) {
      m >>= 1;
      result += 1u << (ZSTD_OPT_PRICE_SHIFT - 1u - i);
    }
  }
  return result;
}

/**
 * @brief Turn counts into prices: -log2(count / total), floored.
 *
 * The floor is what the format can actually charge.  A literal is Huffman
 * coded, and no Huffman code is shorter than one bit, so pricing a very
 * common byte at a third of a bit would be a promise the encoder cannot
 * keep.  An FSE-coded sequence code genuinely can cost less than a bit, so
 * its floor is only there to keep an edge from being free.
 */
static void zstd_opt_price_from_freq(const uint32_t * freq, uint32_t * price,
    size_t count, uint32_t floor_price) {
  uint32_t total = 0;
  for (size_t i = 0; i < count; i++) {
    total += freq[i];
  }
  uint32_t log_total = zstd_opt_log2(total);
  for (size_t i = 0; i < count; i++) {
    uint32_t p = log_total - zstd_opt_log2(freq[i]);
    price[i] = (p < floor_price) ? floor_price : p;
  }
}

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
  zstd_opt_price_from_freq(
      st->lit_freq, st->lit_price, 256, ZSTD_OPT_PRICE_ONE);
  zstd_opt_price_from_freq(st->ll_freq, st->ll_price, ZSTD_SEQ_LL_CODES,
      ZSTD_OPT_PRICE_ONE / 8u);
  zstd_opt_price_from_freq(st->ml_freq, st->ml_price, ZSTD_SEQ_ML_CODES,
      ZSTD_OPT_PRICE_ONE / 8u);
  zstd_opt_price_from_freq(st->of_freq, st->of_price, ZSTD_SEQ_OF_CODES,
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
 * When there are literals before the match, a distance equal to one of the
 * three most recently used is written as 1, 2 or 3.  With no literals the
 * three codes mean something else (RFC 8878 section 3.1.1.3.2.1.1), and
 * rather than encode that shift this parse writes the distance out in full,
 * which is always legal and merely costs more.
 */
static inline uint32_t zstd_opt_encode_offset(
    const uint32_t * rep, uint32_t litlen, uint32_t offset) {
  if (litlen > 0) {
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
  return offset + 3u;
}

/**
 * @brief The three repeat offsets after a sequence written as @p encoded.
 */
static inline void zstd_opt_rep_after(const uint32_t * in, uint32_t encoded,
    uint32_t offset, uint32_t * out) {
  switch (encoded) {
    case 1u: // Already the most recent; nothing moves.
      out[0] = in[0];
      out[1] = in[1];
      out[2] = in[2];
      break;
    case 2u: // Promote the second past the first.
      out[0] = in[1];
      out[1] = in[0];
      out[2] = in[2];
      break;
    case 3u: // Promote the third past both.
      out[0] = in[2];
      out[1] = in[0];
      out[2] = in[1];
      break;
    default: // A distance not in the list; it becomes the most recent.
      out[0] = offset;
      out[1] = in[0];
      out[2] = in[1];
      break;
  }
}

//
// Setup and teardown
//

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
  if (mem_tracker) {
    gcomp_memory_track_alloc(mem_tracker,
        sizeof(struct zstd_opt_state_s) +
            st->node_cap * sizeof(zstd_opt_node_t));
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
        sizeof(struct zstd_opt_state_s) +
            st->node_cap * sizeof(zstd_opt_node_t));
  }
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

/// Counts of what one block emitted, folded back into the model afterwards.
typedef struct {
  uint32_t lit[256];
  uint32_t ll[ZSTD_SEQ_LL_CODES];
  uint32_t ml[ZSTD_SEQ_ML_CODES];
  uint32_t of[ZSTD_SEQ_OF_CODES];
} zstd_opt_tally_t;

/**
 * @brief Fold one block's counts into the model, halving what was there.
 *
 * Halving is what makes this a moving average rather than a total: a block
 * counts fully, the one before it half as much, the one before that a
 * quarter.  A file whose character changes part way through is priced by the
 * part it is in.
 */
static void zstd_opt_absorb(
    struct zstd_opt_state_s * st, const zstd_opt_tally_t * tally) {
  for (size_t i = 0; i < 256; i++) {
    uint32_t v = (st->lit_freq[i] >> 1) + tally->lit[i];
    st->lit_freq[i] = v ? v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_LL_CODES; i++) {
    uint32_t v = (st->ll_freq[i] >> 1) + tally->ll[i];
    st->ll_freq[i] = v ? v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_ML_CODES; i++) {
    uint32_t v = (st->ml_freq[i] >> 1) + tally->ml[i];
    st->ml_freq[i] = v ? v : 1u;
  }
  for (size_t i = 0; i < ZSTD_SEQ_OF_CODES; i++) {
    uint32_t v = (st->of_freq[i] >> 1) + tally->of[i];
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
  zstd_opt_rep_after(here_rep, enc, offset, next_rep);

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
  if (!st->primed) {
    for (size_t i = start_pos; i < data_size; i++) {
      st->lit_freq[data[i]]++;
    }
    st->primed = true;
  }
  zstd_opt_rebuild_prices(st);

  zstd_opt_tally_t tally;
  memset(&tally, 0, sizeof(tally));

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

    size_t j = 0;
    uint32_t forced_len = 0;
    uint32_t forced_off = 0;

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
      // happens once, here, whatever the parse goes on to decide.
      zstd_mf_candidate_t cand[ZSTD_MF_MAX_CANDIDATES];
      size_t ncand = zstd_mf_find_matches(
          mf, data, p, data_size, cand, ZSTD_MF_MAX_CANDIDATES);

      // The three repeat offsets are worth trying at every position, not
      // only where the match finder happens to turn one up: they cost a code
      // and no distance, so a short match through one can beat a longer
      // match through a distance written out in full.
      uint32_t rep_len[3] = {0u, 0u, 0u};
      if (here_litlen > 0u) {
        for (unsigned r = 0; r < 3u; r++) {
          uint32_t off = here_rep[r];
          if (off == 0u || (size_t)off > max_offset || (size_t)off > p) {
            continue;
          }
          size_t rl = zstd_mf_count_match(data + p, data + p - off, limit);
          if (rl >= MF_MIN_MATCH) {
            rep_len[r] = (uint32_t)rl;
          }
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
              zstd_opt_encode_offset(here_rep, here_litlen, here_rep[r]);
          uint32_t price =
              zstd_opt_of_price(st, enc) + zstd_opt_ml_price(st, rep_len[r]);
          if (!forced_len || price < best_forced_price) {
            best_forced_price = price;
            forced_len = rep_len[r];
            forced_off = here_rep[r];
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
            here_rep, here_litlen, here_rep[r], &budget, &dirty);
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

    // `j` is where the sweep stopped, and the best path to exactly there is
    // the parse for this segment.  Walk it backwards, recording at each
    // position the edge the path leaves by, so it can then be walked
    // forwards to emit.
    {
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
      zstd_opt_rep_after(rep, enc, off, next_rep);
      rep[0] = next_rep[0];
      rep[1] = next_rep[1];
      rep[2] = next_rep[2];

      memcpy(literals_out + lit_pos, data + lit_start, lit_len);
      lit_pos += lit_len;
      for (uint32_t b = 0; b < lit_len; b++) {
        tally.lit[data[lit_start + b]]++;
      }
      tally.ll[zstd_enc_get_ll_code(lit_len)]++;
      tally.ml[zstd_enc_get_ml_code(m)]++;
      tally.of[zstd_enc_get_of_code(enc)]++;

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
      zstd_opt_rep_after(rep, enc, forced_off, next_rep);
      rep[0] = next_rep[0];
      rep[1] = next_rep[1];
      rep[2] = next_rep[2];

      memcpy(literals_out + lit_pos, data + lit_start, lit_len);
      lit_pos += lit_len;
      for (uint32_t b = 0; b < lit_len; b++) {
        tally.lit[data[lit_start + b]]++;
      }
      tally.ll[zstd_enc_get_ll_code(lit_len)]++;
      tally.ml[zstd_enc_get_ml_code(forced_len)]++;
      tally.of[zstd_enc_get_of_code(enc)]++;

      sequences[num_seq].lit_length = lit_len;
      sequences[num_seq].match_offset = enc;
      sequences[num_seq].match_length = forced_len;
      num_seq++;

      size_t match_end = pos + forced_len;
      for (size_t f = pos + 1u; f < match_end; f++) {
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
      tally.lit[data[lit_start + b]]++;
    }
  }

  zstd_opt_absorb(st, &tally);

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
