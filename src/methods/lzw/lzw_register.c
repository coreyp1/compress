/**
 * @file lzw_register.c
 *
 * LZW method registration for the Ghoti.io Compress library.
 *
 * - Method descriptor with vtable hooks for encoder/decoder
 * - Option schema: lzw.format, lzw.lit_width, lzw.max_code_bits, limits.*
 * - Public registration and auto-registration (GCOMP_AUTOREG_METHOD)
 *
 * SCHEMA / OPTIONS
 * ================
 *
 * Public options are exposed through the registry schema so callers can:
 * - discover available keys (introspection)
 * - validate user-provided options at create time
 * - reject unknown keys (policy: GCOMP_UNKNOWN_KEY_ERROR)
 *
 * LZW is profile-driven: `lzw.format` selects the on-the-wire variant:
 * - `"gif"`  : LSB-first bit packing and GIF code-width growth rule
 * - `"tiff"` : MSB-first bit packing and TIFF code-width growth rule
 *
 * The library does not define a container or embed the format identifier in
 * the stream. Callers must supply matching options on encode and decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "lzw_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

//
// Option schema (LW1.1): lzw.format, lzw.lit_width, lzw.max_code_bits, limits.*
//

static const char LZW_DEFAULT_FORMAT[] = "gif";
static const char LZW_DEFAULT_ENCODER_LOOKUP[] = "hash";

static const gcomp_option_schema_t g_lzw_option_schemas[] = {
    {
        "lzw.format",                // key
        GCOMP_OPT_STRING,            // type
        1,                           // has_default
        {.str = LZW_DEFAULT_FORMAT}, // default_value
        0,                           // has_min
        0,                           // has_max
        0,                           // min_int
        0,                           // max_int
        0,                           // min_uint
        0,                           // max_uint
        "LZW format: gif or tiff",   // help
    },
    {
        "lzw.lit_width",  // key
        GCOMP_OPT_UINT64, // type
        0,                // has_default (optional; format default used)
        {.ui64 = 0},      // default_value
        0,                // has_min
        0,                // has_max
        0,                // min_int
        0,                // max_int
        0,                // min_uint
        0,                // max_uint
        "Initial code width in bits (e.g. 8 for GIF, 9 for TIFF)",
    },
    {
        "lzw.max_code_bits", // key
        GCOMP_OPT_UINT64,    // type
        1,                   // has_default
        {.ui64 = 12},        // default_value
        0,                   // has_min
        0,                   // has_max
        0,                   // min_int
        0,                   // max_int
        0,                   // min_uint
        0,                   // max_uint
        "Maximum code width in bits (e.g. 12 for 4096 entries)",
    },
    {
        "lzw.encoder_lookup",                // key
        GCOMP_OPT_STRING,                    // type
        1,                                   // has_default
        {.str = LZW_DEFAULT_ENCODER_LOOKUP}, // default_value
        0,
        0,
        0,
        0,
        0,
        0,
        "Encoder lookup mode: linear or hash (hash is faster)",
    },
    {
        "limits.max_output_bytes",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum decompressed output bytes",
    },
    {
        "limits.max_memory_bytes",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum memory usage",
    },
    {
        "limits.max_expansion_ratio",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "Decompression bomb protection (output/input ratio)",
    },
};

static const char * const g_lzw_option_keys[] = {
    "lzw.format",
    "lzw.lit_width",
    "lzw.max_code_bits",
    "lzw.encoder_lookup",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_lzw_schema = {
    g_lzw_option_schemas,
    sizeof(g_lzw_option_schemas) / sizeof(g_lzw_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_lzw_option_keys,
};

static const gcomp_method_schema_t * lzw_get_schema(void) {
  return &g_lzw_schema;
}

//
// Encoder/decoder wrappers
//

static gcomp_status_t lzw_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return lzw_encoder_update(encoder, input, output);
}

static gcomp_status_t lzw_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return lzw_encoder_finish(encoder, output);
}

static gcomp_status_t lzw_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return lzw_decoder_update(decoder, input, output);
}

static gcomp_status_t lzw_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return lzw_decoder_finish(decoder, output);
}

//
// Factory functions
//

static gcomp_status_t lzw_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = lzw_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = lzw_encoder_update_wrapper;
  (*encoder_out)->finish_fn = lzw_encoder_finish_wrapper;
  (*encoder_out)->reset_fn = lzw_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t lzw_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = lzw_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = lzw_decoder_update_wrapper;
  (*decoder_out)->finish_fn = lzw_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = lzw_decoder_reset;
  return GCOMP_OK;
}

static void lzw_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  lzw_encoder_destroy(encoder);
}

static void lzw_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  lzw_decoder_destroy(decoder);
}

//
// Method descriptor
//

static const gcomp_method_t g_lzw_method = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "lzw",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = lzw_create_encoder,
    .create_decoder = lzw_create_decoder,
    .destroy_encoder = lzw_destroy_encoder_wrapper,
    .destroy_decoder = lzw_destroy_decoder_wrapper,
    .get_schema = lzw_get_schema,
};

gcomp_status_t gcomp_method_lzw_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_lzw_method);
}

GCOMP_AUTOREG_METHOD(lzw, gcomp_method_lzw_register)
