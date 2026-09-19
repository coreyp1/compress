/**
 * @file zlib_decoder.c
 *
 * RFC 1950 decoder: validate two header bytes, run deflate, check the
 * Adler-32.
 *
 * ## Finding the end of the deflate stream
 *
 * The container gives no length, so the only thing that says where the
 * deflate data stops is deflate itself, at its final block.  The inner
 * decoder stops consuming input there and leaves the trailer bytes untouched,
 * which is what lets this pick them up -- the same mechanism the gzip decoder
 * uses to find its own trailer.
 *
 * ## What is checked, and when
 *
 * The header check is cheap and total: CM must be 8, CINFO at most 7, and the
 * two bytes together a multiple of 31.  That last one is FCHECK's whole
 * purpose and it catches a mangled or misaligned stream before a single byte
 * is decompressed.
 *
 * The Adler-32 is checked at the end, against the decompressed output.  A
 * mismatch is reported as GCOMP_ERR_CORRUPT even though the bytes have
 * already been handed to the caller -- there is no way to un-hand them, and
 * saying nothing would be worse.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "zlib_internal.h"

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/wrapper_options.h"
#include <ghoti.io/compress/adler32.h>
#include <string.h>

static uint32_t zlib_read_be32(const uint8_t * data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
      ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

gcomp_status_t zlib_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  if (!decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  zlib_decoder_state_t * state =
      gcomp_calloc(alloc, 1, sizeof(zlib_decoder_state_t));
  if (!state) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "failed to allocate zlib decoder state");
  }
  state->allocator = alloc;
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(zlib_decoder_state_t));

  state->max_output_bytes = ZLIB_DEFAULT_MAX_OUTPUT_BYTES;
  state->max_expansion_ratio = ZLIB_DEFAULT_MAX_EXPANSION_RATIO;
  state->max_memory_bytes = ZLIB_DEFAULT_MAX_MEMORY_BYTES;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "limits.max_output_bytes", &v) ==
        GCOMP_OK) {
      state->max_output_bytes = v;
    }
    if (gcomp_options_get_uint64(options, "limits.max_expansion_ratio", &v) ==
        GCOMP_OK) {
      state->max_expansion_ratio = v;
    }
    if (gcomp_options_get_uint64(options, "limits.max_memory_bytes", &v) ==
        GCOMP_OK) {
      state->max_memory_bytes = v;
    }
  }

  gcomp_options_t * deflate_options = NULL;
  gcomp_status_t status = gcomp_clone_options_for_method(
      registry, "deflate", options, &deflate_options);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, state);
    return gcomp_decoder_set_error(
        decoder, status, "failed to prepare deflate options");
  }
  status = gcomp_decoder_create(
      registry, "deflate", deflate_options, &state->inner_decoder);
  if (deflate_options) {
    gcomp_options_destroy(deflate_options);
  }
  if (status != GCOMP_OK) {
    gcomp_free(alloc, state);
    return gcomp_decoder_set_error(
        decoder, status, "failed to create inner deflate decoder");
  }

  state->adler = GCOMP_ADLER32_INIT;
  state->stage = ZLIB_DEC_STAGE_HEADER;
  state->header_need = ZLIB_HEADER_SIZE;
  decoder->method_state = state;
  return GCOMP_OK;
}

void zlib_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return;
  }
  zlib_decoder_state_t * state = decoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;
  if (state->inner_decoder) {
    gcomp_decoder_destroy(state->inner_decoder);
  }
  gcomp_free(alloc, state);
  decoder->method_state = NULL;
}

/**
 * @brief Take header bytes from the input until the header is complete.
 *
 * @return GCOMP_OK when the header is parsed and the stage has advanced,
 *         GCOMP_ERR_LIMIT when more input is needed, or an error.
 */
