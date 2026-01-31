/**
 * @file lz4_register.c
 *
 * LZ4 Frame Format method registration for the Ghoti.io Compress library.
 *
 * This file provides:
 * - Method descriptor with vtable hooks for encoder/decoder
 * - Option schema defining LZ4-specific options
 * - Public registration function `gcomp_method_lz4_register()`
 * - Auto-registration hook for the default registry
 *
 * ## Architecture
 *
 * The LZ4 method is a standalone compression method (not a wrapper).
 * It implements the LZ4 Frame Format specification, which provides:
 * - Frame header with configuration flags
 * - Multiple data blocks with optional checksums
 * - Optional content checksum
 *
 * ## Options
 *
 * LZ4-specific options use the `lz4.*` prefix:
 * - `lz4.block_size`: Block size (64KB, 256KB, 1MB, or 4MB)
 * - `lz4.block_checksum`: Enable per-block checksums
 * - `lz4.content_checksum`: Enable content checksum in trailer
 * - `lz4.independent_blocks`: Use independent blocks (parallel-friendly)
 * - `lz4.concat`: Decoder: support concatenated frames
 *
 * Limit options use the `limits.*` prefix (shared with core):
 * - `limits.max_output_bytes`: Maximum decompressed output
 * - `limits.max_block_bytes`: Maximum block size during decode
 * - `limits.max_memory_bytes`: Maximum memory usage
 * - `limits.max_expansion_ratio`: Decompression bomb protection
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "lz4_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

//
// Option Schema
//

static const gcomp_option_schema_t g_lz4_option_schemas[] = {
    // lz4.block_size - Block size (must be one of: 64KB, 256KB, 1MB, 4MB)
    {
        "lz4.block_size",                 // key
        GCOMP_OPT_UINT64,                 // type
        1,                                // has_default
        {.ui64 = LZ4_DEFAULT_BLOCK_SIZE}, // default_value
        1,                                // has_min
        1,                                // has_max
        0,                                // min_int
        0,                                // max_int
        LZ4_BLOCK_SIZE_64KB,              // min_uint
        LZ4_BLOCK_SIZE_4MB,               // max_uint
        "Block size in bytes (65536, 262144, 1048576, or 4194304)", // help
    },
    // lz4.block_checksum - Enable per-block checksum
    {
        "lz4.block_checksum",                 // key
        GCOMP_OPT_BOOL,                       // type
        1,                                    // has_default
        {.b = LZ4_DEFAULT_BLOCK_CHECKSUM},    // default_value
        0,                                    // has_min
        0,                                    // has_max
        0,                                    // min_int
        0,                                    // max_int
        0,                                    // min_uint
        0,                                    // max_uint
        "Enable per-block xxHash32 checksum", // help
    },
    // lz4.content_checksum - Enable content checksum in trailer
    {
        "lz4.content_checksum",                              // key
        GCOMP_OPT_BOOL,                                      // type
        1,                                                   // has_default
        {.b = LZ4_DEFAULT_CONTENT_CHECKSUM},                 // default_value
        0,                                                   // has_min
        0,                                                   // has_max
        0,                                                   // min_int
        0,                                                   // max_int
        0,                                                   // min_uint
        0,                                                   // max_uint
        "Enable content xxHash32 checksum in frame trailer", // help
    },
    // lz4.independent_blocks - Use independent blocks
    {
        "lz4.independent_blocks",                              // key
        GCOMP_OPT_BOOL,                                        // type
        1,                                                     // has_default
        {.b = LZ4_DEFAULT_INDEPENDENT_BLOCKS},                 // default_value
        0,                                                     // has_min
        0,                                                     // has_max
        0,                                                     // min_int
        0,                                                     // max_int
        0,                                                     // min_uint
        0,                                                     // max_uint
        "Use independent blocks (better for parallel decode)", // help
    },
    // lz4.concat - Decoder: support concatenated frames
    {
        "lz4.concat",                               // key
        GCOMP_OPT_BOOL,                             // type
        1,                                          // has_default
        {.b = LZ4_DEFAULT_CONCAT},                  // default_value
        0,                                          // has_min
        0,                                          // has_max
        0,                                          // min_int
        0,                                          // max_int
        0,                                          // min_uint
        0,                                          // max_uint
        "Decoder: support concatenated LZ4 frames", // help
    },
    // lz4.dictionary_id - Dictionary ID (optional, parsing only in v1)
    {
        "lz4.dictionary_id",                        // key
        GCOMP_OPT_UINT64,                           // type
        0,                                          // has_default (optional)
        {.ui64 = 0},                                // default_value
        1,                                          // has_min
        1,                                          // has_max
        0,                                          // min_int
        0,                                          // max_int
        0,                                          // min_uint
        0xFFFFFFFFU,                                // max_uint
        "Dictionary ID (written to header if set)", // help
    },
    // lz4.content_size - Content size (optional)
    {
        "lz4.content_size",                        // key
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
    // limits.max_output_bytes - Maximum decompressed output
    {
        "limits.max_output_bytes",              // key
        GCOMP_OPT_UINT64,                       // type
        1,                                      // has_default
        {.ui64 = LZ4_DEFAULT_MAX_OUTPUT_BYTES}, // default_value
        1,                                      // has_min
        0,                                      // has_max
        0,                                      // min_int
        0,                                      // max_int
        1,                                      // min_uint (at least 1 byte)
        0,                                      // max_uint
        "Maximum decompressed output bytes",    // help
    },
    // limits.max_block_bytes - Maximum block size during decode
    {
        "limits.max_block_bytes",           // key
        GCOMP_OPT_UINT64,                   // type
        1,                                  // has_default
        {.ui64 = LZ4_DEFAULT_BLOCK_SIZE},   // default_value
        1,                                  // has_min
        0,                                  // has_max
        0,                                  // min_int
        0,                                  // max_int
        1,                                  // min_uint
        0,                                  // max_uint
        "Maximum block size during decode", // help
    },
    // limits.max_memory_bytes - Maximum memory usage
    {
        "limits.max_memory_bytes",                     // key
        GCOMP_OPT_UINT64,                              // type
        1,                                             // has_default
        {.ui64 = LZ4_DEFAULT_MAX_MEMORY_BYTES},        // default_value
        1,                                             // has_min
        0,                                             // has_max
        0,                                             // min_int
        0,                                             // max_int
        1,                                             // min_uint
        0,                                             // max_uint
        "Maximum memory usage (buffers, hash tables)", // help
    },
    // limits.max_expansion_ratio - Decompression bomb protection
    {
        "limits.max_expansion_ratio",              // key
        GCOMP_OPT_UINT64,                          // type
        1,                                         // has_default
        {.ui64 = LZ4_DEFAULT_MAX_EXPANSION_RATIO}, // default_value
        1,                                         // has_min
        0,                                         // has_max
        0,                                         // min_int
        0,                                         // max_int
        1,                                         // min_uint
        0,                                         // max_uint
        "Maximum output/input ratio (decompression bomb protection)", // help
    },
};

static const char * const g_lz4_option_keys[] = {
    "lz4.block_size",
    "lz4.block_checksum",
    "lz4.content_checksum",
    "lz4.independent_blocks",
    "lz4.concat",
    "lz4.dictionary_id",
    "lz4.content_size",
    "limits.max_output_bytes",
    "limits.max_block_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_lz4_schema = {
    g_lz4_option_schemas,
    sizeof(g_lz4_option_schemas) / sizeof(g_lz4_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR, // LZ4 doesn't wrap another method
    g_lz4_option_keys,
};

static const gcomp_method_schema_t * lz4_get_schema(void) {
  return &g_lz4_schema;
}

//
// Option Validation
//

/**
 * @brief Validate that block_size is one of the allowed values.
 *
 * LZ4 frame format only allows specific block sizes:
 * - 64 KB (65536)
 * - 256 KB (262144)
 * - 1 MB (1048576)
 * - 4 MB (4194304)
 */
