/**
 * @file zlib_register.c
 *
 * Method descriptor, option schema and registration for "zlib".
 *
 * The schema policy is GCOMP_UNKNOWN_KEY_IGNORE, as gzip's is, because a
 * caller configures the compression through `deflate.*` keys that this method
 * does not declare and passes straight through -- see
 * gcomp_clone_options_for_method().
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "zlib_internal.h"

#include "../../autoreg/autoreg_platform.h"
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/zlib.h>
#include "../../core/bound_internal.h"
#include <ghoti.io/compress/compress.h>
#include <string.h>

//
// Option schema
//

static const gcomp_option_schema_t g_zlib_option_schemas[] = {
    // zlib.dictionary - preset dictionary (RFC 1950 FDICT)
    {
        "zlib.dictionary",  // key
        GCOMP_OPT_BYTES,    // type
        0,                  // has_default
        {.ui64 = 0},        // default_value
        0,                  // has_min
        0,                  // has_max
        0,                  // min_int
        0,                  // max_int
        0,                  // min_uint
        0,                  // max_uint
        "Preset dictionary (not yet supported; rejected rather than ignored)",
    },
    // limits.max_output_bytes
    {
        "limits.max_output_bytes",              // key
        GCOMP_OPT_UINT64,                       // type
        1,                                      // has_default
        {.ui64 = ZLIB_DEFAULT_MAX_OUTPUT_BYTES}, // default_value
        0,                                      // has_min
        0,                                      // has_max
        0,                                      // min_int
        0,                                      // max_int
        0,                                      // min_uint
        0,                                      // max_uint
        "Maximum decompressed output bytes",    // help
    },
    // limits.max_memory_bytes
    {
        "limits.max_memory_bytes",               // key
        GCOMP_OPT_UINT64,                        // type
        1,                                       // has_default
        {.ui64 = ZLIB_DEFAULT_MAX_MEMORY_BYTES}, // default_value
        0,                                       // has_min
        0,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        0,                                       // min_uint
        0,                                       // max_uint
        "Maximum working memory",                // help
    },
    // limits.max_expansion_ratio
    {
        "limits.max_expansion_ratio",                // key
        GCOMP_OPT_UINT64,                            // type
        1,                                           // has_default
        {.ui64 = ZLIB_DEFAULT_MAX_EXPANSION_RATIO},  // default_value
        0,                                           // has_min
        0,                                           // has_max
        0,                                           // min_int
        0,                                           // max_int
        0,                                           // min_uint
        0,                                           // max_uint
        "Maximum output/input ratio (decompression bomb protection)", // help
    },
};

static const char * const g_zlib_option_keys[] = {
    "zlib.dictionary",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_zlib_schema = {
    g_zlib_option_schemas,
    sizeof(g_zlib_option_schemas) / sizeof(g_zlib_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_IGNORE, // deflate.* passes through to the inner method
    g_zlib_option_keys,
};

static const gcomp_method_schema_t * zlib_get_schema(void) {
  return &g_zlib_schema;
}

//
// Vtable adapters
//

static gcomp_status_t zlib_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return zlib_encoder_update(encoder, input, output);
}

static gcomp_status_t zlib_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return zlib_encoder_finish(encoder, output);
}

static gcomp_status_t zlib_encoder_flush_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output, gcomp_flush_t mode) {
  return zlib_encoder_flush(encoder, output, mode);
}

static gcomp_status_t zlib_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return zlib_decoder_update(decoder, input, output);
}

static gcomp_status_t zlib_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return zlib_decoder_finish(decoder, output);
}

static void zlib_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  zlib_encoder_destroy(encoder);
}

static void zlib_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  zlib_decoder_destroy(decoder);
}

//
// Factories
//

static gcomp_status_t zlib_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = zlib_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = zlib_encoder_update_wrapper;
  (*encoder_out)->finish_fn = zlib_encoder_finish_wrapper;
  (*encoder_out)->flush_fn = zlib_encoder_flush_wrapper;
  (*encoder_out)->reset_fn = zlib_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t zlib_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = zlib_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = zlib_decoder_update_wrapper;
  (*decoder_out)->finish_fn = zlib_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = zlib_decoder_reset;
  return GCOMP_OK;
}

//
// Method descriptor
//

//
// Worst-case encoded size
//

/**
 * @brief DEFLATE's bound plus RFC 1950's wrapper.
 *
 * Section 2.2: the two-byte CMF/FLG header and the four-byte Adler-32 of the
 * uncompressed data.  The four-byte DICTID is not counted because this encoder
 * does not write FDICT - zlib.dictionary is rejected rather than ignored - so
 * a stream that carries one cannot come from here.
 */
