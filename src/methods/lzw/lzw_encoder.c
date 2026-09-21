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
 * @file lzw_encoder.c
 *
 * LZW encoder: streaming update/finish/reset. Builds dictionary during
 * encoding; emits codes via profile bit I/O; respects CLEAR/EOI and
 * code-width growth rules.
 *
 * DESIGN NOTES
 * ============
 *
 * This encoder is *profile-driven* and intentionally does not implement a
 * container. The caller must choose the same `lzw.format` on encode and decode.
 *
 * - **Dictionary model:** Classic LZW with a base dictionary of 0..255 literal
 *   codes, followed by newly learned sequences.
 * - **CLEAR / EOI:** The encoder emits CLEAR at the beginning of a stream and
 *   EOI on finish. CLEAR is also emitted when the dictionary becomes full (at
 *   `2^max_code_bits` entries).
 * - **Code widths:** Start at 9 bits and grow up to `lzw.max_code_bits`
 *   (default 12). When the width increments is dictated by the profile:
 *   GIF and TIFF increment at different table thresholds.
 * - **Streaming + bit packing:** `update()` and `finish()` are allowed to be
 *   called with arbitrarily sized output buffers. The bit writer may retain a
 *   partially filled byte between calls; `lzw_bitwriter_set_buffer()` switches
 *   the output window without clearing pending bits.
 *
 * PERF NOTE
 * =========
 *
 * The current dictionary lookup uses a linear search (sufficient for unit
 * tests and correctness). If performance becomes an issue, replace
 * `lzw_core_encoder_find()` with a hashed lookup structure while keeping the
 * core prefix/append table layout intact.
 */

#include <ghoti.io/compress/macros.h>
#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/strutil_internal.h"
#include "lzw_internal.h"
#include <ghoti.io/compress/limits.h>
#include <limits.h>
#include <string.h>

#define LZW_FORMAT_DEFAULT LZW_FORMAT_GIF
// The width of a LITERAL code, not the width the stream opens at.  TIFF 6.0
// section 13 fixes its literals at 8 bits and its first code at 9; this is the
// 8.  It read 9 while the profile layer ignored the value and returned 9 for
// the opening width regardless, so the two errors cancelled and TIFF worked.
// Now that the profile derives from this, 9 here would open TIFF at 10 bits
// with CLEAR at 512.
#define LZW_LIT_WIDTH_GIF 8
#define LZW_LIT_WIDTH_TIFF 8
#define LZW_MAX_CODE_BITS_DEFAULT 12
#define LZW_NO_PREFIX UINT32_MAX

