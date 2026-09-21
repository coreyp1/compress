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
#include "../deflate/deflate_internal.h"

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

    // RFC 1950 section 2.2: the stream was compressed against a dictionary the
    // decompressor "must be presented with", and DICTID is the Adler-32 of
    // that dictionary, so the caller has to supply the right one and we can
    // check that they did.  Without it there is nothing to do but say which
    // one is wanted - guessing would produce plausible wrong bytes.
    const void * dict = NULL;
    size_t dict_len = 0;
    if (!decoder->options ||
        gcomp_options_get_bytes(
            decoder->options, "zlib.dictionary", &dict, &dict_len) != GCOMP_OK ||
        !dict || dict_len == 0) {
      state->stage = ZLIB_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_UNSUPPORTED,
          "zlib stream needs preset dictionary 0x%08X (FDICT); supply it as "
          "zlib.dictionary",
          (unsigned)state->header.dict_id);
    }

    uint32_t have = gcomp_adler32((const uint8_t *)dict, dict_len);
    if (have != state->header.dict_id) {
      state->stage = ZLIB_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "zlib stream names preset dictionary 0x%08X but the one supplied is "
          "0x%08X",
          (unsigned)state->header.dict_id, (unsigned)have);
    }

    gcomp_status_t ds = gcomp_deflate_decoder_set_dictionary(
        state->inner_decoder, dict, dict_len);
    if (ds != GCOMP_OK) {
      state->stage = ZLIB_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, ds,
          "could not load preset dictionary 0x%08X into the deflate decoder",
          (unsigned)state->header.dict_id);
    }
  }

  state->stage = ZLIB_DEC_STAGE_BODY;
  return GCOMP_OK;
}

/**
 * @brief Take back whatever the deflate decoder read past the end of the
 *        stream, which is where the trailer usually is.
 *
 * The deflate bit buffer refills eight bytes at a time, so by the time the
 * final block is recognised the four trailer bytes are normally already
 * inside it.  deflate hands them back by rewinding the input buffer they came
 * from -- but it can only do that when they came from the buffer that call
 * was given.  Bytes it took during an earlier call have nowhere to be put
 * back to, so it keeps them, and they have to be asked for.
 *
 * Nothing here asked.  The Adler-32 therefore went missing whenever the
 * stream happened to end on a call whose input was already spent -- which a
 * small output buffer makes the normal case, because the input runs out long
 * before the output does -- and a perfectly good stream was reported as
 * truncated in its trailer.  gzip has asked all along; see the same
 * retrieval in gzip_decoder.c.
 *
 * Called once, at the move to ZLIB_DEC_STAGE_TRAILER, because the count the
 * deflate decoder reports is not cleared by reading it.
 */
static void zlib_take_readahead(zlib_decoder_state_t * state) {
  state->trailer_pos = 0;
  uint32_t held =
      gcomp_deflate_decoder_get_unconsumed_bytes(state->inner_decoder);
  if (held > 0 && held <= ZLIB_TRAILER_SIZE) {
    gcomp_deflate_decoder_get_unconsumed_data(
        state->inner_decoder, state->trailer_buf, held);
    state->trailer_pos = held;
  }
}

/// Compare the four accumulated trailer bytes against the running Adler-32.
static gcomp_status_t zlib_check_trailer(
    gcomp_decoder_t * decoder, zlib_decoder_state_t * state) {
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

/// Accumulate the four trailer bytes from the input and check them.
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
  return zlib_check_trailer(decoder, state);
}

/**
 * @brief Give the inner deflate decoder room to finish, and account for what
 *        comes out.
 *
 * Shared by update() and finish(), which owe the caller the same thing: the
 * deflate decoder holds state -- bits already read, a match half copied -- and
 * the only way to learn whether the stream has ended is to offer it somewhere
 * to put the rest.
 *
 * Returns what the inner finish returned.  GCOMP_ERR_LIMIT means the output
 * buffer filled first and there is more to come; it is not an error and the
 * stage stays where it was.
 */
static gcomp_status_t zlib_drain_body(gcomp_decoder_t * decoder,
    zlib_decoder_state_t * state, gcomp_buffer_t * output) {
  size_t before = output->used;
  gcomp_buffer_t tail = {(uint8_t *)output->data + output->used,
      output->size - output->used, 0};
  gcomp_status_t done = gcomp_decoder_finish(state->inner_decoder, &tail);

  output->used += tail.used;
  if (tail.used > 0) {
    state->adler = gcomp_adler32_update(
        state->adler, (const uint8_t *)output->data + before, tail.used);
    state->total_output_bytes += tail.used;
    if (state->max_output_bytes > 0 &&
        state->total_output_bytes > state->max_output_bytes) {
      state->stage = ZLIB_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "zlib output size %llu exceeds limit %llu",
          (unsigned long long)state->total_output_bytes,
          (unsigned long long)state->max_output_bytes);
    }
  }
  if (done == GCOMP_OK) {
    zlib_take_readahead(state);
    state->stage = ZLIB_DEC_STAGE_TRAILER;
  }
  return done;
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
    // block, and hands back the trailer bytes it had read ahead.
    gcomp_status_t done = zlib_drain_body(decoder, state, output);
    if (done != GCOMP_OK && state->stage == ZLIB_DEC_STAGE_ERROR) {
      return done;
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

/**
 * @brief Finish a zlib decode.
 *
 * WHY THIS WRITES TO THE OUTPUT BUFFER
 * ====================================
 *
 * It used to ignore it -- `(void)output` -- and report any unfinished stage as
 * a truncated stream.  That is right only if "no more input" meant "no more
 * output", and it does not.  The deflate decoder stops the instant the output
 * buffer is full, keeping whatever it had not room for: bits already read out
 * of the input, and a match half copied.  A caller with a small output buffer
 * runs out of input long before the decoder runs out of things to say -- 193
 * bytes still owed, on the case that found this -- and was then told its
 * perfectly good stream was truncated.
 *
 * So finish() drains, exactly as gcomp_deflate_decoder_finish() does, and
 * reports GCOMP_ERR_LIMIT when the buffer fills before the stream ends: drain
 * it and call again.  Truncation is what is left when there is room to spare
 * and it still cannot finish.
 */
gcomp_status_t zlib_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zlib_decoder_state_t * state = decoder->method_state;

  if (state->stage == ZLIB_DEC_STAGE_BODY) {
    gcomp_status_t done = zlib_drain_body(decoder, state, output);
    if (done == GCOMP_ERR_LIMIT) {
      // The buffer filled first.  Nothing is wrong; there is simply more.
      return GCOMP_ERR_LIMIT;
    }
    if (done != GCOMP_OK) {
      state->stage = ZLIB_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_CORRUPT, "zlib stream truncated in deflate data");
    }
    // zlib_drain_body() moved the stage on and collected the trailer bytes
    // deflate had read ahead; whether all four arrived is asked below.
  }

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
    if (state->trailer_pos >= ZLIB_TRAILER_SIZE) {
      return zlib_check_trailer(decoder, state);
    }
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
