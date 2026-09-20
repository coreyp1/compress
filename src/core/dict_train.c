/**
 * @file dict_train.c
 *
 * Building a dictionary from samples: FASTCOVER.
 *
 * COVER (Liao, Petri, Moffat, Wirth, WWW 2016) scores a candidate segment by
 * the total frequency of the distinct d-byte sequences it contains, takes the
 * best one, and repeats. FASTCOVER is the same algorithm with the exact
 * frequency map replaced by a fixed-size table indexed by a hash of the
 * sequence - collisions are accepted, because the score only has to rank
 * segments against one another and a collision perturbs a sum of thousands.
 *
 * ## Two details that are the whole algorithm
 *
 * **Distinct.** A segment containing the same common sequence twenty times
 * must not score twenty times for it, or the winner is always the most
 * repetitive stretch in the samples - which is the stretch an encoder would
 * have compressed perfectly well on its own. The window below keeps a count
 * per sequence and adds to the score only when that count goes from zero to
 * one.
 *
 * **Zeroing.** After a segment is chosen, the frequency of everything in it is
 * set to zero, so the next pick is about content not already covered. Without
 * that, a trainer picks the same neighbourhood repeatedly and the dictionary
 * is one idea written out many times.
 *
 * ## Order matters
 *
 * The best segment goes at the *end* of the dictionary. A dictionary is
 * history, and a match costs bits in proportion to how far back it reaches, so
 * the content most likely to be matched belongs closest to the data - which is
 * the end. libzstd does the same, and it is worth a few percent for nothing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "alloc_internal.h"
#include "registry_internal.h"
#include <ghoti.io/compress/dict.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

/// Default k: the length of the pieces a dictionary is built from.
#define GCOMP_DICT_DEFAULT_SEGMENT 256u
/// Default d: the length of the sequences whose frequency is counted.
#define GCOMP_DICT_DEFAULT_DMER 8u
/// Log2 of the frequency table. 2^18 counters is 1 MiB and ample for FASTCOVER.
#define GCOMP_DICT_FREQ_LOG 18u
#define GCOMP_DICT_FREQ_SIZE (1u << GCOMP_DICT_FREQ_LOG)

/**
 * @brief Hash a d-byte sequence into the frequency table.
 *
 * Reads eight bytes and drops the ones beyond @p d, so every length uses the
 * same multiply. The constant is the usual 64-bit Fibonacci one; taking the
 * high bits after the multiply is what mixes the low-entropy bytes of ASCII
 * into the whole index.
 */
static uint32_t gcomp_dict_hash(const uint8_t * p, unsigned d) {
  uint64_t v = 0;
  memcpy(&v, p, 8u);
  if (d < 8u) {
    v &= (~(uint64_t)0) >> ((8u - d) * 8u);
  }
  return (uint32_t)((v * 0x9E3779B97F4A7C15ull) >> (64u - GCOMP_DICT_FREQ_LOG));
}

/**
 * @brief One contiguous run of samples, with the boundaries between them.
 *
 * Segments must not straddle two samples: a piece that is half the end of one
 * record and half the start of another is a piece no message contains.
 */
typedef struct {
  uint8_t * data;
  size_t size;
  size_t * ends; ///< End offset of each sample
  size_t count;
} gcomp_dict_corpus_t;

static void gcomp_dict_corpus_free(
    const gcomp_allocator_t * alloc, gcomp_dict_corpus_t * c) {
  gcomp_free(alloc, c->data);
  gcomp_free(alloc, c->ends);
  c->data = NULL;
  c->ends = NULL;
}

/// Copy the samples end to end, recording where each stops.
static gcomp_status_t gcomp_dict_corpus_build(const gcomp_allocator_t * alloc,
    const void * const * samples, const size_t * sizes, size_t n,
    gcomp_dict_corpus_t * out) {
  size_t total = 0;
  size_t used = 0;
  for (size_t i = 0; i < n; i++) {
    if (!samples[i] && sizes[i] > 0) {
      return GCOMP_ERR_INVALID_ARG;
    }
    if (sizes[i] == 0) {
      continue;
    }
    if (!gcu_safe_add_size(total, sizes[i], &total)) {
      return GCOMP_ERR_LIMIT;
    }
    used++;
  }
  if (total == 0 || used == 0) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  out->data = gcomp_malloc(alloc, total);
  out->ends = gcomp_malloc(alloc, used * sizeof(size_t));
  if (!out->data || !out->ends) {
    gcomp_dict_corpus_free(alloc, out);
    return GCOMP_ERR_MEMORY;
  }

  size_t at = 0;
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    if (sizes[i] == 0) {
      continue;
    }
    memcpy(out->data + at, samples[i], sizes[i]);
    at += sizes[i];
    out->ends[j++] = at;
  }
  out->size = total;
  out->count = used;
  return GCOMP_OK;
}