gcomp_status_t lzw_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!registry || !encoder) {
    if (encoder) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  lzw_encoder_state_t * state =
      gcomp_calloc(alloc, 1, sizeof(lzw_encoder_state_t));
  if (!state) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "LZW encoder allocation failed");
  }

  state->allocator = alloc;

  const char * format_str = NULL;
  if (options &&
      gcomp_options_get_string(options, "lzw.format", &format_str) ==
          GCOMP_OK &&
      format_str) {
    gcomp_copy_cstr(state->format, sizeof(state->format), format_str);
  }
  else {
    gcomp_copy_cstr(state->format, sizeof(state->format), LZW_FORMAT_DEFAULT);
  }

  state->profile_id = lzw_profile_from_string(state->format);
  if (state->profile_id == LZW_PROFILE_UNKNOWN) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "LZW format must be gif or tiff");
  }

  uint64_t u64 = 0;
  if (options &&
      gcomp_options_get_uint64(options, "lzw.lit_width", &u64) == GCOMP_OK &&
      u64 != 0) {
    state->lit_width = u64;
  }
  else {
    state->lit_width = (strcmp(state->format, LZW_FORMAT_TIFF) == 0)
        ? LZW_LIT_WIDTH_TIFF
        : LZW_LIT_WIDTH_GIF;
  }

  if (options &&
      gcomp_options_get_uint64(options, "lzw.max_code_bits", &u64) ==
          GCOMP_OK) {
    state->max_code_bits = u64;
  }
  else {
    state->max_code_bits = LZW_MAX_CODE_BITS_DEFAULT;
  }

  // lzw.encoder_lookup declares "hash" as its default in lzw_register.c, but
  // this only ever switched the hash on when a caller passed the option
  // explicitly.  Encoding with no options at all -- which is what
  // gcomp_encode_buffer(..., NULL, ...) does, and what every caller in the
  // suite does -- therefore ran lzw_core_encoder_find(), a linear scan over
  // the whole code table for every input byte.  That scan was 98.9% of the
  // encoder's instructions and held it under 3 MB/s.  Both paths produce
  // identical bytes, so the only visible symptom was the speed.  The default
  // now matches the one that is advertised; only an explicit "linear" turns
  // it off.
  state->use_hash = 1;
  state->hash_table = NULL;
  state->hash_table_bytes = 0;
  if (options) {
    const char * lookup_str = NULL;
    if (gcomp_options_get_string(options, "lzw.encoder_lookup", &lookup_str) ==
            GCOMP_OK &&
        lookup_str && strcmp(lookup_str, "linear") == 0) {
      state->use_hash = 0;
    }
  }

  if (options) {
    u64 = 0;
    if (gcomp_options_get_uint64(options, "limits.max_output_bytes", &u64) ==
        GCOMP_OK) {
      state->max_output_bytes = u64;
    }
    if (gcomp_options_get_uint64(options, "limits.max_memory_bytes", &u64) ==
        GCOMP_OK) {
      state->max_memory_bytes = u64;
    }
    if (gcomp_options_get_uint64(options, "limits.max_expansion_ratio", &u64) ==
        GCOMP_OK) {
      state->max_expansion_ratio = u64;
    }
  }

  // Eight is the ceiling because a literal decodes to one byte: the code
  // table's append_char is a uint8_t, so a ninth bit of literal has nowhere
  // to go.  Two is the floor because GIF says so (89a 22).  The core checks
  // max_code_bits; nothing checked this, because until the profile began
  // reading lit_width an out-of-range value changed nothing.
  unsigned lit = (unsigned)state->lit_width;
  unsigned enc_max_bits = (unsigned)state->max_code_bits;
  if (enc_max_bits > LZW_CORE_MAX_CODE_BITS) {
    enc_max_bits = LZW_CORE_MAX_CODE_BITS;
  }
  unsigned lit_ceiling = enc_max_bits - 1u < 8u ? enc_max_bits - 1u : 8u;
  if (lit < 2u || lit > lit_ceiling) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "LZW lit_width must be 2..%u", lit_ceiling);
  }
  state->clear_code = lzw_profile_clear_code(state->profile_id, lit);
  state->eoi_code = lzw_profile_eoi_code(state->profile_id, lit);
  state->current_bits = lzw_profile_initial_code_bits(state->profile_id, lit);
  state->prefix_code = LZW_NO_PREFIX;
  state->pending_code = 0;
  state->pending_bits = 0;
  state->header_emitted = 0;

  state->mem_tracker.current_bytes = 0;
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(lzw_encoder_state_t));

  gcomp_status_t s = lzw_core_encoder_init(&state->core, alloc,
      (unsigned)state->max_code_bits, state->clear_code, state->eoi_code);
  if (s != GCOMP_OK) {
    gcomp_memory_track_free(&state->mem_tracker, sizeof(lzw_encoder_state_t));
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(encoder, s, "LZW encoder core init failed");
  }

  {
    uint32_t cap = state->core.capacity;
    size_t prefix_bytes = (size_t)cap * sizeof(uint16_t);
    size_t append_bytes = (size_t)cap * sizeof(uint8_t);
    gcomp_memory_track_alloc(&state->mem_tracker, prefix_bytes);
    gcomp_memory_track_alloc(&state->mem_tracker, append_bytes);
  }

  if (state->max_memory_bytes != 0) {
    gcomp_status_t lim =
        gcomp_memory_check_limit(&state->mem_tracker, state->max_memory_bytes);
    if (lim != GCOMP_OK) {
      // Read out of `state` before the teardown below frees it; see the same
      // note in lzw_decoder.c.
      unsigned long long used =
          (unsigned long long)state->mem_tracker.current_bytes;
      unsigned long long limit = (unsigned long long)state->max_memory_bytes;
      uint32_t cap = state->core.capacity;
      gcomp_memory_track_free(
          &state->mem_tracker, (size_t)cap * sizeof(uint16_t));
      gcomp_memory_track_free(
          &state->mem_tracker, (size_t)cap * sizeof(uint8_t));
      lzw_core_encoder_destroy(&state->core);
      gcomp_memory_track_free(&state->mem_tracker, sizeof(lzw_encoder_state_t));
      gcomp_free(alloc, state);
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_LIMIT,
          "LZW encoder memory usage %llu exceeds limit %llu", used, limit);
    }
  }

  if (state->use_hash) {
    gcomp_status_t hash_status =
        lzw_encoder_hash_init(&state->hash_table, alloc, state->core.capacity,
            &state->mem_tracker, &state->hash_table_bytes);
    if (hash_status != GCOMP_OK) {
      uint32_t cap = state->core.capacity;
      gcomp_memory_track_free(
          &state->mem_tracker, (size_t)cap * sizeof(uint16_t));
      gcomp_memory_track_free(
          &state->mem_tracker, (size_t)cap * sizeof(uint8_t));
      lzw_core_encoder_destroy(&state->core);
      gcomp_memory_track_free(&state->mem_tracker, sizeof(lzw_encoder_state_t));
      gcomp_free(alloc, state);
      return gcomp_encoder_set_error(
          encoder, hash_status, "LZW encoder hash table init failed");
    }
    if (state->max_memory_bytes != 0) {
      gcomp_status_t lim = gcomp_memory_check_limit(
          &state->mem_tracker, state->max_memory_bytes);
      if (lim != GCOMP_OK) {
        // Read out of `state` before the teardown below frees it; see the same
        // note in lzw_decoder.c.
        unsigned long long used =
            (unsigned long long)state->mem_tracker.current_bytes;
        unsigned long long limit = (unsigned long long)state->max_memory_bytes;
        lzw_encoder_hash_destroy(&state->hash_table, alloc, &state->mem_tracker,
            state->hash_table_bytes);
        state->hash_table_bytes = 0;
        uint32_t cap = state->core.capacity;
        gcomp_memory_track_free(
            &state->mem_tracker, (size_t)cap * sizeof(uint16_t));
        gcomp_memory_track_free(
            &state->mem_tracker, (size_t)cap * sizeof(uint8_t));
        lzw_core_encoder_destroy(&state->core);
        gcomp_memory_track_free(
            &state->mem_tracker, sizeof(lzw_encoder_state_t));
        gcomp_free(alloc, state);
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_LIMIT,
            "LZW encoder memory usage %llu exceeds limit %llu", used, limit);
      }
    }
  }

  lzw_bitwriter_init(
      &state->writer, NULL, 0, lzw_profile_bit_order(state->profile_id));

  encoder->method_state = state;
  return GCOMP_OK;
}

