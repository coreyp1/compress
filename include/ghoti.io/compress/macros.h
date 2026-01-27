/**
 * @file macros.h
 * @brief Cross-compiler macros and utilities for the Ghoti.io Compress library
 *
 * This header provides cross-compiler macros for common functionality
 * such as marking unused parameters, deprecated functions, and utility
 * macros for array operations.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_MACROS_H
#define GHOTI_IO_GCOMP_MACROS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cross-compiler macro for marking a function parameter as unused
 *
 * Use this macro to suppress compiler warnings about unused parameters.
 * Works with GCC, Clang, and MSVC.
 *
 * @param X The parameter name to mark as unused
 *
 * Example:
 * @code
 * void my_function(int GCOMP_MAYBE_UNUSED(param)) {
 *     // param is intentionally unused
 * }
 * @endcode
 */
#if defined(__GNUC__) || defined(__clang__)
#define GCOMP_MAYBE_UNUSED(X) __attribute__((unused)) X

#elif defined(_MSC_VER)
#define GCOMP_MAYBE_UNUSED(X) (void)(X)

#else
#define GCOMP_MAYBE_UNUSED(X) X

#endif

/**
 * @brief Cross-compiler macro for marking a function as deprecated
 *
 * Use this macro to mark functions that are deprecated and will be
 * removed in a future version. Works with GCC, Clang, and MSVC.
 *
 * Example:
 * @code
 * GCOMP_DEPRECATED
 * void old_function(void);
 * @endcode
 */
#if defined(__GNUC__) || defined(__clang__)
#define GCOMP_DEPRECATED __attribute__((deprecated))

#elif defined(_MSC_VER)
#define GCOMP_DEPRECATED __declspec(deprecated)

#else
#define GCOMP_DEPRECATED

#endif

/**
 * @brief API export macro for cross-platform library symbols
 *
 * Use this macro to mark functions that should be exported from the
 * shared library. Automatically handles Windows DLL export/import
 * and Unix symbol visibility.
 *
 * Example:
 * @code
 * GCOMP_API void public_function(void);
 * @endcode
 */
#ifdef __cplusplus
#define GCOMP_EXTERN extern "C"
#else
#define GCOMP_EXTERN
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef GCOMP_BUILD
#define GCOMP_API GCOMP_EXTERN __declspec(dllexport)
#else
#define GCOMP_API GCOMP_EXTERN __declspec(dllimport)
#endif
#else
#define GCOMP_API GCOMP_EXTERN __attribute__((visibility("default")))
#endif

/**
 * @brief Internal API export macro for testing
 *
 * This macro is used to export internal functions that are needed for testing
 * but should not be part of the public API. These functions are only exported
 * when GCOMP_TEST_BUILD is defined during library compilation.
 *
 * Example:
 * @code
 * GCOMP_INTERNAL_API void internal_function(void);
 * @endcode
 */
#ifdef GCOMP_TEST_BUILD
#define GCOMP_INTERNAL_API GCOMP_API
#else
#define GCOMP_INTERNAL_API
#endif

/**
 * @brief Macro for declaring a function to be run at library initialization
 *
 * Functions declared with this macro are automatically called when the library
 * is loaded, before main() is executed. This is useful for module initialization,
 * such as allocating global resources or setting up internal data structures.
 *
 * Works with GCC, Clang, MinGW, and MSVC. On unsupported compilers, this macro
 * is a no-op.
 *
 * @param function_name The name of the function to be called at startup
 *
 * Example:
 * @code
 * GCOMP_INIT_FUNCTION(my_module_constructor) {
 *     // Initialize module resources
 *     my_module_init();
 * }
 * @endcode
 *
 * @note The function must be declared as `static void function_name(void)`
 * @note Multiple init functions may be declared; execution order is not guaranteed
 */
#ifdef _MSC_VER  // If using Visual Studio

#include <windows.h>

// Define the startup macro for Visual Studio
#define GCOMP_INIT_FUNCTION(function_name) \
    __pragma(section(".CRT$XCU", read)) \
    __declspec(allocate(".CRT$XCU")) void (*function_name##_init)(void) = function_name; \
    static void function_name(void)

#elif defined(__GNUC__) || defined(__clang__) || defined(__MINGW32__) || defined(__MINGW64__)  // If using GCC/Clang/MinGW

// Define the startup macro for GCC/Clang/MinGW
#define GCOMP_INIT_FUNCTION(function_name) \
    __attribute__((constructor)) static void function_name(void)

#else  // Other compilers (add more cases as needed)

// Default no-op macro for unsupported compilers
#define GCOMP_INIT_FUNCTION(function_name)

#endif

/**
 * @brief Macro for declaring a function to be run at library shutdown
 *
 * Functions declared with this macro are automatically called when the library
 * is unloaded, after main() returns. This is useful for module cleanup,
 * such as freeing global resources or destroying internal data structures.
 *
 * Works with GCC, Clang, MinGW, and MSVC. On unsupported compilers, this macro
 * is a no-op.
 *
 * @param function_name The name of the function to be called at shutdown
 *
 * Example:
 * @code
 * GCOMP_CLEANUP_FUNCTION(my_module_destructor) {
 *     // Clean up module resources
 *     my_module_cleanup();
 * }
 * @endcode
 *
 * @note The function must be declared as `static void function_name(void)`
 * @note Multiple cleanup functions may be declared; execution order is not guaranteed
 */
#ifdef _MSC_VER  // If using Visual Studio

// Define the cleanup macro for Visual Studio
#define GCOMP_CLEANUP_FUNCTION(function_name) \
    __pragma(section(".CRT$XTU", read)) \
    __declspec(allocate(".CRT$XTU")) void (*function_name##_cleanup)(void) = function_name; \
    static void function_name(void)

#elif defined(__GNUC__) || defined(__clang__) || defined(__MINGW32__) || defined(__MINGW64__)  // If using GCC/Clang/MinGW

// Define the cleanup macro for GCC/Clang/MinGW
#define GCOMP_CLEANUP_FUNCTION(function_name) \
    __attribute__((destructor)) static void function_name(void)

#else  // Other compilers (add more cases as needed)

// Default no-op macro for unsupported compilers
#define GCOMP_CLEANUP_FUNCTION(function_name)

#endif

/**
 * @brief Compile-time array size helper
 *
 * Calculates the number of elements in a statically-allocated array.
 * This is a compile-time constant and safe to use in constant expressions.
 *
 * @param a The array (not a pointer)
 * @return The number of elements in the array
 *
 * Example:
 * @code
 * int arr[10];
 * size_t count = GCOMP_ARRAY_SIZE(arr);  // count = 10
 * @endcode
 */
#ifndef GCOMP_ARRAY_SIZE
#define GCOMP_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/**
 * @brief Bit manipulation macro
 *
 * Creates a bitmask with a single bit set at the specified position.
 *
 * @param x The bit position (0-based)
 * @return A bitmask with bit x set
 *
 * Example:
 * @code
 * unsigned int flags = GCOMP_BIT(3);  // flags = 0x08
 * @endcode
 */
#ifndef GCOMP_BIT
#define GCOMP_BIT(x) (1u << (x))
#endif

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_MACROS_H */
