/**
 * @file zlib_encoder.c
 *
 * RFC 1950 encoder: two header bytes, a deflate stream, an Adler-32.
 *
 * The compression is entirely the inner deflate encoder's; what is here is
 * framing and a checksum.  The state machine is the same three stages gzip
 * uses, because the container has the same three parts.
 *
 * ## The checksum is over the input, not the output
 *
 * RFC 1950 section 2.2: ADLER32 is "a checksum value of the uncompressed
 * data".  So it is accumulated in update() as bytes arrive, before deflate
 * sees them -- which also means it is already correct for everything a flush
 * is about to emit, and flush needs to do nothing about it.
 *
 * ## And it is big-endian
 *
 * The one field in this library stored most significant byte first.  RFC 1950
 * says so for both ADLER32 and DICTID, in a format whose own length fields
 * elsewhere are little-endian, and getting it backwards produces a stream
 * that every decoder rejects at the very last byte.
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

/// Write a 32-bit value most significant byte first (RFC 1950 section 2.2).
static void zlib_write_be32(uint8_t * out, uint32_t value) {
  out[0] = (uint8_t)((value >> 24) & 0xFFu);
  out[1] = (uint8_t)((value >> 16) & 0xFFu);
  out[2] = (uint8_t)((value >> 8) & 0xFFu);
  out[3] = (uint8_t)(value & 0xFFu);
}

gcomp_status_t zlib_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  zlib_encoder_state_t * state =
      gcomp_calloc(alloc, 1, sizeof(zlib_encoder_state_t));
  if (!state) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "failed to allocate zlib encoder state");
  }
  state->allocator = alloc;
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(zlib_encoder_state_t));

  state->max_memory_bytes = ZLIB_DEFAULT_MAX_MEMORY_BYTES;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "limits.max_memory_bytes", &v) ==
        GCOMP_OK) {
      state->max_memory_bytes = v;
    }
  }

  // A preset dictionary (RFC 1950 section 2.2): FDICT in the header, DICTID
  // after it, and the same bytes handed to deflate as the history the stream
  // starts from.
  const void * dict = NULL;
  size_t dict_len = 0;
  if (options) {
    if (gcomp_options_get_bytes(options, "zlib.dictionary", &dict, &dict_len) !=
            GCOMP_OK ||
        !dict || dict_len == 0) {
      dict = NULL;
      dict_len = 0;
    }
  }

  // CINFO has to describe the window deflate will actually use, so it is read
  // from the same option deflate reads, with deflate's own default when the
  // caller said nothing.
  uint64_t window_bits = ZLIB_WINDOW_BITS_MAX;
  int64_t level = 6;
  if (options) {
    uint64_t wb = 0;
    if (gcomp_options_get_uint64(options, "deflate.window_bits", &wb) ==
        GCOMP_OK) {
      window_bits = wb;
    }
    int64_t lv = 0;
    if (gcomp_options_get_int64(options, "deflate.level", &lv) == GCOMP_OK) {
      level = lv;
    }
  }
  if (window_bits < ZLIB_WINDOW_BITS_MIN ||
      window_bits > ZLIB_WINDOW_BITS_MAX) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "deflate.window_bits %llu has no RFC 1950 CINFO (8 to 15)",
        (unsigned long long)window_bits);
  }
  state->window_bits = (uint8_t)window_bits;
  state->level = (int)level;

  gcomp_status_t status = zlib_write_header(
      (unsigned)window_bits, (int)level, dict != NULL, state->header_buf);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(
        encoder, status, "failed to build zlib header");
  }
  state->header_len = ZLIB_HEADER_SIZE;
  if (dict) {
    // DICTID is the Adler-32 of the dictionary as supplied, in network byte
    // order.  Of the whole of it, not of the tail deflate will actually match
    // against: section 2.2 identifies what the caller handed over, and zlib's
    // deflateSetDictionary hashes the same thing.
    uint32_t dict_id = gcomp_adler32((const uint8_t *)dict, dict_len);
    state->header_buf[ZLIB_HEADER_SIZE + 0] = (uint8_t)(dict_id >> 24);
    state->header_buf[ZLIB_HEADER_SIZE + 1] = (uint8_t)(dict_id >> 16);
    state->header_buf[ZLIB_HEADER_SIZE + 2] = (uint8_t)(dict_id >> 8);
    state->header_buf[ZLIB_HEADER_SIZE + 3] = (uint8_t)dict_id;
    state->header_len = ZLIB_HEADER_SIZE + ZLIB_DICTID_SIZE;
  }
  state->header_pos = 0;

  gcomp_options_t * deflate_options = NULL;
  status = gcomp_clone_options_for_method(
      registry, "deflate", options, &deflate_options);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(
        encoder, status, "failed to prepare deflate options");
  }
  // zlib.dictionary and deflate.dictionary are different keys, so the filtered
  // clone does not carry it across; it is put in by name.
  if (dict && deflate_options) {
    status = gcomp_options_set_bytes(
        deflate_options, "deflate.dictionary", dict, dict_len);
    if (status != GCOMP_OK) {
      gcomp_options_destroy(deflate_options);
      gcomp_free(alloc, state);
      return gcomp_encoder_set_error(
          encoder, status, "failed to hand the preset dictionary to deflate");
    }
  }

  status = gcomp_encoder_create(
      registry, "deflate", deflate_options, &state->inner_encoder);
  if (deflate_options) {
    gcomp_options_destroy(deflate_options);
  }
  if (status != GCOMP_OK) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(
        encoder, status, "failed to create inner deflate encoder");
  }

  if (gcomp_memory_check_limit(&state->mem_tracker, state->max_memory_bytes) !=
      GCOMP_OK) {
    gcomp_encoder_destroy(state->inner_encoder);
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
        "zlib encoder memory usage exceeds limit");
  }

  state->adler = GCOMP_ADLER32_INIT;
  state->stage = ZLIB_ENC_STAGE_HEADER;
  encoder->method_state = state;
  return GCOMP_OK;
}

void zlib_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }
  zlib_encoder_state_t * state = encoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;
  if (state->inner_encoder) {
    gcomp_encoder_destroy(state->inner_encoder);
  }
  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

/// Hand out header bytes; true once they have all gone.
static bool zlib_emit_header(
    zlib_encoder_state_t * state, gcomp_buffer_t * output) {
  uint8_t * out = (uint8_t *)output->data;
  while (state->header_pos < state->header_len && output->used < output->size) {
    out[output->used++] = state->header_buf[state->header_pos++];
  }
  if (state->header_pos >= state->header_len) {
    state->stage = ZLIB_ENC_STAGE_BODY;
    return true;
  }
  return false;
}

gcomp_status_t zlib_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if ((input->size > 0 && !input->data) ||
      (output->size > 0 && !output->data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zlib_encoder_state_t * state = encoder->method_state;

  if (state->stage == ZLIB_ENC_STAGE_HEADER) {
    if (!zlib_emit_header(state, output)) {
      return GCOMP_OK; // Need more output space.
    }
  }

  if (state->stage != ZLIB_ENC_STAGE_BODY) {
    return GCOMP_OK;
  }

  size_t before = input->used;
  gcomp_status_t status =
      gcomp_encoder_update(state->inner_encoder, input, output);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, status,
        "deflate encoder update failed: %s",
        gcomp_encoder_get_error_detail(state->inner_encoder));
  }

  // The checksum is over the uncompressed data, so it follows what deflate
  // took, not what it produced.
  size_t consumed = input->used - before;
  if (consumed > 0) {
    state->adler = gcomp_adler32_update(
        state->adler, (const uint8_t *)input->data + before, consumed);
  }
  return GCOMP_OK;
}

gcomp_status_t zlib_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zlib_encoder_state_t * state = encoder->method_state;

  if (state->stage == ZLIB_ENC_STAGE_TRAILER ||
      state->stage == ZLIB_ENC_STAGE_DONE) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "zlib encoder cannot flush after finish");
  }

  // The header comes before any deflate data, so a flush before the first
  // byte of input still has this much to hand over.
  if (state->stage == ZLIB_ENC_STAGE_HEADER) {
    if (!zlib_emit_header(state, output)) {
      return GCOMP_ERR_LIMIT;
    }
  }

  // Nothing zlib-specific to do: the Adler-32 is accumulated as input arrives
  // and is already correct for everything this is about to emit.
  gcomp_status_t status =
      gcomp_encoder_flush(state->inner_encoder, output, mode);
  if (status == GCOMP_ERR_LIMIT) {
    return GCOMP_ERR_LIMIT;
  }
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, status,
        "deflate encoder flush failed: %s",
        gcomp_encoder_get_error_detail(state->inner_encoder));
  }
  return GCOMP_OK;
}

gcomp_status_t zlib_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zlib_encoder_state_t * state = encoder->method_state;

  if (state->stage == ZLIB_ENC_STAGE_HEADER) {
    if (!zlib_emit_header(state, output)) {
      // GCOMP_ERR_LIMIT, not GCOMP_OK: finish() reports completion with
      // GCOMP_OK, so saying so here would make a truncated stream
      // indistinguishable from a finished one.
      return GCOMP_ERR_LIMIT;
    }
  }

  if (state->stage == ZLIB_ENC_STAGE_BODY) {
    gcomp_status_t status = gcomp_encoder_finish(state->inner_encoder, output);
    if (status == GCOMP_ERR_LIMIT) {
      return GCOMP_ERR_LIMIT;
    }
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(encoder, status,
          "deflate encoder finish failed: %s",
          gcomp_encoder_get_error_detail(state->inner_encoder));
    }
    zlib_write_be32(state->trailer_buf, state->adler);
    state->trailer_pos = 0;
    state->stage = ZLIB_ENC_STAGE_TRAILER;
  }

  if (state->stage == ZLIB_ENC_STAGE_TRAILER) {
    uint8_t * out = (uint8_t *)output->data;
    while (state->trailer_pos < ZLIB_TRAILER_SIZE &&
        output->used < output->size) {
      out[output->used++] = state->trailer_buf[state->trailer_pos++];
    }
    if (state->trailer_pos < ZLIB_TRAILER_SIZE) {
      return GCOMP_ERR_LIMIT;
    }
    state->stage = ZLIB_ENC_STAGE_DONE;
  }

  return state->stage == ZLIB_ENC_STAGE_DONE ? GCOMP_OK : GCOMP_ERR_LIMIT;
}

gcomp_status_t zlib_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  zlib_encoder_state_t * state = encoder->method_state;

  gcomp_status_t status = gcomp_encoder_reset(state->inner_encoder);
  if (status != GCOMP_OK) {
    return status;
  }

  state->adler = GCOMP_ADLER32_INIT;
  state->stage = ZLIB_ENC_STAGE_HEADER;
  state->header_pos = 0;
  state->trailer_pos = 0;
  return GCOMP_OK;
}