void lzw_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;
  if (state->hash_table) {
    lzw_encoder_hash_destroy(&state->hash_table, state->allocator,
        &state->mem_tracker, state->hash_table_bytes);
    state->hash_table_bytes = 0;
  }
  if (state->stage_buf) {
    gcomp_free(state->allocator, state->stage_buf);
    state->stage_buf = NULL;
    state->stage_size = 0;
  }
  uint32_t cap = state->core.capacity;
  gcomp_memory_track_free(&state->mem_tracker, (size_t)cap * sizeof(uint16_t));
  gcomp_memory_track_free(&state->mem_tracker, (size_t)cap * sizeof(uint8_t));
  lzw_core_encoder_destroy(&state->core);
  gcomp_memory_track_free(&state->mem_tracker, sizeof(lzw_encoder_state_t));
  const gcomp_allocator_t * alloc = state->allocator;
  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

/**
 * Emit one code.
 *
 * On GCOMP_ERR_LIMIT the code is stashed in pending_code/pending_bits and the
 * bit writer rolls itself back, so the next update() call replays it into a
 * fresh output window. That makes a full output buffer an ordinary pause
 * rather than an error, so no encoder error is recorded here.
 */
static gcomp_status_t emit_code(
    lzw_encoder_state_t * state, gcomp_encoder_t * encoder, uint32_t code) {
  (void)encoder;
  gcomp_status_t s =
      lzw_bitwriter_write_bits(&state->writer, code, state->current_bits);
  if (s == GCOMP_ERR_LIMIT) {
    state->pending_code = code;
    state->pending_bits = state->current_bits;
  }
  return s;
}