static gcomp_status_t zlib_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  size_t bound = 0;
  gcomp_status_t s = gcomp_bound_deflate_raw(
      input_size, gcomp_bound_deflate_window_bits(options), &bound);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_bound_add(&bound, 2u + 4u);
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}


//
// Peeking
//

/**
 * @brief The two header bytes RFC 1950 section 2.2 defines.
 *
 * Reuses gcomp_zlib_peek_header(), which is the same parser the decoder runs,
 * so the two cannot come to different conclusions about one stream.
 */
static gcomp_status_t zlib_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  (void)options;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  memset(info_out, 0, sizeof(*info_out));

  // Two bytes, or six when FDICT names a dictionary.
  if (input_size < 2) {
    if (needed_out) {
      *needed_out = 2;
    }
    return GCOMP_ERR_LIMIT;
  }

  gcomp_zlib_header_info_t zh;
  gcomp_status_t s = gcomp_zlib_peek_header(input, input_size, &zh);
  if (s != GCOMP_OK) {
    if (s == GCOMP_ERR_LIMIT && needed_out) {
      *needed_out = 6; // the FDICT form is the only longer one
    }
    return s;
  }

  info_out->header_size = zh.header_size;
  info_out->window_size = (uint64_t)1u << zh.window_bits;
  // The Adler-32 of the uncompressed data, always present (section 2.2).
  info_out->has_checksum = 1;
  info_out->has_dictionary = zh.has_dictionary ? 1 : 0;
  info_out->dictionary_id = zh.dictionary_id;
  if (needed_out) {
    *needed_out = zh.header_size;
  }
  return GCOMP_OK;
}


static const gcomp_method_t g_zlib_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "zlib",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = zlib_create_encoder,
    .create_decoder = zlib_create_decoder,
    .destroy_encoder = zlib_destroy_encoder_wrapper,
    .destroy_decoder = zlib_destroy_decoder_wrapper,
    .get_schema = zlib_get_schema,
    .encode_bound = zlib_encode_bound,
    .peek = zlib_peek,
};

gcomp_status_t gcomp_method_zlib_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_zlib_method);
}

gcomp_status_t gcomp_zlib_peek_header(const void * input, size_t input_size,
    gcomp_zlib_header_info_t * info_out) {
  if (!input || !info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (input_size < ZLIB_HEADER_SIZE) {
    return GCOMP_ERR_LIMIT;
  }

  zlib_header_t parsed;
  gcomp_status_t status =
      zlib_parse_header((const uint8_t *)input, &parsed);
  if (status != GCOMP_OK) {
    return status;
  }
  if (parsed.fdict && input_size < ZLIB_HEADER_SIZE + ZLIB_DICTID_SIZE) {
    return GCOMP_ERR_LIMIT;
  }

  info_out->window_bits = parsed.window_bits;
  info_out->level_hint = parsed.flevel;
  info_out->has_dictionary = parsed.fdict ? 1 : 0;
  info_out->dictionary_id = 0;
  info_out->header_size = ZLIB_HEADER_SIZE;
  if (parsed.fdict) {
    const uint8_t * d = (const uint8_t *)input + ZLIB_HEADER_SIZE;
    info_out->dictionary_id = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
        ((uint32_t)d[2] << 8) | (uint32_t)d[3];
    info_out->header_size = ZLIB_HEADER_SIZE + ZLIB_DICTID_SIZE;
  }
  return GCOMP_OK;
}

//
// Auto-Registration Hook
//
GCOMP_AUTOREG_METHOD(zlib, gcomp_method_zlib_register)
