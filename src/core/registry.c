/**
 * @file registry.c
 *
 * Implementation of the compression method registry for the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "alloc_internal.h"
#include "registry_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <string.h>

// Simple hash table implementation for methods
#define GCOMP_REGISTRY_HASH_SIZE 32

// Registry entry
typedef struct gcomp_registry_entry_s {
  const gcomp_method_t * method;
  struct gcomp_registry_entry_s * next; // For chaining
} gcomp_registry_entry_t;

// Registry structure
struct gcomp_registry_s {
  gcomp_registry_entry_t * buckets[GCOMP_REGISTRY_HASH_SIZE];
  int is_default; // 1 if this is the default registry, 0 otherwise
  const gcomp_allocator_t * allocator;
};

// Simple djb2 hash function
static uint32_t hash_string(const char * str) {
  uint32_t hash = 5381;
  int c;
  while ((c = *str++)) {
    hash = ((hash << 5) + hash) + c; // hash * 33 + c
  }
  return hash;
}

// Default registry (singleton)
static gcomp_registry_t * g_default_registry = NULL;

gcomp_registry_t * gcomp_registry_default(void) {
  if (!g_default_registry) {
    const gcomp_allocator_t * alloc = gcomp_allocator_default();
    g_default_registry = gcomp_calloc(alloc, 1, sizeof(gcomp_registry_t));
    if (g_default_registry) {
      g_default_registry->is_default = 1;
      g_default_registry->allocator = alloc;
    }
  }
  return g_default_registry;
}

gcomp_status_t gcomp_registry_create(
    void * allocator, gcomp_registry_t ** registry_out) {
  if (!registry_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc =
      gcomp_alloc_or_default((const gcomp_allocator_t *)allocator);

  gcomp_registry_t * reg = gcomp_calloc(alloc, 1, sizeof(gcomp_registry_t));
  if (!reg) {
    return GCOMP_ERR_MEMORY;
  }

  reg->is_default = 0;
  reg->allocator = alloc;
  *registry_out = reg;
  return GCOMP_OK;
}

void gcomp_registry_destroy(gcomp_registry_t * registry) {
  if (!registry || registry->is_default) {
    // Cannot destroy default registry
    return;
  }

  const gcomp_allocator_t * alloc = gcomp_alloc_or_default(registry->allocator);

  // Free all entries
  for (size_t i = 0; i < GCOMP_REGISTRY_HASH_SIZE; i++) {
    gcomp_registry_entry_t * entry = registry->buckets[i];
    while (entry) {
      gcomp_registry_entry_t * next = entry->next;
      gcomp_free(alloc, entry);
      entry = next;
    }
  }

  gcomp_free(alloc, registry);
}

gcomp_status_t gcomp_registry_register(
    gcomp_registry_t * registry, const gcomp_method_t * method) {
  if (!registry || !method || !method->name) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Check if method already registered
  if (gcomp_registry_find(registry, method->name)) {
    // Already registered - this is not an error, just ignore
    return GCOMP_OK;
  }

  uint32_t hash = hash_string(method->name);
  uint32_t bucket = hash % GCOMP_REGISTRY_HASH_SIZE;

  // Create new entry
  const gcomp_allocator_t * alloc = gcomp_alloc_or_default(registry->allocator);
  gcomp_registry_entry_t * entry =
      gcomp_calloc(alloc, 1, sizeof(gcomp_registry_entry_t));
  if (!entry) {
    return GCOMP_ERR_MEMORY;
  }

  entry->method = method;

  // Insert at head of bucket
  entry->next = registry->buckets[bucket];
  registry->buckets[bucket] = entry;

  return GCOMP_OK;
}

const gcomp_method_t * gcomp_registry_find(
    gcomp_registry_t * registry, const char * name) {
  if (!registry || !name) {
    return NULL;
  }

  uint32_t hash = hash_string(name);
  uint32_t bucket = hash % GCOMP_REGISTRY_HASH_SIZE;

  gcomp_registry_entry_t * entry = registry->buckets[bucket];
  while (entry) {
    if (strcmp(entry->method->name, name) == 0) {
      return entry->method;
    }
    entry = entry->next;
  }

  return NULL;
}

const gcomp_allocator_t * gcomp_registry_get_allocator(
    const gcomp_registry_t * registry) {
  if (!registry) {
    return gcomp_allocator_default();
  }
  return gcomp_alloc_or_default(registry->allocator);
}

// Cleanup function to free default registry at library shutdown
GCOMP_CLEANUP_FUNCTION(gcomp_registry_cleanup) {
  if (!g_default_registry) {
    return;
  }

  const gcomp_allocator_t * alloc =
      gcomp_alloc_or_default(g_default_registry->allocator);

  // Free all entries
  for (size_t i = 0; i < GCOMP_REGISTRY_HASH_SIZE; i++) {
    gcomp_registry_entry_t * entry = g_default_registry->buckets[i];
    while (entry) {
      gcomp_registry_entry_t * next = entry->next;
      gcomp_free(alloc, entry);
      entry = next;
    }
    g_default_registry->buckets[i] = NULL;
  }

  // Free the default registry itself
  gcomp_free(alloc, g_default_registry);
  g_default_registry = NULL;
}
