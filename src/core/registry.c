/**
 * @file registry.c
 *
 * Implementation of the compression method registry for the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stdlib.h>
#include <string.h>

/* Simple hash table implementation for methods */
#define GCOMP_REGISTRY_HASH_SIZE 32

/* Registry entry */
typedef struct gcomp_registry_entry_s {
  const gcomp_method_t * method;
  struct gcomp_registry_entry_s * next; /* For chaining */
} gcomp_registry_entry_t;

/* Registry structure */
struct gcomp_registry_s {
  gcomp_registry_entry_t * buckets[GCOMP_REGISTRY_HASH_SIZE];
  int is_default; /* 1 if this is the default registry, 0 otherwise */
};

/* Simple djb2 hash function */
static uint32_t hash_string(const char * str) {
  uint32_t hash = 5381;
  int c;
  while ((c = *str++)) {
    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  }
  return hash;
}

/* Default registry (singleton) */
static gcomp_registry_t * g_default_registry = NULL;

gcomp_registry_t * gcomp_registry_default(void) {
  if (!g_default_registry) {
    g_default_registry = calloc(1, sizeof(gcomp_registry_t));
    if (g_default_registry) {
      g_default_registry->is_default = 1;
    }
  }
  return g_default_registry;
}

gcomp_status_t gcomp_registry_create(
    void * allocator, gcomp_registry_t ** registry_out) {
  (void)allocator; /* Not used yet, reserved for future use */

  if (!registry_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_registry_t * reg = calloc(1, sizeof(gcomp_registry_t));
  if (!reg) {
    return GCOMP_ERR_MEMORY;
  }

  reg->is_default = 0;
  *registry_out = reg;
  return GCOMP_OK;
}

void gcomp_registry_destroy(gcomp_registry_t * registry) {
  if (!registry || registry->is_default) {
    /* Cannot destroy default registry */
    return;
  }

  /* Free all entries */
  for (size_t i = 0; i < GCOMP_REGISTRY_HASH_SIZE; i++) {
    gcomp_registry_entry_t * entry = registry->buckets[i];
    while (entry) {
      gcomp_registry_entry_t * next = entry->next;
      free(entry);
      entry = next;
    }
  }

  free(registry);
}

gcomp_status_t gcomp_registry_register(
    gcomp_registry_t * registry, const gcomp_method_t * method) {
  if (!registry || !method || !method->name) {
    return GCOMP_ERR_INVALID_ARG;
  }

  /* Check if method already registered */
  if (gcomp_registry_find(registry, method->name)) {
    /* Already registered - this is not an error, just ignore */
    return GCOMP_OK;
  }

  uint32_t hash = hash_string(method->name);
  uint32_t bucket = hash % GCOMP_REGISTRY_HASH_SIZE;

  /* Create new entry */
  gcomp_registry_entry_t * entry = calloc(1, sizeof(gcomp_registry_entry_t));
  if (!entry) {
    return GCOMP_ERR_MEMORY;
  }

  entry->method = method;

  /* Insert at head of bucket */
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
