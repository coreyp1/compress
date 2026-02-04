/**
 * @file rle_decoder.c
 *
 * RLE decoder: streaming update/finish/reset. Profile-driven;
 * core logic in rle_core and rle_profile.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "rle_internal.h"
#include "rle_profile.h"
#include <ghoti.io/compress/limits.h>
#include <string.h>

#define RLE_FORMAT_DEFAULT RLE_FORMAT_PACKBITS

static void copy_format(char * dst, size_t dst_size, const char * src) {
  if (!dst || dst_size == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

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
    copy_format(state->format, sizeof(state->format), format_str);
  }
  else {
    copy_format(state->format, sizeof(state->format), RLE_FORMAT_DEFAULT);
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
  if (input_avail == 0 || output_avail == 0) {
    return GCOMP_OK;
  }

  const uint8_t * in_ptr = (const uint8_t *)input->data;
  uint8_t * out_ptr = (uint8_t *)output->data;
  size_t input_consumed = 0;

  gcomp_status_t s = rle_profile_decode(state, in_ptr + input->used,
      input_avail, &input_consumed, out_ptr, output->size, &output->used);
  if (s != GCOMP_OK) {
    return gcomp_decoder_set_error(decoder, s, "RLE profile decode failed");
  }

  input->used += input_consumed;
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

  (void)output;
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
