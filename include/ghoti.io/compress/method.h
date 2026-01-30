/**
 * @file method.h
 *
 * Compression method interface and metadata for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_METHOD_H
#define GHOTI_IO_GCOMP_METHOD_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/options.h>
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
  GCOMP_CAP_ENCODE = 1 << 0, ///< Method supports encoding
  GCOMP_CAP_DECODE = 1 << 1, ///< Method supports decoding
} gcomp_capabilities_t;

/**
 * @brief Option schema descriptor for a single key.
 *
 * Methods expose their supported options via arrays of this structure.
 */
typedef struct gcomp_option_schema_s {
  /**
   * @brief Option key name (e.g., "deflate.level").
   */
  const char * key;

  /**
   * @brief Option value type.
   */
  gcomp_option_type_t type;

  /**
   * @brief Whether a default value is provided.
   */
  int has_default;

  /**
   * @brief Default value (valid only if @ref has_default is non-zero).
   */
  union {
    int64_t i64;
    uint64_t ui64;
    int b;
    const char * str;
    struct {
      const void * data;
      size_t size;
    } bytes;
    double f;
  } default_value;

  /**
   * @brief Whether minimum/maximum constraints are present.
   *
   * These currently apply only to integer and unsigned integer types.
   */
  int has_min;
  int has_max;

  /**
   * @brief Integer constraints (for ::GCOMP_OPT_INT64).
   */
  int64_t min_int;
  int64_t max_int;

  /**
   * @brief Unsigned integer constraints (for ::GCOMP_OPT_UINT64).
   */
  uint64_t min_uint;
  uint64_t max_uint;

  /**
   * @brief Optional help text (may be NULL).
   */
  const char * help;
} gcomp_option_schema_t;

/**
 * @brief Schema descriptor for all options supported by a method.
 */
typedef struct gcomp_method_schema_s {
  /**
   * @brief Array of option schemas.
   */
  const gcomp_option_schema_t * options;

  /**
   * @brief Number of entries in @ref options.
   */
  size_t num_options;

  /**
   * @brief Policy for handling unknown keys.
   */
  gcomp_unknown_key_policy_t unknown_key_policy;

  /**
   * @brief Optional array of option key strings.
   *
   * If non-NULL, this must be an array of @ref num_options pointers, each
   * matching the corresponding @ref gcomp_option_schema_s::key. If NULL,
   * callers can instead iterate @ref options directly.
   */
  const char * const * keys;
} gcomp_method_schema_t;

/**
 * @brief Forward declaration and typedef for the method vtable structure.
 */
typedef struct gcomp_method_s gcomp_method_t;

/**
 * @brief Compression method vtable
 *
 * Each compression method provides an implementation of this vtable
 * to register with the registry.
 */
struct gcomp_method_s {
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
  const char * name;

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
  gcomp_status_t (*create_encoder)(gcomp_registry_t * registry,
      gcomp_options_t * options, gcomp_encoder_t ** encoder_out);

  /**
   * @brief Create a decoder instance
   *
   * @param registry The registry (for accessing wrapped methods)
   * @param options Configuration options
   * @param decoder_out Output parameter for the created decoder
   * @return Status code
   */
  gcomp_status_t (*create_decoder)(gcomp_registry_t * registry,
      gcomp_options_t * options, gcomp_decoder_t ** decoder_out);

  /**
   * @brief Destroy an encoder instance
   *
   * @param encoder The encoder to destroy
   */
  void (*destroy_encoder)(gcomp_encoder_t * encoder);

  /**
   * @brief Destroy a decoder instance
   *
   * @param decoder The decoder to destroy
   */
  void (*destroy_decoder)(gcomp_decoder_t * decoder);

  /**
   * @brief Retrieve the option schema for this method.
   *
   * Methods that support option introspection must implement this hook
   * and return a pointer to a static @ref gcomp_method_schema_t instance.
   * Methods that do not support introspection may leave this as @c NULL.
   *
   * @return Pointer to the method's schema, or NULL if not available.
   */
  const gcomp_method_schema_t * (*get_schema)(void);
};

/**
 * @brief List all option keys supported by a method.
 *
 * The returned @p keys_out pointer (if non-NULL) is owned by the method and
 * remains valid for the lifetime of the method descriptor; it must not be
 * freed by the caller.
 *
 * @param method The method whose option keys should be listed.
 * @param keys_out Output parameter for the array of key strings (may be NULL).
 * @param count_out Output parameter for the number of keys.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_method_get_option_keys(
    const gcomp_method_t * method, const char * const ** keys_out,
    size_t * count_out);

/**
 * @brief Get the schema descriptor for a specific option key.
 *
 * @param method The method whose schema should be queried.
 * @param key The option key to look up.
 * @param schema_out Output parameter for the schema descriptor.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_method_get_option_schema(
    const gcomp_method_t * method, const char * key,
    const gcomp_option_schema_t ** schema_out);

/**
 * @brief Get the full option schema for a method.
 *
 * @param method The method whose schema should be retrieved.
 * @param schema_out Output parameter for the schema descriptor.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_method_get_all_schemas(
    const gcomp_method_t * method, const gcomp_method_schema_t ** schema_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_METHOD_H
