/**
 * @file zstd_register.c
 *
 * Zstandard method registration for the Ghoti.io Compress library.
 *
 * This file provides:
 * - Method descriptor with vtable hooks for encoder/decoder
 * - Option schema defining Zstd-specific options
 * - Public registration function `gcomp_method_zstd_register()`
 * - Auto-registration hook for the default registry
 *
 * ## Architecture
 *
 * The Zstd method is a standalone compression method (not a wrapper).
 * It implements the Zstandard frame format specification, which provides:
 * - Frame header with configuration flags
 * - Multiple data blocks (Raw, RLE, or Compressed)
 * - Optional content checksum (xxHash64 low 32 bits)
 *
 * ## Options
 *
 * Zstd-specific options use the `zstd.*` prefix:
 * - `zstd.level`: Compression level (1-22)
 * - `zstd.checksum`: Enable content checksum
 * - `zstd.window_log`: Window log (10-31, or 0 for auto)
 * - `zstd.content_size`: Content size for header (optional)
 * - `zstd.concat`: Decoder: support concatenated frames
 *
 * Limit options use the `limits.*` prefix (shared with core):
 * - `limits.max_output_bytes`: Maximum decompressed output
 * - `limits.max_window_bytes`: Maximum window size
 * - `limits.max_memory_bytes`: Maximum memory usage
 * - `limits.max_expansion_ratio`: Decompression bomb protection
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "zstd_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zstd.h>
#include <stddef.h>
#include <stdint.h>

//
// Option Schema
//

static const gcomp_option_schema_t g_zstd_option_schemas[] = {
    // zstd.level - Compression level (1-22)
    {
        "zstd.level",                // key
        GCOMP_OPT_INT64,             // type
        1,                           // has_default
        {.i64 = ZSTD_LEVEL_DEFAULT}, // default_value
        1,                           // has_min
        1,                           // has_max
        ZSTD_LEVEL_MIN,              // min_int
        ZSTD_LEVEL_MAX,              // max_int
        0,                           // min_uint
        0,                           // max_uint
        "Compression level (1-22, higher = better compression)", // help
    },
    // zstd.checksum - Enable content checksum
    {
        "zstd.checksum",                    // key
        GCOMP_OPT_BOOL,                     // type
        1,                                  // has_default
        {.b = false},                       // default_value
        0,                                  // has_min
        0,                                  // has_max
        0,                                  // min_int
        0,                                  // max_int
        0,                                  // min_uint
        0,                                  // max_uint
        "Enable xxHash64 content checksum", // help
    },
    // zstd.window_log - Window log (10-31, or 0 for auto)
    {
        "zstd.window_log",                       // key
        GCOMP_OPT_UINT64,                        // type
        1,                                       // has_default
        {.ui64 = 0},                             // default_value (0 = auto)
        1,                                       // has_min
        1,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        0,                                       // min_uint (0 = auto allowed)
        ZSTD_WINDOW_LOG_MAX,                     // max_uint
        "Window log (10-31, 0=auto from level)", // help
    },
    // zstd.content_size - Content size for header (optional)
    {
        "zstd.content_size",                       // key
        GCOMP_OPT_UINT64,                          // type
        0,                                         // has_default (optional)
        {.ui64 = 0},                               // default_value
        0,                                         // has_min
        0,                                         // has_max
        0,                                         // min_int
        0,                                         // max_int
        0,                                         // min_uint
        0,                                         // max_uint
        "Content size (written to header if set)", // help
    },
    // zstd.concat - Decoder: support concatenated frames
    {
        "zstd.concat",                               // key
        GCOMP_OPT_BOOL,                              // type
        1,                                           // has_default
        {.b = false},                                // default_value
        0,                                           // has_min
        0,                                           // has_max
        0,                                           // min_int
        0,                                           // max_int
        0,                                           // min_uint
        0,                                           // max_uint
        "Decoder: support concatenated zstd frames", // help
    },
    // limits.max_output_bytes - Maximum decompressed output
    {
        "limits.max_output_bytes",               // key
        GCOMP_OPT_UINT64,                        // type
        1,                                       // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_OUTPUT_BYTES}, // default_value
        1,                                       // has_min
        0,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        1,                                       // min_uint (at least 1 byte)
        0,                                       // max_uint
        "Maximum decompressed output bytes",     // help
    },
    // limits.max_window_bytes - Maximum window size
    {
        "limits.max_window_bytes",               // key
        GCOMP_OPT_UINT64,                        // type
        1,                                       // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_WINDOW_BYTES}, // default_value
        1,                                       // has_min
        0,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        1,                                       // min_uint
        0,                                       // max_uint
        "Maximum window size in bytes",          // help
    },
    // limits.max_memory_bytes - Maximum memory usage
    {
        "limits.max_memory_bytes",                // key
        GCOMP_OPT_UINT64,                         // type
        1,                                        // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_MEMORY_BYTES},  // default_value
        1,                                        // has_min
        0,                                        // has_max
        0,                                        // min_int
        0,                                        // max_int
        1,                                        // min_uint
        0,                                        // max_uint
        "Maximum memory usage (buffers, tables)", // help
    },
    // limits.max_expansion_ratio - Decompression bomb protection
    {
        "limits.max_expansion_ratio",               // key
        GCOMP_OPT_UINT64,                           // type
        1,                                          // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_EXPANSION_RATIO}, // default_value
        1,                                          // has_min
        0,                                          // has_max
        0,                                          // min_int
        0,                                          // max_int
        1,                                          // min_uint
        0,                                          // max_uint
        "Maximum output/input ratio (decompression bomb protection)", // help
    },
    // threads.count - Number of worker threads for parallel compression
    {
        "threads.count",                                       // key
        GCOMP_OPT_UINT64,                                      // type
        1,                                                     // has_default
        {.ui64 = 1},                                           // default_value
        1,                                                     // has_min
        0,                                                     // has_max
        0,                                                     // min_int
        0,                                                     // max_int
        0,                                                     // min_uint
        0,                                                     // max_uint
        "Number of worker threads (0 or 1 = single-threaded)", // help
    },
    // zstd.job_size - Size of each compression job in parallel mode
    {
        "zstd.job_size",   // key
        GCOMP_OPT_UINT64,  // type
        1,                 // has_default
        {.ui64 = 0},       // default_value (0 = auto)
        1,                 // has_min
        1,                 // has_max
        0,                 // min_int
        0,                 // max_int
        0,                 // min_uint (0 = auto)
        ZSTD_MAX_JOB_SIZE, // max_uint
        "Job size for parallel compression (0=auto, min 64KB)", // help
    },
};

static const char * const g_zstd_option_keys[] = {
    "zstd.level",
    "zstd.checksum",
    "zstd.window_log",
    "zstd.content_size",
    "zstd.concat",
    "zstd.job_size",
    "threads.count",
    "limits.max_output_bytes",
    "limits.max_window_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_zstd_schema = {
    g_zstd_option_schemas,
    sizeof(g_zstd_option_schemas) / sizeof(g_zstd_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR, // Zstd doesn't wrap another method
    g_zstd_option_keys,
};

static const gcomp_method_schema_t * zstd_get_schema(void) {
  return &g_zstd_schema;
}

//
// Option Validation
//

/**
 * @brief Validate window_log option value.
 *
 * Window log must be 0 (auto) or in range [10, 31].
 */
