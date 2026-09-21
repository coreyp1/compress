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
 * @file rle_encoder.c
 *
 * RLE encoder: streaming update/finish/reset.
 *
 * Data flow: Raw input is passed to the selected profile (rle_profile_encode).
 * The profile accumulates literals and runs, emits tokens (control + data)
 * into the output buffer, and uses rle_core only indirectly (core is used by
 * the decoder for emitting decompressed bytes). Finish flushes any pending
 * literal or run token so the stream is complete. Reset clears literal_count
 * and run_len for reuse; buffers are retained.
 */

#include <ghoti.io/compress/macros.h>
#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/strutil_internal.h"
#include "rle_internal.h"
#include "rle_profile.h"
#include <string.h>

#define RLE_FORMAT_DEFAULT RLE_FORMAT_PACKBITS

gcomp_status_t rle_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!registry || !encoder) {
    if (encoder) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  rle_encoder_state_t * state =
      gcomp_calloc(alloc, 1, sizeof(rle_encoder_state_t));
  if (!state) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "RLE encoder allocation failed");
  }

  state->allocator = alloc;

  const char * format_str = NULL;
  if (options &&
      gcomp_options_get_string(options, "rle.format", &format_str) ==
          GCOMP_OK &&
      format_str) {
    gcomp_copy_cstr(state->format, sizeof(state->format), format_str);
  }
  else {
    gcomp_copy_cstr(state->format, sizeof(state->format), RLE_FORMAT_DEFAULT);
  }

  if (options) {
    uint64_t u64 = 0;
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

  encoder->method_state = state;
  return GCOMP_OK;
}

void rle_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  rle_encoder_state_t * state = (rle_encoder_state_t *)encoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;
  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

/** Length byte plus a full 128-byte PackBits literal block. */
#define RLE_MIN_OUTPUT_SPACE 129u

gcomp_status_t rle_encoder_update(gcomp_encoder_t * encoder,
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

  rle_encoder_state_t * state = (rle_encoder_state_t *)encoder->method_state;
  size_t input_avail = input->size - input->used;
  size_t output_avail = output->size - output->used;
  if (input_avail == 0 || output_avail == 0) {
    return GCOMP_OK;
  }

  const uint8_t * in_ptr = (const uint8_t *)input->data;
  uint8_t * out_ptr = (uint8_t *)output->data;
  size_t input_consumed = 0;
  size_t output_used_before = output->used;

  gcomp_status_t s = rle_profile_encode(state, in_ptr + input->used,
      input_avail, &input_consumed, out_ptr, output->size, &output->used);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, s, "RLE profile encode failed");
  }

  input->used += input_consumed;

  // A PackBits literal block is a length byte plus up to 128 bytes, so the
  // encoder needs 129 bytes of free output to flush one. Given less, it
  // cannot emit anything and used to return GCOMP_OK having consumed nothing
  // and produced nothing - a caller looping until its input was consumed
  // would spin forever with no indication why. Say so instead.
  if (input_consumed == 0 && output->used == output_used_before &&
      input->used < input->size) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_LIMIT,
        "RLE encoder needs at least %u bytes of free output space to make "
        "progress; %zu available",
        (unsigned)(RLE_MIN_OUTPUT_SPACE), output->size - output_used_before);
  }
  return GCOMP_OK;
}

gcomp_status_t rle_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  rle_encoder_state_t * state = (rle_encoder_state_t *)encoder->method_state;
  uint8_t * out_ptr = (uint8_t *)output->data;
  gcomp_status_t s =
      rle_profile_encode_finish(state, out_ptr, output->size, &output->used);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, s, "RLE profile encode finish failed");
  }
  state->finish_called = true;
  return GCOMP_OK;
}

gcomp_status_t rle_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  // Both modes are the same work.  RLE has no history to drop: every packet
  // is self-contained, so what follows a flush could never have referred to
  // what preceded it anyway.
  (void)mode;

  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  rle_encoder_state_t * state = (rle_encoder_state_t *)encoder->method_state;
  if (state->finish_called) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "RLE encoder cannot flush after finish");
  }

  // Closing out the pending run and literal block leaves exactly the state a
  // fresh encoder starts in, so encoding simply continues afterwards.  The
  // finish helper clears each piece as it emits it, so a short output buffer
  // resumes where it stopped rather than emitting anything twice.
  size_t before = output->used;
  gcomp_status_t s = rle_profile_encode_finish(
      state, (uint8_t *)output->data, output->size, &output->used);
  if (s == GCOMP_ERR_LIMIT) {
    // A PackBits literal packet is a length byte and up to 128 bytes, written
    // as a unit; there is no way to hand out half of one.  So unlike the
    // other methods, RLE cannot flush into an arbitrarily small buffer, and a
    // caller that keeps offering one would loop forever waiting for progress
    // that cannot come.  Say which it is: this is the same minimum
    // rle_encoder_update() reports for the same reason.
    if (output->used == before &&
        output->size - before < RLE_MIN_OUTPUT_SPACE) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_LIMIT,
          "RLE encoder needs at least %u bytes of free output space to flush; "
          "%zu available",
          (unsigned)RLE_MIN_OUTPUT_SPACE, output->size - before);
    }
    return GCOMP_ERR_LIMIT; // Caller drains and calls again.
  }
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, s, "RLE profile flush failed");
  }
  return GCOMP_OK;
}

gcomp_status_t rle_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  rle_encoder_state_t * state = (rle_encoder_state_t *)encoder->method_state;
  state->literal_count = 0;
  state->run_len = 0;
  state->finish_called = false;
  return GCOMP_OK;
}
