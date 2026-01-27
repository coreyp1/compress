/**
 * @file registry.h
 *
 * Compression method registry for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_REGISTRY_H
#define GHOTI_IO_GCOMP_REGISTRY_H

#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/errors.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forward declaration
 */
typedef struct gcomp_registry_s gcomp_registry_t;

/**
 * @brief Get the default (global) registry
 *
 * This returns a thread-safe singleton registry that is initialized
 * with all available methods (if auto-registration is enabled).
 *
 * @return Pointer to the default registry (never NULL)
 */
GCOMP_API gcomp_registry_t *gcomp_registry_default(void);

/**
 * @brief Create a new registry
 *
 * Creates an explicit registry that can be populated with specific
 * methods. The registry uses the provided allocator (if any) for
 * memory management.
 *
 * @param allocator Optional allocator (NULL for default)
 * @param registry_out Output parameter for the created registry
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_registry_create(void *allocator,
                          gcomp_registry_t **registry_out);

/**
 * @brief Destroy a registry
 *
 * Destroys an explicit registry created with gcomp_registry_create().
 * The default registry cannot be destroyed.
 *
 * @param registry The registry to destroy
 */
GCOMP_API void
gcomp_registry_destroy(gcomp_registry_t *registry);

/**
 * @brief Register a compression method
 *
 * Registers a compression method with the registry. The method can
 * then be found by name using gcomp_registry_find().
 *
 * @param registry The registry
 * @param method The method to register
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_registry_register(gcomp_registry_t *registry,
                             const gcomp_method_t *method);

/**
 * @brief Find a compression method by name
 *
 * @param registry The registry
 * @param name The method name (e.g., "deflate", "gzip", "zstd")
 * @return Pointer to the method, or NULL if not found
 */
GCOMP_API const gcomp_method_t *
gcomp_registry_find(gcomp_registry_t *registry, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_REGISTRY_H */