/// The end of the sample that position @p at belongs to.
static size_t gcomp_dict_sample_end(
    const gcomp_dict_corpus_t * c, size_t at) {
  size_t lo = 0;
  size_t hi = c->count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    if (at < c->ends[mid]) {
      hi = mid;
    }
    else {
      lo = mid + 1u;
    }
  }
  return (lo < c->count) ? c->ends[lo] : c->size;
}

gcomp_status_t gcomp_dict_train(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * const * samples, const size_t * sample_sizes, size_t n_samples,
    void * dict_out, size_t dict_capacity, size_t * dict_size_out) {
  if (!method_name || !samples || !sample_sizes || n_samples == 0 ||
      !dict_out || dict_capacity == 0 || !dict_size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *dict_size_out = 0;

  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }
  // The content is the same whichever method will use it; the name is checked
  // so that a typo is refused rather than silently producing a dictionary for
  // a method that does not exist.
  if (!gcomp_registry_find(registry, method_name)) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  unsigned k = GCOMP_DICT_DEFAULT_SEGMENT;
  unsigned d = GCOMP_DICT_DEFAULT_DMER;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "dict.segment_size", &v) ==
            GCOMP_OK &&
        v >= 16u && v <= 4096u) {
      k = (unsigned)v;
    }
    if (gcomp_options_get_uint64(options, "dict.dmer_size", &v) == GCOMP_OK &&
        v >= 6u && v <= 8u) {
      d = (unsigned)v;
    }
  }

  gcomp_dict_corpus_t corpus;
  memset(&corpus, 0, sizeof(corpus));
  gcomp_status_t status = gcomp_dict_corpus_build(
      alloc, samples, sample_sizes, n_samples, &corpus);
  if (status != GCOMP_OK) {
    return status;
  }
  // Eight bytes are read at every d-mer position, so the last few positions
  // have to be left alone whatever d is.
  if (corpus.size < (size_t)k + 8u) {
    gcomp_dict_corpus_free(alloc, &corpus);
    return GCOMP_ERR_UNSUPPORTED;
  }

  // A segment must fit inside one sample, so a segment size larger than the
  // samples picks nothing at all - and the samples a dictionary is *for* are
  // short. 4000 log lines of about 170 bytes each got GCOMP_ERR_UNSUPPORTED
  // from the 256-byte default, which is the one case this function exists to
  // serve.
  //
  // So k is clamped to the median sample length rather than the longest: the
  // median guarantees that at least half the samples can contribute a segment,
  // where the longest would let a single large sample set a size no other
  // sample could meet, and the dictionary would then be built from that one
  // sample alone.
  {
    size_t * lengths = gcomp_malloc(alloc, corpus.count * sizeof(size_t));
    if (!lengths) {
      gcomp_dict_corpus_free(alloc, &corpus);
      return GCOMP_ERR_MEMORY;
    }
    size_t prev = 0;
    for (size_t i = 0; i < corpus.count; i++) {
      lengths[i] = corpus.ends[i] - prev;
      prev = corpus.ends[i];
    }
    // Insertion sort: the corpus is sample counts, not bytes, and this runs
    // once.
    for (size_t i = 1; i < corpus.count; i++) {
      const size_t v = lengths[i];
      size_t j = i;
      while (j > 0 && lengths[j - 1u] > v) {
        lengths[j] = lengths[j - 1u];
        j--;
      }
      lengths[j] = v;
    }
    const size_t median = lengths[corpus.count / 2u];
    gcomp_free(alloc, lengths);

    if ((size_t)k > median) {
      k = (median > 16u) ? (unsigned)median : 16u;
    }
    if (corpus.size < (size_t)k + 8u) {
      gcomp_dict_corpus_free(alloc, &corpus);
      return GCOMP_ERR_UNSUPPORTED;
    }
  }

  uint32_t * freq = gcomp_calloc(alloc, GCOMP_DICT_FREQ_SIZE, sizeof(uint32_t));
  uint16_t * window =
      gcomp_calloc(alloc, GCOMP_DICT_FREQ_SIZE, sizeof(uint16_t));
  if (!freq || !window) {
    gcomp_free(alloc, freq);
    gcomp_free(alloc, window);
    gcomp_dict_corpus_free(alloc, &corpus);
    return GCOMP_ERR_MEMORY;
  }

  const size_t last = corpus.size - 8u; // Last position a d-mer may start at.
  for (size_t i = 0; i <= last; i++) {
    const uint32_t h = gcomp_dict_hash(corpus.data + i, d);
    if (freq[h] < 0xFFFFFFFFu) {
      freq[h]++;
    }
  }

  // Chosen segments, most valuable first; written out in reverse.
  size_t * picks = NULL;
  const size_t max_picks = (dict_capacity / k) + 1u;
  picks = gcomp_malloc(alloc, max_picks * sizeof(size_t));
  if (!picks) {
    gcomp_free(alloc, freq);
    gcomp_free(alloc, window);
    gcomp_dict_corpus_free(alloc, &corpus);
    return GCOMP_ERR_MEMORY;
  }

  size_t n_picks = 0;
  size_t filled = 0;

  while (filled + k <= dict_capacity && n_picks < max_picks) {
    uint64_t best_score = 0;
    size_t best_at = corpus.size;

    // One pass over every position a segment may start at. The score is
    // maintained incrementally: a d-mer entering the window adds its frequency
    // only if it was not already in the window, and one leaving subtracts only
    // when the last copy goes.
    size_t i = 0;
    while (i <= last) {
      const size_t sample_end = gcomp_dict_sample_end(&corpus, i);
      // A segment has to fit inside one sample, and its last d-mer has to fit
      // inside the corpus.
      size_t limit = sample_end;
      if (limit > corpus.size - 8u + 1u) {
        limit = corpus.size - 8u + 1u;
      }
      if (i + k > limit) {
        i = sample_end; // This sample has no room for a whole segment.
        continue;
      }

      uint64_t score = 0;
      size_t w_start = i;
      for (size_t j = i; j < i + k; j++) {
        const uint32_t h = gcomp_dict_hash(corpus.data + j, d);
        if (window[h]++ == 0) {
          score += freq[h];
        }
      }
      if (score > best_score) {
        best_score = score;
        best_at = i;
      }

      // Slide to the end of this sample.
      for (size_t start = i + 1u; start + k <= limit; start++) {
        const uint32_t out_h = gcomp_dict_hash(corpus.data + start - 1u, d);
        if (--window[out_h] == 0) {
          score -= freq[out_h];
        }
        const uint32_t in_h = gcomp_dict_hash(corpus.data + start + k - 1u, d);
        if (window[in_h]++ == 0) {
          score += freq[in_h];
        }
        if (score > best_score) {
          best_score = score;
          best_at = start;
        }
        w_start = start;
      }
      // Clear the window for the next sample rather than memset the whole
      // table: the window only ever holds k entries.
      for (size_t j = w_start; j < w_start + k; j++) {
        window[gcomp_dict_hash(corpus.data + j, d)] = 0;
      }
      i = sample_end;
    }

    if (best_at >= corpus.size || best_score == 0) {
      break; // Nothing left worth taking.
    }

    picks[n_picks++] = best_at;
    filled += k;

    // Everything this segment covers is now covered, so it must not pull the
    // next pick back to the same neighbourhood.
    for (size_t j = best_at; j < best_at + k; j++) {
      freq[gcomp_dict_hash(corpus.data + j, d)] = 0;
    }
  }

  uint8_t * out = (uint8_t *)dict_out;
  size_t written = 0;
  // Reverse order: the best segment ends up nearest the data that will match
  // against it, where a reference to it is cheapest.
  for (size_t i = n_picks; i > 0; i--) {
    const size_t at = picks[i - 1u];
    memcpy(out + written, corpus.data + at, k);
    written += k;
  }

  gcomp_free(alloc, picks);
  gcomp_free(alloc, freq);
  gcomp_free(alloc, window);
  gcomp_dict_corpus_free(alloc, &corpus);

  if (written == 0) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  *dict_size_out = written;
  return GCOMP_OK;
}
