# Auto-Registration

The Ghoti.io Compress library supports automatic registration of compression methods with the default registry. When the library is loaded (before `main()` executes), built-in methods like `deflate` are automatically registered and ready to use.

## Overview

Auto-registration eliminates the need for explicit initialization code in most applications:

```c
// Without auto-registration (explicit):
gcomp_method_deflate_register(gcomp_registry_default());

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "deflate", NULL, &enc);

// With auto-registration (automatic):
// deflate is already registered when library loads
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "deflate", NULL, &enc);
```

## How It Works

Auto-registration uses platform-specific mechanisms to run initialization code at library load time:

| Platform | Mechanism |
|----------|-----------|
| GCC/Clang | `__attribute__((constructor))` |
| MSVC | `.CRT$XCU` section (C runtime startup) |
| MinGW | `__attribute__((constructor))` |

On unsupported compilers, auto-registration is disabled and methods must be registered explicitly.

## Disabling Auto-Registration

Define `GCOMP_NO_AUTOREG` before including any compress headers to disable auto-registration:

```c
#define GCOMP_NO_AUTOREG
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>

int main(void) {
    // deflate is NOT auto-registered; must register explicitly
    gcomp_method_deflate_register(gcomp_registry_default());
    // ...
}
```

Reasons to disable auto-registration:

- **Testing:** Verify explicit registration behavior
- **Control:** Choose exactly which methods are available
- **Binary size:** Avoid linking unused method implementations
- **Determinism:** Ensure predictable initialization order

## Checking Auto-Registration Status

Use the `GCOMP_AUTOREG_ENABLED` macro to check at compile time:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>

int main(void) {
#if !GCOMP_AUTOREG_ENABLED
    // Auto-registration disabled; register manually
    gcomp_method_deflate_register(gcomp_registry_default());
#endif
    // deflate is now available regardless of auto-registration setting
    // ...
}
```

## Auto-Registered Methods

The following methods are auto-registered by default:

| Method | Header | Registration Function |
|--------|--------|----------------------|
| `deflate` | `<ghoti.io/compress/deflate.h>` | `gcomp_method_deflate_register()` |

## Custom Registries

Auto-registration only affects the **default registry** (returned by `gcomp_registry_default()`). Custom registries created with `gcomp_registry_create()` are always empty and require explicit registration:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);

// Custom registry is empty; must register explicitly
gcomp_method_deflate_register(custom);

// Now deflate is available in the custom registry
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(custom, "deflate", NULL, &enc);

gcomp_registry_destroy(custom);
```

## Thread Safety

Auto-registration runs before `main()` in a single-threaded context, so no synchronization is needed during registration. The default registry can be safely accessed from multiple threads after initialization.

## Error Handling

Auto-registration errors are silently ignored. If registration fails (e.g., due to memory allocation failure), the method will not be available. Use explicit registration with error checking for critical applications:

```c
gcomp_status_t status = gcomp_method_deflate_register(gcomp_registry_default());
if (status != GCOMP_OK) {
    fprintf(stderr, "Failed to register deflate: %s\n",
            gcomp_status_to_string(status));
    return 1;
}
```

Note that explicit registration is idempotent—calling it when the method is already registered (via auto-registration or a previous explicit call) succeeds and returns `GCOMP_OK`.

## Adding Auto-Registration to New Methods

Method implementers can opt-in to auto-registration using the `GCOMP_AUTOREG_METHOD` macro:

```c
#include "autoreg/autoreg_platform.h"

// Method registration function
gcomp_status_t gcomp_method_mymethod_register(gcomp_registry_t *registry) {
    if (!registry) return GCOMP_ERR_INVALID_ARG;
    return gcomp_registry_register(registry, &g_mymethod);
}

// Enable auto-registration with the default registry
GCOMP_AUTOREG_METHOD(mymethod, gcomp_method_mymethod_register)
```

The macro parameters are:

1. **method_name**: A unique identifier (used to generate internal function names)
2. **register_fn**: The registration function to call

See `src/methods/deflate/deflate_register.c` for a complete example.