#define LZW_STAGE_SIZE 8192u
/* Headroom kept free so a single input step - which emits at most a code plus
 * a CLEAR - always fits without the bit writer hitting its limit mid-step. */
#define LZW_STAGE_HEADROOM 16u

/** Copy staged output to the caller, as far as it will fit. */
static int lzw_drain_stage(lzw_encoder_state_t * state, gcomp_buffer_t * output) {
  size_t avail = state->stage_used - state->stage_copied;
  if (avail) {
    size_t space = output->size - output->used;
    size_t n = (avail < space) ? avail : space;
    if (n) {
      memcpy((uint8_t *)output->data + output->used,
          state->stage_buf + state->stage_copied, n);
      output->used += n;
      state->stage_copied += n;
    }
  }
  if (state->stage_copied >= state->stage_used) {
    state->stage_used = 0;
    state->stage_copied = 0;
    return 1;
  }
  return 0;
}

/**
 * @brief Encode one batch of input into the staging buffer.
 *
 * Stops on a step boundary once the staging buffer is within
 * LZW_STAGE_HEADROOM of full, so a code is never half-emitted and the
 * dictionary and prefix always advance together with the bytes that describe
 * them.
 */
static gcomp_status_t lzw_encode_batch(
    lzw_encoder_state_t * state, gcomp_encoder_t * encoder, gcomp_buffer_t * input) {
  gcomp_status_t s;
  lzw_bitwriter_set_buffer(&state->writer, state->stage_buf, state->stage_size);

  /* Emit CLEAR at start of stream */
  if (!state->header_emitted) {
    s = emit_code(state, encoder, state->clear_code);
    if (s != GCOMP_OK) {
      return s;
    }
    state->header_emitted = 1;
  }

  const uint8_t * in_ptr = (const uint8_t *)input->data;

  while (input->used < input->size) {
    if (lzw_bitwriter_bytes_written(&state->writer) + LZW_STAGE_HEADROOM >
        state->stage_size) {
      break;
    }

    uint8_t byte = in_ptr[input->used];
    uint32_t prefix = state->prefix_code;
    uint32_t code_out = 0;
    int found = 0;

    if (prefix == LZW_NO_PREFIX) {
      /* First byte: current string is just this literal */
      state->prefix_code = (uint32_t)byte;
      input->used++;
      continue;
    }

    if (state->use_hash && state->hash_table) {
      found = lzw_encoder_hash_find(
          state->hash_table, &state->core, prefix, byte, &code_out);
    }
    else {
      found = lzw_core_encoder_find(&state->core, prefix, byte, &code_out);
    }
    if (found) {
      state->prefix_code = code_out;
      input->used++;
      continue;
    }

    /* Not in table: emit prefix, add (prefix, byte), new prefix = byte */
    s = emit_code(state, encoder, prefix);
    if (s != GCOMP_OK) {
      return s;
    }

    if (lzw_core_encoder_is_full(&state->core)) {
      s = emit_code(state, encoder, state->clear_code);
      if (s != GCOMP_OK) {
        return s;
      }
      lzw_core_encoder_reset(&state->core, state->clear_code, state->eoi_code);
      if (state->hash_table) {
        lzw_encoder_hash_reset(state->hash_table);
      }
      state->current_bits = lzw_profile_initial_code_bits(
          state->profile_id, (unsigned)state->lit_width);
    }
    else {
      uint32_t new_code = lzw_core_encoder_add(&state->core, prefix, byte);
      if (state->hash_table && new_code != 0) {
        lzw_encoder_hash_insert(state->hash_table, prefix, byte, new_code);
      }
      // should_increment_bits() asks whether the highest code that must still
      // be representable fits in current_bits.  For an encoder that is
      // next_code - 1: the largest code it can ever emit is the last slot it
      // assigned, never next_code itself.  Passing next_code widened every
      // code one entry too early, which no conforming decoder expects - such
      // a stream is unreadable by other GIF/TIFF implementations even though
      // this library's own decoder, widening on the same schedule, agreed
      // with it.
      if (lzw_profile_should_increment_bits(state->profile_id,
              state->core.next_code - 1u, state->current_bits)) {
        if (state->current_bits < (unsigned)state->max_code_bits) {
          state->current_bits++;
        }
      }
    }

    state->prefix_code = (uint32_t)byte;
    input->used++;
  }

  state->stage_used = lzw_bitwriter_bytes_written(&state->writer);
  return GCOMP_OK;
}

