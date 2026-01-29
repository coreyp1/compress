/**
 * @file deflate_register.c
 *
 * DEFLATE method registration and option schema for the Ghoti.io Compress
 * library. Encoder/decoder implementation is stubbed until T3.5/T3.6.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/stream_internal.h"
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

//
// Option schema for deflate (T3.2). Limits are in core (limits.max_output_bytes
// etc.); deflate-specific options are defined here.
//

#define DEFLATE_LEVEL_DEFAULT 6
#define DEFLATE_LEVEL_MIN 0
#define DEFLATE_LEVEL_MAX 9
#define DEFLATE_WINDOW_BITS_DEFAULT 15
#define DEFLATE_WINDOW_BITS_MIN 8
#define DEFLATE_WINDOW_BITS_MAX 15

static const gcomp_option_schema_t g_deflate_option_schemas[] = {
    {
        "deflate.level",                          /* key */
        GCOMP_OPT_INT64,                          /* type */
        1,                                        /* has_default */
        {.i64 = DEFLATE_LEVEL_DEFAULT},           /* default_value */
        1,                                        /* has_min */
        1,                                        /* has_max */
        DEFLATE_LEVEL_MIN,                        /* min_int */
        DEFLATE_LEVEL_MAX,                        /* max_int */
        0,                                        /* min_uint */
        0,                                        /* max_uint */
        "Compression level 0 (none) to 9 (best)", /* help */
    },
    {
        "deflate.window_bits",                         /* key */
        GCOMP_OPT_UINT64,                              /* type */
        1,                                             /* has_default */
        {.ui64 = DEFLATE_WINDOW_BITS_DEFAULT},         /* default_value */
        1,                                             /* has_min */
        1,                                             /* has_max */
        0,                                             /* min_int */
        0,                                             /* max_int */
        DEFLATE_WINDOW_BITS_MIN,                       /* min_uint */
        DEFLATE_WINDOW_BITS_MAX,                       /* max_uint */
        "LZ77 window size in bits (8..15, 32KiB max)", /* help */
    },
};

static const char * const g_deflate_option_keys[] = {
    "deflate.level",
    "deflate.window_bits",
};

static const gcomp_method_schema_t g_deflate_schema = {
    g_deflate_option_schemas,
    sizeof(g_deflate_option_schemas) / sizeof(g_deflate_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_deflate_option_keys,
};

static const gcomp_method_schema_t * deflate_get_schema(void) {
  return &g_deflate_schema;
}

//
// Stub encoder/decoder: return GCOMP_ERR_UNSUPPORTED until T3.5/T3.6.
//

static gcomp_status_t deflate_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)encoder;
  (void)input;
  (void)output;
  return GCOMP_ERR_UNSUPPORTED;
}

static gcomp_status_t deflate_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  (void)encoder;
  (void)output;
  return GCOMP_ERR_UNSUPPORTED;
}

static gcomp_status_t deflate_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)decoder;
  (void)input;
  (void)output;
  return GCOMP_ERR_UNSUPPORTED;
}

static gcomp_status_t deflate_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  (void)decoder;
  (void)output;
  return GCOMP_ERR_UNSUPPORTED;
}

static gcomp_status_t deflate_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  (void)registry;
  (void)options;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  (*encoder_out)->update_fn = deflate_encoder_update;
  (*encoder_out)->finish_fn = deflate_encoder_finish;
  return GCOMP_OK;
}

static gcomp_status_t deflate_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  (void)registry;
  (void)options;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  (*decoder_out)->update_fn = deflate_decoder_update;
  (*decoder_out)->finish_fn = deflate_decoder_finish;
  return GCOMP_OK;
}

static void deflate_destroy_encoder(gcomp_encoder_t * encoder) {
  (void)encoder;
}

static void deflate_destroy_decoder(gcomp_decoder_t * decoder) {
  (void)decoder;
}

//
// Method descriptor
//

static const gcomp_method_t g_deflate_method = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "deflate",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = deflate_create_encoder,
    .create_decoder = deflate_create_decoder,
    .destroy_encoder = deflate_destroy_encoder,
    .destroy_decoder = deflate_destroy_decoder,
    .get_schema = deflate_get_schema,
};

gcomp_status_t gcomp_method_deflate_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_deflate_method);
}