static gcomp_status_t zlib_take_header(
    gcomp_decoder_t * decoder, zlib_decoder_state_t * state,
    gcomp_buffer_t * input) {
  const uint8_t * in = (const uint8_t *)input->data;

  while (state->header_accum_pos < state->header_need) {
    if (input->used >= input->size) {
      return GCOMP_ERR_LIMIT; // Come back with more.
    }
    state->header_accum[state->header_accum_pos++] = in[input->used++];
    state->total_input_bytes++;

    // The two fixed bytes are enough to know whether four more follow.
    if (state->header_accum_pos == ZLIB_HEADER_SIZE &&
        state->header_need == ZLIB_HEADER_SIZE) {
      gcomp_status_t status =
          zlib_parse_header(state->header_accum, &state->header);
      if (status != GCOMP_OK) {
        state->stage = ZLIB_DEC_STAGE_ERROR;
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "not a zlib stream: CMF=0x%02X FLG=0x%02X (needs CM=8, CINFO<=7, "
            "and (CMF*256+FLG) %% 31 == 0)",
            (unsigned)state->header_accum[0],
            (unsigned)state->header_accum[1]);
      }
      if (state->header.fdict) {
        state->header_need = ZLIB_HEADER_SIZE + ZLIB_DICTID_SIZE;
      }
    }
  }

  if (state->header.fdict) {
    state->header.dict_id =
        zlib_read_be32(state->header_accum + ZLIB_HEADER_SIZE);
    state->stage = ZLIB_DEC_STAGE_ERROR;
    // Decoding this needs the dictionary it names loaded into deflate's
    // window before the first block, and the deflate decoder has no way to be
    // given one.  Say which dictionary, so a caller can at least recognise
    // the stream; do not guess and produce plausible wrong bytes.
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_UNSUPPORTED,
        "zlib stream needs preset dictionary 0x%08X (FDICT), which this "
        "library cannot supply to the deflate decoder yet",
        (unsigned)state->header.dict_id);
  }

  state->stage = ZLIB_DEC_STAGE_BODY;
  return GCOMP_OK;
}

/// Accumulate the four trailer bytes and check them.
static gcomp_status_t zlib_take_trailer(
    gcomp_decoder_t * decoder, zlib_decoder_state_t * state,
    gcomp_buffer_t * input) {
  const uint8_t * in = (const uint8_t *)input->data;
  while (state->trailer_pos < ZLIB_TRAILER_SIZE) {
    if (input->used >= input->size) {
      return GCOMP_ERR_LIMIT;
    }
    state->trailer_buf[state->trailer_pos++] = in[input->used++];
    state->total_input_bytes++;
  }

  uint32_t stored = zlib_read_be32(state->trailer_buf);
  if (stored != state->adler) {
    state->stage = ZLIB_DEC_STAGE_ERROR;
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "zlib Adler-32 mismatch: stream says 0x%08X, data gives 0x%08X",
        (unsigned)stored, (unsigned)state->adler);
  }
  state->stage = ZLIB_DEC_STAGE_DONE;
  return GCOMP_OK;
}