gcomp_status_t lzw_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input->size > 0 && input->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "input->data is NULL but size > 0");
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;

  if (!state->stage_buf) {
    state->stage_buf = gcomp_malloc(state->allocator, LZW_STAGE_SIZE);
    if (!state->stage_buf) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_MEMORY, "LZW encoder staging buffer allocation failed");
    }
    state->stage_size = LZW_STAGE_SIZE;
    state->stage_used = 0;
    state->stage_copied = 0;
  }

  for (;;) {
    if (!lzw_drain_stage(state, output)) {
      return GCOMP_OK;
    }
    if (input->used >= input->size || output->used >= output->size) {
      return GCOMP_OK;
    }

    size_t before = input->used;
    gcomp_status_t s = lzw_encode_batch(state, encoder, input);
    if (s != GCOMP_OK) {
      (void)lzw_drain_stage(state, output);
      return s;
    }
    if (input->used == before && state->stage_used == 0) {
      return GCOMP_OK;
    }
  }
}

gcomp_status_t lzw_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;

  // Anything update() staged but could not deliver must go out first, ahead of
  // the final code and EOI. Report GCOMP_ERR_LIMIT until it has all been
  // handed over; gcomp_encoder_finish() reserves GCOMP_OK for a complete
  // stream, so returning it here would silently truncate.
  if (state->stage_buf && state->stage_used > state->stage_copied) {
    if (!lzw_drain_stage(state, output)) {
      return GCOMP_ERR_LIMIT;
    }
  }

  // The tail is rendered into the staging buffer once and then drained, for
  // the same reason update() stages its output: written straight into the
  // caller's buffer it could not survive running out of room.  Every step
  // below mutates encoder state as it goes -- clearing the pending code,
  // setting header_emitted -- so a second pass over it does not repeat the
  // first, it writes a different and wrong tail.  See finish_staged.
  if (!state->finish_staged) {
    if (!state->stage_buf) {
      state->stage_buf = gcomp_malloc(state->allocator, LZW_STAGE_SIZE);
      if (!state->stage_buf) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
            "failed to allocate LZW staging buffer (%u bytes)",
            (unsigned)LZW_STAGE_SIZE);
      }
      state->stage_size = LZW_STAGE_SIZE;
    }
    state->stage_used = 0;
    state->stage_copied = 0;
    lzw_bitwriter_set_buffer(
        &state->writer, state->stage_buf, state->stage_size);

    /* Retry any pending code */
    if (state->pending_bits != 0) {
      gcomp_status_t s = lzw_bitwriter_write_bits(
          &state->writer, state->pending_code, state->pending_bits);
      if (s != GCOMP_OK) {
        return gcomp_encoder_set_error(
            encoder, s, "LZW encoder flush pending code failed");
      }
      state->pending_code = 0;
      state->pending_bits = 0;
    }

    /* Emit CLEAR if we never did (empty input) */
    if (!state->header_emitted) {
      gcomp_status_t s = emit_code(state, encoder, state->clear_code);
      if (s != GCOMP_OK) {
        return s;
      }
      state->header_emitted = 1;
    }

    /* Emit current string then EOI */
    if (state->prefix_code != LZW_NO_PREFIX) {
      gcomp_status_t s = emit_code(state, encoder, state->prefix_code);
      if (s != GCOMP_OK) {
        return s;
      }
      state->prefix_code = LZW_NO_PREFIX;
    }

    // The final code is emitted without adding a dictionary entry, because
    // there is no following byte to extend the string with.  A decoder does
    // not know that: reading that code it adds the entry it would have added
    // anyway, and then applies its own widening rule to the result.  So it may
    // widen once more than the loop above ever did, and read End_of_Information
    // at that wider width.
    //
    // The loop's check is the encoder's (the widest code it can still emit,
    // core.next_code - 1); this one is the decoder's (core.next_code), and it
    // has to be made here or the two disagree on the width of the last code in
    // the stream.  libtiff does the same thing in LZWPostEncode, which is why
    // its streams carry a wider End_of_Information than ours did at the
    // lengths where the final entry lands on a boundary.
    //
    // TIFF 6.0 section 13 makes this visible because of the early change: the
    // boundary is one entry lower than GIF's, so a TIFF stream hits it on
    // lengths a GIF stream does not.  The rule is the decoder's either way.
    if (lzw_profile_should_increment_bits(
            state->profile_id, state->core.next_code, state->current_bits)) {
      if (state->current_bits < (unsigned)state->max_code_bits) {
        state->current_bits++;
      }
    }

    gcomp_status_t s = emit_code(state, encoder, state->eoi_code);
    if (s != GCOMP_OK) {
      return s;
    }

    s = lzw_bitwriter_flush(&state->writer);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, s, "LZW encoder bit flush failed");
    }

    state->stage_used = lzw_bitwriter_bytes_written(&state->writer);
    state->finish_staged = 1;
  }

  if (!lzw_drain_stage(state, output)) {
    return GCOMP_ERR_LIMIT;
  }
  return GCOMP_OK;
}

