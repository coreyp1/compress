/**
 * @file huffman_lengths.c
 *
 * Length-limited Huffman code lengths by boundary package-merge.
 *
 * References:
 *   - L. L. Larmore and D. S. Hirschberg, "A fast algorithm for optimal
 *     length-limited Huffman codes", JACM 37(3), 1990.  The package-merge
 *     algorithm itself.
 *   - J. Katajainen, A. Moffat and A. Turpin, "A fast and space-economical
 *     algorithm for length-limited coding", ISAAC 1995.  The boundary
 *     formulation implemented here, which keeps two chains per level instead
 *     of materialising every package.
 *   - RFC 1951 section 3.2.2 (DEFLATE), RFC 8878 section 4.2.1 (Zstandard):
 *     the callers' length caps and their requirement that the lengths
 *     describe a complete code.
 *
 * WHY THIS RATHER THAN CAPPING
 * ----------------------------
 * The obvious way to respect a cap is to build a plain Huffman tree, clamp
 * everything longer than the cap, and then lengthen other symbols until the
 * code fits back inside the Kraft budget.  That is what this library did
 * before, and it is not optimal: clamping is a local repair that cannot see
 * which lengthening costs fewest bits, so it routinely spends the budget in
 * the wrong place.  The Zstandard copy of it lengthened whichever symbol had
 * the *shortest* code - the most frequent symbol in the block - which is the
 * single most expensive choice available.
 *
 * Package-merge decides the whole assignment at once.  Think of each symbol
 * as buying up to max_bits coins of denominations 2^-1 .. 2^-max_bits; a
 * symbol that buys k coins gets code length k.  A complete code spends
 * exactly n - 1 in total, so the problem is to buy that total as cheaply as
 * possible, where a coin of denomination 2^-l for symbol i costs freq[i].
 * Sorting each denomination's coins and packaging pairs upward turns that
 * into a linear scan per level, and the cheapest purchase is read straight
 * off the result.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "huffman_lengths.h"

#include "alloc_internal.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief One leaf: a symbol that occurs at least once.
 */
typedef struct {
  uint64_t weight; ///< Frequency of the symbol.
  uint32_t symbol; ///< Index into the caller's frequency array.
} huff_leaf_t;

/**
 * @brief One chain in the boundary formulation.
 *
 * A chain stands for a prefix of the merged list at its level: @c count says
 * how many of the sorted leaves that prefix has taken, and @c tail points at
 * the chain one level down that the packages came from.  Reading @c count off
 * every chain in a tail walk is all that is needed to recover the lengths.
 */
typedef struct {
  uint64_t weight; ///< Weight of the last item taken into the chain.
  uint32_t count;  ///< Leaves taken at this level.
  int32_t tail;    ///< Chain one level down, or -1.
} huff_chain_t;

typedef struct {
  huff_chain_t * pool;
  size_t pool_used;
  size_t pool_capacity;
  int overflowed; ///< Set if the capacity proof below ever fails.
  const huff_leaf_t * leaves;
  uint32_t num_leaves;
  int32_t (*lists)[2]; ///< Last two chains of each level.
} huff_pm_t;

/**
 * @brief Order leaves by weight, then by symbol.
 *
 * The tie-break on symbol is not cosmetic: package-merge reads the leaves in
 * order, so an unstable order would make the lengths depend on the sort
 * implementation rather than on the frequencies.
 */
static int huff_leaf_cmp(const void * a, const void * b) {
  const huff_leaf_t * l = (const huff_leaf_t *)a;
  const huff_leaf_t * r = (const huff_leaf_t *)b;
  if (l->weight != r->weight) {
    return l->weight < r->weight ? -1 : 1;
  }
  return l->symbol < r->symbol ? -1 : (l->symbol > r->symbol ? 1 : 0);
}

/**
 * @brief Take the next chain slot.
 *
 * The capacity is proved below, so running out means the proof is wrong
 * rather than that the input was unusual.  Recording that and reusing slot 0
 * keeps the walk inside the buffer; the caller turns the flag into an error.
 */
static int32_t huff_pm_alloc(huff_pm_t * pm) {
  if (pm->pool_used >= pm->pool_capacity) {
    pm->overflowed = 1;
    return 0;
  }
  return (int32_t)pm->pool_used++;
}

/**
 * @brief Extend one level's chain by one item.
 *
 * The next item at a level is either the next unused leaf or the package of
 * the two front chains one level down, whichever is lighter.  Taking the
 * package consumes those two chains, so the level below is extended twice to
 * restore its lookahead - that recursion is what keeps the algorithm from
 * building every package explicitly.  Depth is bounded by @c max_bits.
 */
static void huff_pm_extend(huff_pm_t * pm, unsigned level) {
  int32_t last = pm->lists[level][1];
  uint32_t lastcount = pm->pool[last].count;

  if (level == 0 && lastcount >= pm->num_leaves) {
    // Every leaf is already in the bottom list; there is nothing to take.
    return;
  }

  int32_t fresh = huff_pm_alloc(pm);
  pm->lists[level][0] = last;
  pm->lists[level][1] = fresh;

  if (level == 0) {
    pm->pool[fresh].weight = pm->leaves[lastcount].weight;
    pm->pool[fresh].count = lastcount + 1;
    pm->pool[fresh].tail = -1;
    return;
  }

  uint64_t sum = pm->pool[pm->lists[level - 1][0]].weight +
      pm->pool[pm->lists[level - 1][1]].weight;
  if (lastcount < pm->num_leaves && sum > pm->leaves[lastcount].weight) {
    // The leaf is lighter than the package, so it goes in first.  The chain
    // below is untouched, so the new chain inherits the old one's tail.
    pm->pool[fresh].weight = pm->leaves[lastcount].weight;
    pm->pool[fresh].count = lastcount + 1;
    pm->pool[fresh].tail = pm->pool[last].tail;
    return;
  }

  pm->pool[fresh].weight = sum;
  pm->pool[fresh].count = lastcount;
  pm->pool[fresh].tail = pm->lists[level - 1][1];
  huff_pm_extend(pm, level - 1);
  huff_pm_extend(pm, level - 1);
}