gcomp_status_t zlib_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if ((input->size > 0 && !input->data) ||
      (output->size > 0 && !output->data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zlib_decoder_state_t * state = decoder->method_state;

  if (state->stage == ZLIB_DEC_STAGE_ERROR) {
    return GCOMP_ERR_CORRUPT;
  }
  if (state->stage == ZLIB_DEC_STAGE_DONE) {
    return GCOMP_OK;
  }

  if (state->stage == ZLIB_DEC_STAGE_HEADER) {
    gcomp_status_t status = zlib_take_header(decoder, state, input);
    if (status == GCOMP_ERR_LIMIT) {
      return GCOMP_OK; // Not an error: more input will come.
    }
    if (status != GCOMP_OK) {
      return status;
    }
  }

  if (state->stage == ZLIB_DEC_STAGE_BODY) {
    size_t out_before = output->used;
    size_t in_before = input->used;

    gcomp_status_t status =
        gcomp_decoder_update(state->inner_decoder, input, output);
    if (status != GCOMP_OK) {
      state->stage = ZLIB_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, status,
          "deflate decoder update failed: %s",
          gcomp_decoder_get_error_detail(state->inner_decoder));
    }

    size_t produced = output->used - out_before;
    state->total_input_bytes += input->used - in_before;
    if (produced > 0) {
      state->adler = gcomp_adler32_update(
          state->adler, (const uint8_t *)output->data + out_before, produced);
      state->total_output_bytes += produced;
    }

    if (state->max_output_bytes > 0 &&
        state->total_output_bytes > state->max_output_bytes) {
      state->stage = ZLIB_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "zlib output size %llu exceeds limit %llu",
          (unsigned long long)state->total_output_bytes,
          (unsigned long long)state->max_output_bytes);
    }
    if (state->max_expansion_ratio > 0 && state->total_input_bytes > 0) {
      gcomp_status_t lim = gcomp_limits_check_expansion_ratio(
          state->total_input_bytes, state->total_output_bytes,
          state->max_expansion_ratio);
      if (lim != GCOMP_OK) {
        state->stage = ZLIB_DEC_STAGE_ERROR;
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
            "zlib expansion ratio exceeds limit %llu (input=%llu, "
            "output=%llu)",
            (unsigned long long)state->max_expansion_ratio,
            (unsigned long long)state->total_input_bytes,
            (unsigned long long)state->total_output_bytes);
      }
    }

    // Nothing in the container says where the deflate data ends, so ask
    // deflate: finish() reports GCOMP_OK exactly when it has seen the final
    // block, and leaves the trailer bytes unconsumed for us.
    size_t tail_before = output->used;
    gcomp_buffer_t tail = {(uint8_t *)output->data + output->used,
        output->size - output->used, 0};
    gcomp_status_t done = gcomp_decoder_finish(state->inner_decoder, &tail);
    if (done == GCOMP_OK) {
      output->used += tail.used;
      size_t extra = output->used - tail_before;
      if (extra > 0) {
        state->adler = gcomp_adler32_update(
            state->adler, (const uint8_t *)output->data + tail_before, extra);
        state->total_output_bytes += extra;
      }
      state->stage = ZLIB_DEC_STAGE_TRAILER;
    }
  }

  if (state->stage == ZLIB_DEC_STAGE_TRAILER) {
    gcomp_status_t status = zlib_take_trailer(decoder, state, input);
    if (status == GCOMP_ERR_LIMIT) {
      return GCOMP_OK; // The trailer will arrive.
    }
    return status;
  }

  return GCOMP_OK;
}

gcomp_status_t zlib_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  (void)output;

  zlib_decoder_state_t * state = decoder->method_state;

  switch (state->stage) {
  case ZLIB_DEC_STAGE_DONE:
    return GCOMP_OK;
  case ZLIB_DEC_STAGE_ERROR:
    return GCOMP_ERR_CORRUPT;
  case ZLIB_DEC_STAGE_HEADER:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "zlib stream truncated in header (%zu of %zu bytes)",
        state->header_accum_pos, state->header_need);
  case ZLIB_DEC_STAGE_BODY:
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_CORRUPT, "zlib stream truncated in deflate data");
  case ZLIB_DEC_STAGE_TRAILER:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "zlib stream truncated in Adler-32 trailer (%zu of %u bytes)",
        state->trailer_pos, (unsigned)ZLIB_TRAILER_SIZE);
  default:
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INTERNAL, "unexpected zlib decoder stage");
  }
}

gcomp_status_t zlib_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  zlib_decoder_state_t * state = decoder->method_state;

  gcomp_status_t status = gcomp_decoder_reset(state->inner_decoder);
  if (status != GCOMP_OK) {
    return status;
  }

  state->adler = GCOMP_ADLER32_INIT;
  state->stage = ZLIB_DEC_STAGE_HEADER;
  state->header_accum_pos = 0;
  state->header_need = ZLIB_HEADER_SIZE;
  memset(&state->header, 0, sizeof(state->header));
  state->trailer_pos = 0;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;
  return GCOMP_OK;
}