/**
 * @brief Render the flush tail into the staging buffer.
 *
 * WHY THIS EMITS A CLEAR, AND WHY IT DOES NOT BYTE-ALIGN
 * =====================================================
 *
 * LZW is a bare bit stream.  Codes are 9 to 12 bits and do not stop on byte
 * boundaries, so at any moment the last code written is partly in the bit
 * writer and partly in the bytes already handed over.  A decoder given those
 * bytes cannot decode that last code, and so cannot produce the bytes it
 * stands for -- which is exactly what a flush has to deliver.
 *
 * The fix is to write one more code after it.  A code is at least 9 bits and
 * a partial byte is at most 7, so writing anything at all pushes the previous
 * code entirely into whole bytes.  The code to write is CLEAR: it is the one
 * code that means something harmless here, and the format already uses it
 * mid-stream when the dictionary fills, so every decoder handles it.
 *
 * That is also why LZW's flush is always a full flush.  CLEAR resets the
 * dictionary and the code width; there is no way to push the pending code out
 * without it, so there is no cheaper mode to offer.
 *
 * What this deliberately does NOT do is pad to a byte boundary.  Padding
 * would put bits into the stream that the decoder would read as the start of
 * the next code, and the encoder -- which resumes from its own bit position,
 * not from a byte boundary -- would then be writing a code the decoder is
 * already misreading.  So the partial byte stays in the writer and continues
 * into the next output window, which is what lzw_bitwriter_set_buffer()
 * preserves bit_buffer/bit_count for.  Callers who need byte-aligned
 * boundaries need a framed format; GIF and TIFF get theirs from sub-block
 * and strip lengths outside the LZW stream.
 */
