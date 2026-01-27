/**
 * @file method.h
 *
 * Compression method interface and metadata for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_METHOD_H
#define GHOTI_IO_GCOMP_METHOD_H

#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forward declarations
 */
typedef struct gcomp_registry_s gcomp_registry_t;
typedef struct gcomp_options_s gcomp_options_t;
typedef struct gcomp_encoder_s gcomp_encoder_t;
typedef struct gcomp_decoder_s gcomp_decoder_t;

/**
 * @brief Compression method capabilities
 */
typedef enum {
  GCOMP_CAP_NONE = 0,
  GCOMP_CAP_ENCODE = 1 << 0,  /**< Method supports encoding */
  GCOMP_CAP_DECODE = 1 << 1,  /**< Method supports decoding */
} gcomp_capabilities_t;

/**
 * @brief Compression method vtable
 *
 * Each compression method provides an implementation of this vtable
 * to register with the registry.
 */
typedef struct {
  /**
   * @brief ABI version for forward compatibility
   */
  uint32_t abi_version;

  /**
   * @brief Size of this structure for forward compatibility
   */
  size_t size;

  /**
   * @brief Method name (e.g., "deflate", "gzip", "zstd")
   */
  const char *name;

  /**
   * @brief Method capabilities
   */
  gcomp_capabilities_t capabilities;

  /**
   * @brief Create an encoder instance
   *
   * @param registry The registry (for accessing wrapped methods)
   * @param options Configuration options
   * @param encoder_out Output parameter for the created encoder
   * @return Status code
   */
  gcomp_status_t (*create_encoder)(
      gcomp_registry_t *registry, gcomp_options_t *options,
      gcomp_encoder_t **encoder_out);

  /**
   * @brief Create a decoder instance
   *
   * @param registry The registry (for accessing wrapped methods)
   * @param options Configuration options
   * @param decoder_out Output parameter for the created decoder
   * @return Status code
   */
  gcomp_status_t (*create_decoder)(
      gcomp_registry_t *registry, gcomp_options_t *options,
      gcomp_decoder_t **decoder_out);

  /**
   * @brief Destroy an encoder instance
   *
   * @param encoder The encoder to destroy
   */
  void (*destroy_encoder)(gcomp_encoder_t *encoder);

  /**
   * @brief Destroy a decoder instance
   *
   * @param decoder The decoder to destroy
   */
  void (*destroy_decoder)(gcomp_decoder_t *decoder);
} gcomp_method_t;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_METHOD_H */