gcomp_status_t gcomp_huffman_code_lengths(const gcomp_allocator_t * alloc,
    const uint32_t * freq, size_t num_symbols, unsigned max_bits,
    uint8_t * lengths_out) {
  if (!freq || !lengths_out || max_bits == 0 || max_bits > 32) {
    return GCOMP_ERR_INVALID_ARG;
  }

  memset(lengths_out, 0, num_symbols);

  huff_leaf_t stack_leaves[288]; // Covers DEFLATE's largest alphabet.
  huff_leaf_t * leaves = stack_leaves;
  huff_leaf_t * heap_leaves = NULL;
  if (num_symbols > sizeof(stack_leaves) / sizeof(stack_leaves[0])) {
    heap_leaves = (huff_leaf_t *)gcomp_malloc(alloc,
        num_symbols * sizeof(huff_leaf_t));
    if (!heap_leaves) {
      return GCOMP_ERR_MEMORY;
    }
    leaves = heap_leaves;
  }

  uint32_t used = 0;
  for (size_t i = 0; i < num_symbols; i++) {
    if (freq[i] > 0) {
      leaves[used].weight = freq[i];
      leaves[used].symbol = (uint32_t)i;
      used++;
    }
  }

  // An alphabet that cannot fit under the cap is the caller's mistake: with
  // max_bits bits there are only 2^max_bits code words to hand out.
  if (max_bits < 32 && used > (1u << max_bits)) {
    gcomp_free(alloc, heap_leaves);
    return GCOMP_ERR_INVALID_ARG;
  }

  if (used == 0) {
    gcomp_free(alloc, heap_leaves);
    return GCOMP_OK;
  }
  if (used == 1) {
    // One symbol still needs a code word, and a zero-bit code word would not
    // be distinguishable from the absence of one.  Both formats expect the
    // single used symbol to be one bit; DEFLATE's decoder builds a
    // one-entry table from it.
    lengths_out[leaves[0].symbol] = 1;
    gcomp_free(alloc, heap_leaves);
    return GCOMP_OK;
  }
  if (used == 2) {
    lengths_out[leaves[0].symbol] = 1;
    lengths_out[leaves[1].symbol] = 1;
    gcomp_free(alloc, heap_leaves);
    return GCOMP_OK;
  }

  qsort(leaves, used, sizeof(huff_leaf_t), huff_leaf_cmp);

  // No code word can need more than used - 1 bits, so a cap above that is
  // slack and only costs work.
  unsigned levels = max_bits;
  if (levels > used - 1) {
    levels = used - 1;
  }

  // Capacity: each level ends with at most 2 * used - 2 chains, counting the
  // two the initialisation puts there, because that is how many items of the
  // merged list are ever needed.  2 * used per level is that bound rounded up.
  size_t capacity = (size_t)levels * 2u * (size_t)used;
  huff_chain_t * pool =
      (huff_chain_t *)gcomp_malloc(alloc, capacity * sizeof(huff_chain_t));
  int32_t (*lists)[2] =
      (int32_t(*)[2])gcomp_malloc(alloc, (size_t)levels * sizeof(int32_t[2]));
  if (!pool || !lists) {
    gcomp_free(alloc, lists);
    gcomp_free(alloc, pool);
    gcomp_free(alloc, heap_leaves);
    return GCOMP_ERR_MEMORY;
  }

  huff_pm_t pm;
  pm.pool = pool;
  pm.pool_used = 0;
  pm.pool_capacity = capacity;
  pm.overflowed = 0;
  pm.leaves = leaves;
  pm.num_leaves = used;
  pm.lists = lists;

  // Every level starts holding the two lightest leaves: the first two items
  // of any level's merged list are always leaves, since a package at that
  // level is the sum of two items and so is heavier than either.
  for (unsigned l = 0; l < levels; l++) {
    int32_t first = huff_pm_alloc(&pm);
    pool[first].weight = leaves[0].weight;
    pool[first].count = 1;
    pool[first].tail = -1;
    int32_t second = huff_pm_alloc(&pm);
    pool[second].weight = leaves[1].weight;
    pool[second].count = 2;
    pool[second].tail = -1;
    lists[l][0] = first;
    lists[l][1] = second;
  }

  // A complete binary tree over `used` leaves has 2 * used - 2 edges below the
  // root, which is how many items the top level must end up holding.  Two are
  // there already.
  uint32_t extensions = 2u * used - 4u;
  for (uint32_t i = 0; i < extensions; i++) {
    huff_pm_extend(&pm, levels - 1);
  }

  gcomp_status_t status = GCOMP_OK;
  if (pm.overflowed) {
    status = GCOMP_ERR_MEMORY;
  }
  else {
    // A leaf's code length is the number of levels that took it in: taking a
    // leaf at level l is buying it one more coin, and a symbol that bought k
    // coins has a k-bit code word.  The chains are nested, so a level that
    // took more than j leaves took leaf j.
    for (int32_t c = lists[levels - 1][1]; c >= 0; c = pool[c].tail) {
      uint32_t count = pool[c].count;
      for (uint32_t j = 0; j < count; j++) {
        lengths_out[leaves[j].symbol]++;
      }
    }
  }

  gcomp_free(alloc, lists);
  gcomp_free(alloc, pool);
  gcomp_free(alloc, heap_leaves);
  return status;
}