static gcomp_status_t lzw_stage_flush_tail(
    lzw_encoder_state_t * state, gcomp_encoder_t * encoder) {
  state->stage_used = 0;
  state->stage_copied = 0;
  lzw_bitwriter_set_buffer(&state->writer, state->stage_buf, state->stage_size);

  // A code update() could not fit goes first, or it would be lost.
  if (state->pending_bits != 0) {
    gcomp_status_t s = lzw_bitwriter_write_bits(
        &state->writer, state->pending_code, state->pending_bits);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, s, "LZW encoder flush pending code failed");
    }
    state->pending_code = 0;
    state->pending_bits = 0;
  }

  // A stream that has emitted nothing still owes its opening CLEAR.
  if (!state->header_emitted) {
    gcomp_status_t s = emit_code(state, encoder, state->clear_code);
    if (s != GCOMP_OK) {
      return s;
    }
    state->header_emitted = 1;
  }

  // The string being built is input already consumed; it has to go out.
  if (state->prefix_code != LZW_NO_PREFIX) {
    gcomp_status_t s = emit_code(state, encoder, state->prefix_code);
    if (s != GCOMP_OK) {
      return s;
    }
    state->prefix_code = LZW_NO_PREFIX;
  }

  // The CLEAR that makes the codes above readable -- see above.
  gcomp_status_t s = emit_code(state, encoder, state->clear_code);
  if (s != GCOMP_OK) {
    return s;
  }
  lzw_core_encoder_reset(&state->core, state->clear_code, state->eoi_code);
  if (state->hash_table) {
    lzw_encoder_hash_reset(state->hash_table);
  }
  state->current_bits = lzw_profile_initial_code_bits(
      state->profile_id, (unsigned)state->lit_width);

  // Whole bytes only.  The partial byte stays in the writer and continues
  // into the next window; flushing it here is the one thing that would break
  // the stream.
  state->stage_used = lzw_bitwriter_bytes_written(&state->writer);
  return GCOMP_OK;
}

gcomp_status_t lzw_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  // Always a full flush; see lzw_stage_flush_tail().
  (void)mode;

  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;
  if (state->finish_staged) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "LZW encoder cannot flush after finish");
  }

  if (!state->stage_buf) {
    state->stage_buf = gcomp_malloc(state->allocator, LZW_STAGE_SIZE);
    if (!state->stage_buf) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
          "LZW encoder staging buffer allocation failed");
    }
    state->stage_size = LZW_STAGE_SIZE;
    state->stage_used = 0;
    state->stage_copied = 0;
  }

  // Anything update() staged but could not deliver goes first, ahead of the
  // flush tail, and the tail is rendered once.  flush_staged is what keeps a
  // second call from rendering a second tail -- the state it mutates is gone
  // by then, so it would write a different and wrong one.
  if (!state->flush_staged) {
    if (state->stage_used > state->stage_copied) {
      if (!lzw_drain_stage(state, output)) {
        return GCOMP_ERR_LIMIT;
      }
    }
    gcomp_status_t s = lzw_stage_flush_tail(state, encoder);
    if (s != GCOMP_OK) {
      return s;
    }
    state->flush_staged = 1;
  }

  if (!lzw_drain_stage(state, output)) {
    return GCOMP_ERR_LIMIT;
  }
  state->flush_staged = 0;
  return GCOMP_OK;
}

gcomp_status_t lzw_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;
  lzw_core_encoder_reset(&state->core, state->clear_code, state->eoi_code);
  if (state->hash_table) {
    lzw_encoder_hash_reset(state->hash_table);
  }
  state->current_bits = lzw_profile_initial_code_bits(
      state->profile_id, (unsigned)state->lit_width);
  state->prefix_code = LZW_NO_PREFIX;
  state->pending_code = 0;
  state->pending_bits = 0;
  state->header_emitted = 0;
  // Discard anything update() had staged but not yet delivered, and the tail
  // finish() may have rendered.
  state->stage_used = 0;
  state->stage_copied = 0;
  state->finish_staged = 0;
  state->flush_staged = 0;
  state->writer.bit_buffer = 0;
  state->writer.bit_count = 0;
  return GCOMP_OK;
}
