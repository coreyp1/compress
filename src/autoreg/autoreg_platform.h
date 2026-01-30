/**
 * @file autoreg_platform.h
 *
 * Platform-specific support for automatic method registration in the
 * Ghoti.io Compress library. Methods can opt-in to auto-registration
 * by using the GCOMP_AUTOREG_METHOD macro.
 *
 * ## Design Rationale
 *
 * Auto-registration provides a zero-configuration experience: applications
 * can use compression methods immediately without explicit initialization.
 * This follows the principle of "sensible defaults" - most users want all
 * built-in methods available.
 *
 * The implementation uses platform-specific constructor mechanisms:
 *
 * - **GCC/Clang/MinGW**: `__attribute__((constructor))` marks a function
 *   to run at shared library load time (or program start for static linking).
 *   This is part of the ELF/Mach-O ABI and is widely portable.
 *
 * - **MSVC**: The `.CRT$XCU` section contains function pointers that the
 *   C runtime calls during startup. The "X" in XCU places it after C library
 *   init ("I") but before user code ("U" = user).
 *
 * ## Trade-offs
 *
 * Auto-registration has some implications:
 *
 * 1. **Link-time behavior**: Methods are registered when linked, even if
 *    unused. This is intentional - it ensures consistent behavior regardless
 *    of code paths that might or might not reference the method.
 *
 * 2. **Error handling**: Registration errors are silently ignored because
 *    there's no caller to report to. This is acceptable for well-tested
 *    methods but applications needing error handling should use explicit
 *    registration.
 *
 * 3. **Initialization order**: Multiple auto-registered methods have
 *    unspecified registration order. This is fine because methods are
 *    independent - there are no dependencies between them.
 *
 * ## Alternatives Considered
 *
 * - **Lazy registration**: Register on first use. Rejected because it adds
 *   complexity and thread-safety concerns to the hot path.
 *
 * - **Explicit-only**: Require explicit registration for all methods.
 *   Rejected because it's unnecessary boilerplate for most applications.
 *
 * - **Registry-per-method**: Each method has its own registry. Rejected
 *   because it complicates the API and doesn't match user mental model.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_AUTOREG_PLATFORM_H
#define GHOTI_IO_GCOMP_AUTOREG_PLATFORM_H

#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Macro to control auto-registration at compile time
 *
 * Define GCOMP_NO_AUTOREG before including this header to disable
 * auto-registration. This is useful for:
 * - Testing explicit registration behavior
 * - Applications that want full control over which methods are available
 * - Reducing binary size by not linking unused methods
 *
 * By default (when GCOMP_NO_AUTOREG is NOT defined), methods that use
 * GCOMP_AUTOREG_METHOD will automatically register themselves with the
 * default registry when the library is loaded.
 */

/**
 * @brief Auto-register a compression method with the default registry
 *
 * This macro declares an auto-registration function that runs at library
 * load time (before main()). The function calls the method's registration
 * function with the default registry.
 *
 * The macro works on:
 * - GCC/Clang: Uses __attribute__((constructor))
 * - MSVC: Uses .CRT$XCU section for C runtime startup
 * - MinGW: Uses __attribute__((constructor))
 *
 * On unsupported compilers, this macro is a no-op and methods must be
 * registered explicitly.
 *
 * @param method_name A unique identifier for the method (used to generate
 *                    unique function names). Should match the method's name.
 * @param register_fn The registration function to call, e.g.,
 *                    gcomp_method_deflate_register
 *
 * Example usage in a method's registration file:
 * @code
 * #include "autoreg/autoreg_platform.h"
 *
 * gcomp_status_t gcomp_method_deflate_register(gcomp_registry_t * registry) {
 *     // ... registration implementation ...
 * }
 *
 * // Auto-register deflate when library loads
 * GCOMP_AUTOREG_METHOD(deflate, gcomp_method_deflate_register)
 * @endcode
 *
 * @note If GCOMP_NO_AUTOREG is defined, this macro expands to nothing.
 * @note Auto-registration errors are silently ignored. Use explicit
 *       registration if you need error handling.
 * @note Multiple methods can use this macro; registration order is not
 *       guaranteed between different methods.
 */
#ifndef GCOMP_NO_AUTOREG

//
// Implementation Notes:
//
// The macro generates a uniquely-named static function using token pasting
// (gcomp_autoreg_##method_name). GCOMP_INIT_FUNCTION wraps this with the
// platform-specific constructor attribute.
//
// The (void) cast silences "unused return value" warnings. Registration
// errors are intentionally ignored here - if registration fails (e.g., OOM),
// the method simply won't be available. Applications needing error handling
// should use explicit registration.
//
// Example expansion for deflate:
//
//   GCOMP_AUTOREG_METHOD(deflate, gcomp_method_deflate_register)
//
// expands to (on GCC/Clang):
//
//   __attribute__((constructor)) static void gcomp_autoreg_deflate(void) {
//       (void)gcomp_method_deflate_register(gcomp_registry_default());
//   }
//

#define GCOMP_AUTOREG_METHOD(method_name, register_fn)                         \
  GCOMP_INIT_FUNCTION(gcomp_autoreg_##method_name) {                           \
    (void)register_fn(gcomp_registry_default());                               \
  }

#else // GCOMP_NO_AUTOREG is defined

#define GCOMP_AUTOREG_METHOD(method_name, register_fn) // no-op

#endif // GCOMP_NO_AUTOREG

/**
 * @brief Check if auto-registration is enabled at compile time
 *
 * This macro evaluates to 1 if auto-registration is enabled, 0 otherwise.
 * Useful for conditional code that depends on auto-registration behavior.
 *
 * Example:
 * @code
 * #if GCOMP_AUTOREG_ENABLED
 *     // deflate is already registered
 * #else
 *     gcomp_method_deflate_register(gcomp_registry_default());
 * #endif
 * @endcode
 */
#ifndef GCOMP_NO_AUTOREG
#define GCOMP_AUTOREG_ENABLED 1
#else
#define GCOMP_AUTOREG_ENABLED 0
#endif

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_AUTOREG_PLATFORM_H
