/**
 * @file rle_decoder.c
 *
 * RLE decoder: streaming update/finish/reset.
 *
 * Data flow: Encoded input is passed to the selected profile
 * (rle_profile_decode). The profile parses control/header bytes and literal/run
 * payloads, and calls rle_core (rle_emit_literal, rle_emit_repeat) to write
 * decompressed bytes with bounds and expansion-ratio checks. Finish is a no-op
 * for RLE (no end-of-stream token). Reset clears partial-token state and totals
 * for reuse.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/strutil_internal.h"
#include "rle_internal.h"
#include "rle_profile.h"
#include <ghoti.io/compress/limits.h>
#include <string.h>

#define RLE_FORMAT_DEFAULT RLE_FORMAT_PACKBITS

gcomp_status_t rle_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  rle_decoder_state_t * state =
      gcomp_calloc(alloc, 1, sizeof(rle_decoder_state_t));
  if (!state) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "RLE decoder allocation failed");
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

  decoder->method_state = state;
  return GCOMP_OK;
}

void rle_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return;
  }

  rle_decoder_state_t * state = (rle_decoder_state_t *)decoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;
  gcomp_free(alloc, state);
  decoder->method_state = NULL;
}

gcomp_status_t rle_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input->size > 0 && input->data == NULL) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "input->data is NULL but size > 0");
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  rle_decoder_state_t * state = (rle_decoder_state_t *)decoder->method_state;
  size_t input_avail = input->size - input->used;
  size_t output_avail = output->size - output->used;
  if (output_avail == 0) {
    return GCOMP_OK;
  }
  // An exhausted input does not mean there is nothing to do: a run whose
  // control and run bytes were both consumed while the output buffer was full
  // is still owed to the caller, and only the profile can write it.
  if (input_avail == 0 && state->partial.phase != RLE_DEC_RUN_EMIT) {
    return GCOMP_OK;
  }

  const uint8_t * in_ptr =
      input->data ? (const uint8_t *)input->data + input->used : NULL;
  uint8_t * out_ptr = (uint8_t *)output->data;
  size_t input_consumed = 0;

  gcomp_status_t s = rle_profile_decode(state, in_ptr, input_avail,
      &input_consumed, out_ptr, output->size, &output->used);
  // The profile reports what it consumed and produced whether or not it
  // failed, so the caller's buffers are advanced either way: bytes already
  // decoded are the caller's, and re-offering the input that produced them
  // would duplicate them.
  input->used += input_consumed;
  if (s != GCOMP_OK) {
    return gcomp_decoder_set_error(decoder, s, "RLE profile decode failed");
  }

  return GCOMP_OK;
}

gcomp_status_t rle_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  rle_decoder_state_t * state = (rle_decoder_state_t *)decoder->method_state;

  // RLE has no end-of-stream token, but finish is still not a no-op: the last
  // run in the stream can be left part-written when the output buffer filled,
  // and up to 128 bytes of it are owed to the caller.  gcomp_decoder_finish()
  // (stream.h) reserves GCOMP_OK for a complete stream and asks for
  // GCOMP_ERR_LIMIT while there is more to give and nowhere to put it, so that
  // a caller draining in a loop cannot stop early and silently lose the tail.
  if (state->partial.phase == RLE_DEC_RUN_EMIT &&
      state->partial.pending_count > 0) {
    size_t input_consumed = 0;
    gcomp_status_t s = rle_profile_decode(state, NULL, 0, &input_consumed,
        (uint8_t *)output->data, output->size, &output->used);
    if (s != GCOMP_OK) {
      return gcomp_decoder_set_error(decoder, s, "RLE profile decode failed");
    }
    if (state->partial.pending_count > 0) {
      return GCOMP_ERR_LIMIT;
    }
  }

  // A token the input never finished is a truncated stream, not a complete
  // one: the control byte promised literal bytes or a run byte that never
  // arrived.  GCOMP_ERR_CORRUPT is the code for "the input ended part way
  // through the stream" (stream.h).
  if (state->partial.phase == RLE_DEC_LITERAL_BYTES ||
      state->partial.phase == RLE_DEC_RUN_BYTE) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "RLE stream ends part way through a token: %u more %s expected",
        (unsigned)state->partial.pending_count,
        state->partial.phase == RLE_DEC_RUN_BYTE ? "run byte"
                                                 : "literal bytes");
  }

  return GCOMP_OK;
}

gcomp_status_t rle_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  rle_decoder_state_t * state = (rle_decoder_state_t *)decoder->method_state;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;
  state->partial.phase = RLE_DEC_CONTROL;
  state->partial.pending_count = 0;
  state->partial.pending_byte = 0;
  return GCOMP_OK;
}