static bool lz4_validate_block_size(uint64_t size) {
  return size == LZ4_BLOCK_SIZE_64KB || size == LZ4_BLOCK_SIZE_256KB ||
      size == LZ4_BLOCK_SIZE_1MB || size == LZ4_BLOCK_SIZE_4MB;
}

//
// Encoder/Decoder Wrappers
//

static gcomp_status_t lz4_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return lz4_encoder_update(encoder, input, output);
}

static gcomp_status_t lz4_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return lz4_encoder_finish(encoder, output);
}

static gcomp_status_t lz4_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return lz4_decoder_update(decoder, input, output);
}

static gcomp_status_t lz4_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return lz4_decoder_finish(decoder, output);
}

//
// Encoder/Decoder Factory Functions
//

static gcomp_status_t lz4_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Validate block_size option if provided
  if (options) {
    uint64_t block_size_val;
    if (gcomp_options_get_uint64(options, "lz4.block_size", &block_size_val) ==
        GCOMP_OK) {
      if (!lz4_validate_block_size(block_size_val)) {
        return GCOMP_ERR_INVALID_ARG;
      }
    }
  }

  gcomp_status_t status = lz4_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = lz4_encoder_update_wrapper;
  (*encoder_out)->finish_fn = lz4_encoder_finish_wrapper;
  (*encoder_out)->reset_fn = lz4_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t lz4_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = lz4_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = lz4_decoder_update_wrapper;
  (*decoder_out)->finish_fn = lz4_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = lz4_decoder_reset;
  return GCOMP_OK;
}

static void lz4_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  lz4_encoder_destroy(encoder);
}

static void lz4_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  lz4_decoder_destroy(decoder);
}

//
// Method Descriptor
//

static const gcomp_method_t g_lz4_method = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "lz4",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = lz4_create_encoder,
    .create_decoder = lz4_create_decoder,
    .destroy_encoder = lz4_destroy_encoder_wrapper,
    .destroy_decoder = lz4_destroy_decoder_wrapper,
    .get_schema = lz4_get_schema,
};

//
// Public Registration Function
//

gcomp_status_t gcomp_method_lz4_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_lz4_method);
}

//
// Auto-Registration Hook
//
GCOMP_AUTOREG_METHOD(lz4, gcomp_method_lz4_register)