static bool zstd_validate_window_log(uint64_t window_log) {
  return window_log == 0 ||
      (window_log >= ZSTD_WINDOW_LOG_MIN && window_log <= ZSTD_WINDOW_LOG_MAX);
}

//
// Encoder/Decoder Wrappers
//

static gcomp_status_t zstd_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return zstd_encoder_update(encoder, input, output);
}

static gcomp_status_t zstd_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return zstd_encoder_finish(encoder, output);
}

static gcomp_status_t zstd_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return zstd_decoder_update(decoder, input, output);
}

static gcomp_status_t zstd_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return zstd_decoder_finish(decoder, output);
}

//
// Encoder/Decoder Factory Functions
//

static gcomp_status_t zstd_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Validate window_log option if provided
  if (options) {
    uint64_t window_log_val;
    if (gcomp_options_get_uint64(options, "zstd.window_log", &window_log_val) ==
        GCOMP_OK) {
      if (!zstd_validate_window_log(window_log_val)) {
        return GCOMP_ERR_INVALID_ARG;
      }
    }
  }

  gcomp_status_t status = zstd_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = zstd_encoder_update_wrapper;
  (*encoder_out)->finish_fn = zstd_encoder_finish_wrapper;
  (*encoder_out)->reset_fn = zstd_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t zstd_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = zstd_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = zstd_decoder_update_wrapper;
  (*decoder_out)->finish_fn = zstd_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = zstd_decoder_reset;
  return GCOMP_OK;
}

static void zstd_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  zstd_encoder_destroy(encoder);
}

static void zstd_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  zstd_decoder_destroy(decoder);
}

//
// Method Descriptor
//

static const gcomp_method_t g_zstd_method = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "zstd",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = zstd_create_encoder,
    .create_decoder = zstd_create_decoder,
    .destroy_encoder = zstd_destroy_encoder_wrapper,
    .destroy_decoder = zstd_destroy_decoder_wrapper,
    .get_schema = zstd_get_schema,
};

//
// Public Registration Function
//

gcomp_status_t gcomp_method_zstd_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_zstd_method);
}

//
// Auto-Registration Hook
//
GCOMP_AUTOREG_METHOD(zstd, gcomp_method_zstd_register)
