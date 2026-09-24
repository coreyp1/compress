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
 * @file zstd_ldm.c
 *
 * Long-distance matching.  See zstd_ldm.h for what it is and why.
 */

#include "zstd_ldm.h"

#include "../../core/endian.h"
#include "zstd_matchfinder_private.h"

#include <string.h>

/// Multiplier for the polynomial rolling hash.  An odd constant, so that
/// multiplying by it is invertible modulo 2^64 and no information is lost.
#define ZSTD_LDM_PRIME 0x9E3779B97F4A7C15ull

/// Spreads the rolling hash across the table.  The high bits of a
/// multiplicative hash are the well-mixed ones, which is why the shift takes
/// from the top.
static inline size_t zstd_ldm_index(const zstd_ldm_t * ldm, uint64_t hash) {
  return (size_t)((hash * ZSTD_LDM_PRIME) >> (64u - ldm->hash_log));
}

/// The rolling hash of the @ref zstd_ldm_t::min_match bytes at @p p.
static inline uint64_t zstd_ldm_hash_at(
    const zstd_ldm_t * ldm, const uint8_t * p) {
  uint64_t h = 0;
  for (unsigned i = 0; i < ldm->min_match; i++) {
    h = h * ZSTD_LDM_PRIME + (uint64_t)p[i];
  }
  return h;
}

/**
 * @brief Move the hash on one byte: @p out leaves the window, @p in joins it.
 *
 * `hash` is the sum of `b[i] * P^(n-1-i)`, so the byte leaving is the one
 * carrying `P^(n-1)` -- not `P^n`.  Getting that exponent wrong does not make
 * the hash weak, it makes it *not a function of the window at all*: the value
 * at a position then depends on every byte since the last time it was
 * computed from scratch, so two identical stretches hash differently and
 * nothing is ever found.  It is worth stating because the mistake is nearly
 * invisible in a ratio test -- a file whose two copies happen to sit a whole
 * number of blocks apart still matches, because every block begins with a
 * hash computed from scratch.
 */
static inline uint64_t zstd_ldm_roll(
    const zstd_ldm_t * ldm, uint64_t hash, uint8_t out, uint8_t in) {
  return (hash - (uint64_t)out * ldm->power) * ZSTD_LDM_PRIME + (uint64_t)in;
}

size_t zstd_ldm_memory_estimate(
    size_t window_size, unsigned hash_log, unsigned hash_rate_log) {
  if (hash_log == 0u) {
    // One entry per sampled position in the window, rounded up to a power of
    // two, so that a full window can be indexed without the table thrashing.
    size_t sampled = (window_size >> hash_rate_log) + 1u;
    hash_log = ZSTD_LDM_HASH_LOG_MIN;
    while (hash_log < ZSTD_LDM_HASH_LOG_MAX &&
        ((size_t)1u << hash_log) < sampled) {
      hash_log++;
    }
  }
  if (hash_log > ZSTD_LDM_HASH_LOG_MAX) {
    hash_log = ZSTD_LDM_HASH_LOG_MAX;
  }
  return ((size_t)1u << hash_log) * sizeof(uint64_t);
}

