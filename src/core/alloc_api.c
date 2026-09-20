/**
 * @file alloc_api.c
 *
 * gcomp_encode_alloc() and gcomp_decode_alloc(): the buffer API without
 * having to know the size first.
 *
 * ## Why decoding needs this and encoding does not, quite
 *
 * gcomp_encode_bound() answers the encoder's question outright, so
 * gcomp_encode_alloc() is a short function: take the bound, allocate it,
 * encode, hand back a buffer trimmed to what was written.
 *
 * Decoding has no such answer.  Some formats state the decompressed size in
 * their header and some do not, and none of them can be trusted about it
 * without limits: the number comes from the stream.  So this takes the size
 * where it is offered, treats it as a hint rather than a promise, and
 * otherwise grows a buffer as the decoder fills it.
 *
 * ## Growth, and why it stays inside the limits
 *
 * Every enlargement is checked against `limits.max_output_bytes` before it is
 * made, so a decompression bomb is refused by the same ceiling that bounds the
 * decoders themselves rather than by running the machine out of memory first.
 * A caller who wants a different ceiling sets that option; a caller who sets it
 * to zero has asked for no ceiling and gets none.
 *
 * Doubling, starting from whichever is larger of 64 KB and four times the
 * input.  Four is a poor guess at a compression ratio and is meant to be: it
 * only decides how many reallocations a stream that beats it will cost.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "alloc_internal.h"
#include "registry_internal.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/cutil/safemath.h>
#include <stddef.h>
#include <stdint.h>

/// Smallest buffer worth starting a decode with.
#define GCOMP_ALLOC_MIN_CAPACITY (64u * 1024u)

void gcomp_buffer_free(gcomp_registry_t * registry, void * data) {
  if (!data) {
    return;
  }
  if (!registry) {
    registry = gcomp_registry_default();
  }
  const gcomp_allocator_t * alloc =
      registry ? gcomp_registry_get_allocator(registry) : NULL;
  gcomp_free(alloc, data);
}

gcomp_status_t gcomp_encode_alloc(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void ** data_out,
    size_t * size_out) {
  if (!method_name || !data_out || !size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *data_out = NULL;
  *size_out = 0;
  if (!input_data && input_size > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  size_t bound = 0;
  gcomp_status_t s =
      gcomp_encode_bound(registry, method_name, options, input_size, &bound);
  if (s != GCOMP_OK) {
    return s;
  }
  // gcomp_encode_buffer() refuses a zero capacity, and a format that writes
  // nothing for an empty input would otherwise land there.
  size_t capacity = bound ? bound : 1u;

  uint8_t * buf = gcomp_malloc(alloc, capacity);
  if (!buf) {
    return GCOMP_ERR_MEMORY;
  }

  size_t written = 0;
  s = gcomp_encode_buffer(registry, method_name, options, input_data,
      input_size, buf, capacity, &written);
  if (s != GCOMP_OK) {
    gcomp_free(alloc, buf);
    return s;
  }

  // Give back the slack.  A bound is a worst case, and on data that compresses
  // it is most of the allocation.  A realloc that declines to shrink is not a
  // failure - the buffer is still correct - so its answer is only taken when
  // it gives one.
  if (written < capacity) {
    uint8_t * shrunk = gcomp_realloc(alloc, buf, written ? written : 1u);
    if (shrunk) {
      buf = shrunk;
    }
  }

  *data_out = buf;
  *size_out = written;
  return GCOMP_OK;
}

gcomp_status_t gcomp_decode_alloc(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void ** data_out,
    size_t * size_out) {
  if (!method_name || !data_out || !size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *data_out = NULL;
  *size_out = 0;
  if (!input_data && input_size > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  const uint64_t max_output =
      gcomp_limits_read_output_max(options, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);

  // A starting size.  If the header states a content size, use it: it is the
  // right answer when it is honest, and when it is not, the growth loop and
  // the output limit below both still apply, so believing it costs nothing
  // more than one wrong allocation.
  size_t capacity = GCOMP_ALLOC_MIN_CAPACITY;
  int have_exact_size = 0;
  uint64_t exact_size = 0;
  {
    size_t four_x = 0;
    if (gcu_safe_mul_size(input_size, 4u, &four_x) && four_x > capacity) {
      capacity = four_x;
    }
    gcomp_stream_info_t info;
    if (gcomp_peek(registry, method_name, options, input_data, input_size,
            &info, NULL) == GCOMP_OK &&
        info.has_content_size &&
        (max_output == 0 || info.content_size <= max_output) &&
        info.content_size <= (uint64_t)(size_t)-1) {
      have_exact_size = 1;
      exact_size = info.content_size;
      if (info.content_size > 0) {
        capacity = (size_t)info.content_size;
      }
    }
  }
  if (max_output != 0 && (uint64_t)capacity > max_output) {
    capacity = (size_t)max_output;
  }
  if (capacity == 0) {
    capacity = 1u;
  }

  // When the frame states its size, decode against that size rather than
  // against the expansion ratio.
  //
  // The ratio exists to bound output for a stream that does not say how large
  // it is, and it does that by guessing: at its 1000:1 default it refuses any
  // stream that compresses harder than that, which ordinary data does.  A
  // 200 KB run of structured text goes to 89 bytes here - 2247:1 - and is
  // refused, which is COMPRESS-TODO section 1 and is what `image` works around
  // by hand.
  //
  // An exact ceiling is strictly the safer of the two, not a relaxation: the
  // output cannot exceed the declared size, the declared size has already been
  // checked against the caller's own max_output_bytes above, and a frame that
  // lies low is caught when the decoder overruns it.  What is given up is the
  // ratio's guess, which had nothing to add once the real number is known.
  //
  // The caller's options are not modified; a copy carries the change.
  gcomp_options_t * decode_opts = NULL;
  if (have_exact_size) {
    gcomp_status_t co = options
        ? gcomp_options_clone(options, &decode_opts)
        : gcomp_options_create(&decode_opts);
    if (co != GCOMP_OK) {
      return co;
    }
    if (gcomp_options_set_uint64(
            decode_opts, "limits.max_output_bytes", exact_size) != GCOMP_OK ||
        gcomp_options_set_uint64(
            decode_opts, "limits.max_expansion_ratio", 0) != GCOMP_OK) {
      // Frozen options, or an allocation failure.  Fall back to the caller's
      // own, which is correct if less forgiving.
      gcomp_options_destroy(decode_opts);
      decode_opts = NULL;
    }
  }

  gcomp_decoder_t * dec = NULL;
  gcomp_status_t s = gcomp_decoder_create(
      registry, method_name, decode_opts ? decode_opts : options, &dec);
  if (s != GCOMP_OK) {
    gcomp_options_destroy(decode_opts);
    return s;
  }

  uint8_t * buf = gcomp_malloc(alloc, capacity);
  if (!buf) {
    gcomp_decoder_destroy(dec);
    gcomp_options_destroy(decode_opts);
    return GCOMP_ERR_MEMORY;
  }

  gcomp_buffer_t in = {input_data, input_size, 0};
  size_t produced = 0;
  int finishing = 0;

  for (;;) {
    int at_ceiling = 0;
    if (produced == capacity) {
      // Full.  Grow if there is room to grow into, so that the next call is
      // not made with nowhere to put its answer.
      size_t next = 0;
      if ((max_output != 0 && (uint64_t)capacity >= max_output) ||
          !gcu_safe_mul_size(capacity, 2u, &next)) {
        at_ceiling = 1;
      }
      else {
        if (max_output != 0 && (uint64_t)next > max_output) {
          next = (size_t)max_output;
        }
        uint8_t * grown = gcomp_realloc(alloc, buf, next);
        if (!grown) {
          s = GCOMP_ERR_MEMORY;
          break;
        }
        buf = grown;
        capacity = next;
      }
      // Being at the ceiling with a full buffer is not itself a failure: a
      // stream whose output is exactly the ceiling decodes correctly and ends
      // there.  Deciding otherwise here would refuse it - which it did, and
      // which examples/detect_and_decode.c ran into on the first file it was
      // pointed at, because a caller that knows the content size sets the
      // ceiling to exactly that.  So carry on and let the decoder say: with no
      // room, update() makes no progress, which is the documented signal to
      // finish, and finish() answers GCOMP_OK for a complete stream or
      // GCOMP_ERR_LIMIT when it really does have more to give.
    }

    gcomp_buffer_t out = {buf, capacity, produced};
    size_t before_in = in.used;
    size_t before_out = produced;

    if (!finishing) {
      s = gcomp_decoder_update(dec, &in, &out);
      if (s != GCOMP_OK) {
        break;
      }
      produced = out.used;
      // The documented stop condition (stream.h): a call that neither consumes
      // input nor produces output has nothing left to do with what it holds.
      // Not `in.used == in.size`, because the decoder can still be holding
      // bits it has read and not yet turned into bytes.
      if (in.used == before_in && produced == before_out) {
        finishing = 1;
      }
      continue;
    }

    s = gcomp_decoder_finish(dec, &out);
    produced = out.used;
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT) {
      break;
    }
    // GCOMP_ERR_LIMIT from finish means more output is staged than fits.  If
    // the buffer did not fill, the limit came from somewhere else - an output
    // ceiling inside the decoder - and growing would not help.
    if (produced < capacity) {
      break;
    }
    if (at_ceiling) {
      break; // Genuinely more output than the caller allowed.
    }
    s = GCOMP_OK; // Grow on the next turn of the loop and ask again.
  }

  if (s != GCOMP_OK) {
    gcomp_free(alloc, buf);
    gcomp_decoder_destroy(dec);
    gcomp_options_destroy(decode_opts);
    return s;
  }
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(decode_opts);

  if (produced < capacity) {
    uint8_t * shrunk = gcomp_realloc(alloc, buf, produced ? produced : 1u);
    if (shrunk) {
      buf = shrunk;
    }
  }

  *data_out = buf;
  *size_out = produced;
  return GCOMP_OK;
}
