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
 *
 * Copyright 2026 by Corey Pennycuff
 */

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

  gcomp_status_t s = rle_profile_encode(state, in_ptr + input->used,
      input_avail, &input_consumed, out_ptr, output->size, &output->used);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, s, "RLE profile encode failed");
  }

  input->used += input_consumed;
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
  return GCOMP_OK;
}

gcomp_status_t rle_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  rle_encoder_state_t * state = (rle_encoder_state_t *)encoder->method_state;
  state->literal_count = 0;
  state->run_len = 0;
  return GCOMP_OK;
}