gcomp_status_t zstd_ldm_init(zstd_ldm_t * ldm, const gcomp_allocator_t * alloc,
    size_t window_size, unsigned min_match, unsigned hash_log,
    unsigned hash_rate_log, gcomp_memory_tracker_t * mem_tracker) {
  if (!ldm) {
    return GCOMP_ERR_INVALID_ARG;
  }
  memset(ldm, 0, sizeof(*ldm));

  if (min_match == 0u) {
    min_match = ZSTD_LDM_MIN_MATCH_DEFAULT;
  }
  if (min_match < ZSTD_LDM_MIN_MATCH_MIN ||
      min_match > ZSTD_LDM_MIN_MATCH_MAX) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (hash_rate_log > ZSTD_LDM_HASH_RATE_LOG_MAX) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (hash_log != 0u &&
      (hash_log < ZSTD_LDM_HASH_LOG_MIN || hash_log > ZSTD_LDM_HASH_LOG_MAX)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (hash_log == 0u) {
    size_t sampled = (window_size >> hash_rate_log) + 1u;
    hash_log = ZSTD_LDM_HASH_LOG_MIN;
    while (hash_log < ZSTD_LDM_HASH_LOG_MAX &&
        ((size_t)1u << hash_log) < sampled) {
      hash_log++;
    }
  }

  ldm->hash_log = hash_log;
  ldm->table_size = (size_t)1u << hash_log;
  ldm->table_mask = ldm->table_size - 1u;
  ldm->min_match = min_match;
  ldm->hash_rate_log = hash_rate_log;
  ldm->rate_mask = ((size_t)1u << hash_rate_log) - 1u;
  ldm->max_offset = window_size;
  ldm->allocator = alloc;

  // P^(min_match - 1): the weight the byte about to leave the window carries.
  // See zstd_ldm_roll().
  ldm->power = 1u;
  for (unsigned i = 0; i + 1u < min_match; i++) {
    ldm->power *= ZSTD_LDM_PRIME;
  }

  ldm->table = gcomp_calloc(alloc, ldm->table_size, sizeof(uint64_t));
  if (!ldm->table) {
    return GCOMP_ERR_MEMORY;
  }
  // Tracked after the fact, like the match finder's tables: the limit itself
  // is answered before anything is allocated, through
  // zstd_ldm_memory_estimate(), because a window log the caller chose can ask
  // for gigabytes.
  if (mem_tracker) {
    gcomp_memory_track_alloc(mem_tracker, ldm->table_size * sizeof(uint64_t));
  }
  return GCOMP_OK;
}

void zstd_ldm_destroy(zstd_ldm_t * ldm, const gcomp_allocator_t * alloc,
    gcomp_memory_tracker_t * mem_tracker) {
  if (!ldm) {
    return;
  }
  if (ldm->table) {
    if (mem_tracker) {
      gcomp_memory_track_free(
          mem_tracker, ldm->table_size * sizeof(uint64_t));
    }
    gcomp_free(alloc, ldm->table);
  }
  if (ldm->matches) {
    // Not un-charged, because zstd_ldm_reserve() does not charge it -- see the
    // comment there. Releasing a charge that was never made would drive the
    // counter down past what the table costs and quietly raise the effective
    // memory limit, which is the opposite of what tracking is for.
    gcomp_free(alloc, ldm->matches);
  }
  memset(ldm, 0, sizeof(*ldm));
}

void zstd_ldm_reset(zstd_ldm_t * ldm) {
  if (!ldm) {
    return;
  }
  if (ldm->table) {
    memset(ldm->table, 0, ldm->table_size * sizeof(uint64_t));
  }
  // The list is kept; it is about to be refilled by the next scan and its
  // capacity is worth holding onto.
  ldm->match_count = 0;
  ldm->base_pos = 0;
  ldm->next_scan = 0;
}

void zstd_ldm_slide(zstd_ldm_t * ldm, size_t shift) {
  if (!ldm || shift == 0u) {
    return;
  }
  // Absolute positions, so the table itself does not move.  An entry now
  // behind the base names a byte that is no longer in the buffer, and the
  // check in the scan rejects it.
  ldm->base_pos += shift;
}

/// Make room for one more match, growing geometrically.
static bool zstd_ldm_reserve(zstd_ldm_t * ldm) {
  if (ldm->match_count < ldm->match_capacity) {
    return true;
  }
  size_t want = ldm->match_capacity ? ldm->match_capacity * 2u : 64u;
  zstd_ldm_match_t * grown = gcomp_realloc(
      ldm->allocator, ldm->matches, want * sizeof(zstd_ldm_match_t));
  if (!grown) {
    return false;
  }
  // The match list is deliberately NOT charged against the memory tracker, and
  // this is the only allocation in the encoder that is not.
  //
  // gcomp_memory_tracker_t is one size_t with no synchronisation - "Initialize
  // with zeros before use" is the whole contract - and this function is the one
  // place a zstd allocation happens on a **worker** thread: in parallel mode the
  // scan runs inside a job, and the tracker it would report to belongs to the
  // encoder, which the main thread is writing to as it allocates the next job.
  // ThreadSanitizer says so precisely, and it was right: limits.c:270 read by a
  // pool worker against limits.c:255 written by main.
  //
  // Charging it would mean either a lock in the scan's inner growth path or a
  // per-job tracker folded in at job boundaries, for an amount that does not
  // matter: entries are 12 bytes, a job's list holds the long matches in one
  // job's content, and the table -- megabytes, and the allocation
  // limits.max_memory_bytes exists to catch -- is charged where it is made, on
  // the main thread, at zstd_ldm_init(). ZstdLdm.IsChargedAgainstTheMemoryLimit
  // is about that table.
  ldm->matches = grown;
  ldm->match_capacity = want;
  return true;
}

gcomp_status_t zstd_ldm_scan(zstd_ldm_t * ldm, const uint8_t * data,
    size_t from, size_t to, size_t data_size) {
  if (!ldm || !ldm->table || !data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // The previous block's matches are spent; a parse only ever asks about the
  // block it is compressing.
  ldm->match_count = 0;

  const size_t min_match = ldm->min_match;
  if (to > data_size) {
    to = data_size;
  }
  if (from + min_match > to) {
    ldm->next_scan = ldm->base_pos + (to > from ? to : from);
    return GCOMP_OK;
  }

  // Never scan a position twice: the table must see each position once, and a
  // second insertion of the same position would be harmless but wasted.  The
  // previous call left next_scan behind it, in absolute terms.
  size_t p = from;
  if (ldm->next_scan > ldm->base_pos && ldm->next_scan - ldm->base_pos > p) {
    p = ldm->next_scan - ldm->base_pos;
  }
  if (p + min_match > to) {
    ldm->next_scan = ldm->base_pos + to;
    return GCOMP_OK;
  }

  uint64_t hash = zstd_ldm_hash_at(ldm, data + p);
  const size_t limit = to - min_match;

  for (;;) {
    const size_t idx = zstd_ldm_index(ldm, hash) & ldm->table_mask;
    const uint64_t entry = ldm->table[idx];

    if (entry != 0u) {
      const size_t cand_abs = (size_t)(entry - 1u);
      // An entry from before the current window names bytes that have been
      // slid out of the buffer; it cannot be read, and an offset reaching
      // that far would in any case exceed the declared window, which RFC 8878
      // section 3.1.1.1.2 does not permit.
      if (cand_abs >= ldm->base_pos) {
        const size_t cand = cand_abs - ldm->base_pos;
        if (cand < p && (p - cand) <= ldm->max_offset &&
            cand + min_match <= data_size) {
          // Confirm by comparison.  A 64-byte minimum makes a false hit
          // cheap: it nearly always fails within the first eight bytes.
          if (memcmp(data + cand, data + p, min_match) == 0) {
            size_t length = min_match +
                zstd_mf_count_match(data + p + min_match,
                    data + cand + min_match, data + data_size);

            if (!zstd_ldm_reserve(ldm)) {
              return GCOMP_ERR_MEMORY;
            }
            ldm->matches[ldm->match_count].pos = p;
            ldm->matches[ldm->match_count].length = (uint32_t)length;
            ldm->matches[ldm->match_count].offset = (uint32_t)(p - cand);
            ldm->match_count++;

            // Index the position that produced the match, then step over the
            // whole of it: a match starting inside one already found would
            // cover bytes the parse has taken, and the parse cannot use it.
            ldm->table[idx] = (uint64_t)(ldm->base_pos + p) + 1u;
            p += length;
            if (p > limit) {
              break;
            }
            hash = zstd_ldm_hash_at(ldm, data + p);
            continue;
          }
        }
      }
    }

    // Insert one position in every `1 << hash_rate_log`, counted absolutely
    // so that the sampling does not shift when the window moves.
    if ((((size_t)(ldm->base_pos + p)) & ldm->rate_mask) == 0u) {
      ldm->table[idx] = (uint64_t)(ldm->base_pos + p) + 1u;
    }

    if (p >= limit) {
      break;
    }
    hash = zstd_ldm_roll(ldm, hash, data[p], data[p + min_match]);
    p++;
  }

  ldm->next_scan = ldm->base_pos + to;
  return GCOMP_OK;
}

const zstd_ldm_match_t * zstd_ldm_at(zstd_ldm_t * ldm, size_t pos) {
  if (!ldm || ldm->match_count == 0u) {
    return NULL;
  }
  // A binary search rather than a cursor that walks forward with the parse.
  // The shortest-path parse re-parses a segment at the levels that ask for
  // two passes (zstd_optimal.c), so it asks about the same positions twice
  // and in decreasing order across the boundary; a cursor would have moved
  // past them and the second pass would see no long matches at all.
  size_t lo = 0;
  size_t hi = ldm->match_count;
  while (lo < hi) {
    const size_t mid = lo + ((hi - lo) >> 1);
    if (ldm->matches[mid].pos < pos) {
      lo = mid + 1u;
    }
    else {
      hi = mid;
    }
  }
  if (lo < ldm->match_count && ldm->matches[lo].pos == pos) {
    return &ldm->matches[lo];
  }
  return NULL;
}
