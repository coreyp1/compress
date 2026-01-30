/**
 * @file options.c
 *
 * Implementation of the key/value option system for the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "alloc_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <string.h>

// Simple hash table implementation for options
#define GCOMP_OPTIONS_HASH_SIZE 64

// Hash table entry
typedef struct gcomp_option_entry_s {
  char * key;
  gcomp_option_type_t type;
  union {
    int64_t i64;
    uint64_t ui64;
    int b;
    char * str;
    struct {
      void * data;
      size_t size;
    } bytes;
    double f;
  } value;
  struct gcomp_option_entry_s * next; // For chaining
} gcomp_option_entry_t;

// Options structure
struct gcomp_options_s {
  gcomp_option_entry_t * buckets[GCOMP_OPTIONS_HASH_SIZE];
  int frozen; // 1 if frozen (immutable), 0 otherwise
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

// Find an entry in the hash table
static gcomp_option_entry_t * find_entry(
    gcomp_options_t * opts, const char * key) {
  if (!opts || !key) {
    return NULL;
  }

  uint32_t hash = hash_string(key);
  uint32_t bucket = hash % GCOMP_OPTIONS_HASH_SIZE;

  gcomp_option_entry_t * entry = opts->buckets[bucket];
  while (entry) {
    if (strcmp(entry->key, key) == 0) {
      return entry;
    }
    entry = entry->next;
  }

  return NULL;
}

// Create a new entry
static gcomp_option_entry_t * create_entry(
    const gcomp_allocator_t * allocator, const char * key) {
  if (!key) {
    return NULL;
  }

  gcomp_option_entry_t * entry =
      gcomp_calloc(allocator, 1, sizeof(gcomp_option_entry_t));
  if (!entry) {
    return NULL;
  }

  entry->key = gcomp_strdup(allocator, key);
  if (!entry->key) {
    gcomp_free(allocator, entry);
    return NULL;
  }

  return entry;
}

// Free an entry and its value
static void free_entry(
    const gcomp_allocator_t * allocator, gcomp_option_entry_t * entry) {
  if (!entry) {
    return;
  }

  gcomp_free(allocator, entry->key);

  if (entry->type == GCOMP_OPT_STRING && entry->value.str) {
    gcomp_free(allocator, entry->value.str);
  }
  else if (entry->type == GCOMP_OPT_BYTES && entry->value.bytes.data) {
    gcomp_free(allocator, entry->value.bytes.data);
  }

  gcomp_free(allocator, entry);
}

// Set an entry in the hash table
static gcomp_status_t set_entry(gcomp_options_t * opts, const char * key,
    gcomp_option_type_t type, const void * value_ptr) {
  if (!opts || !key) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (opts->frozen) {
    return GCOMP_ERR_INVALID_ARG; // Cannot modify frozen options
  }

  const gcomp_allocator_t * alloc = gcomp_alloc_or_default(opts->allocator);

  uint32_t hash = hash_string(key);
  uint32_t bucket = hash % GCOMP_OPTIONS_HASH_SIZE;

  // Check if entry exists
  gcomp_option_entry_t * entry = find_entry(opts, key);

  if (entry) {
    // Update existing entry - free old value if needed
    if (entry->type == GCOMP_OPT_STRING && entry->value.str) {
      gcomp_free(alloc, entry->value.str);
      entry->value.str = NULL;
    }
    else if (entry->type == GCOMP_OPT_BYTES && entry->value.bytes.data) {
      gcomp_free(alloc, entry->value.bytes.data);
      entry->value.bytes.data = NULL;
      entry->value.bytes.size = 0;
    }
  }
  else {
    // Create new entry
    entry = create_entry(alloc, key);
    if (!entry) {
      return GCOMP_ERR_MEMORY;
    }

    // Insert at head of bucket
    entry->next = opts->buckets[bucket];
    opts->buckets[bucket] = entry;
  }

  // Set the value
  entry->type = type;
  switch (type) {
  case GCOMP_OPT_INT64:
    entry->value.i64 = *(const int64_t *)value_ptr;
    break;
  case GCOMP_OPT_UINT64:
    entry->value.ui64 = *(const uint64_t *)value_ptr;
    break;
  case GCOMP_OPT_BOOL:
    entry->value.b = *(const int *)value_ptr;
    break;
  case GCOMP_OPT_STRING: {
    const char * str = *(const char **)value_ptr;
    entry->value.str = gcomp_strdup(alloc, str);
    if (!entry->value.str) {
      return GCOMP_ERR_MEMORY;
    }
    break;
  }
  case GCOMP_OPT_BYTES: {
    const struct {
      const void * data;
      size_t size;
    } * bytes = value_ptr;
    entry->value.bytes.data = gcomp_malloc(alloc, bytes->size);
    if (!entry->value.bytes.data) {
      return GCOMP_ERR_MEMORY;
    }
    memcpy(entry->value.bytes.data, bytes->data, bytes->size);
    entry->value.bytes.size = bytes->size;
    break;
  }
  case GCOMP_OPT_FLOAT:
    entry->value.f = *(const double *)value_ptr;
    break;
  default:
    return GCOMP_ERR_INVALID_ARG;
  }

  return GCOMP_OK;
}

gcomp_status_t gcomp_options_create_with_allocator(
    const gcomp_allocator_t * allocator, gcomp_options_t ** options_out) {
  if (!options_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_alloc_or_default(allocator);
  gcomp_options_t * opts = gcomp_calloc(alloc, 1, sizeof(gcomp_options_t));
  if (!opts) {
    return GCOMP_ERR_MEMORY;
  }

  opts->allocator = alloc;
  *options_out = opts;
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_create(gcomp_options_t ** options_out) {
  return gcomp_options_create_with_allocator(NULL, options_out);
}

void gcomp_options_destroy(gcomp_options_t * options) {
  if (!options) {
    return;
  }

  const gcomp_allocator_t * alloc = gcomp_alloc_or_default(options->allocator);

  // Free all entries
  for (size_t i = 0; i < GCOMP_OPTIONS_HASH_SIZE; i++) {
    gcomp_option_entry_t * entry = options->buckets[i];
    while (entry) {
      gcomp_option_entry_t * next = entry->next;
      free_entry(alloc, entry);
      entry = next;
    }
  }

  gcomp_free(alloc, options);
}

gcomp_status_t gcomp_options_clone(
    const gcomp_options_t * options, gcomp_options_t ** cloned_out) {
  if (!options || !cloned_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_alloc_or_default(options->allocator);
  gcomp_options_t * cloned = NULL;
  gcomp_status_t status = gcomp_options_create_with_allocator(alloc, &cloned);
  if (status != GCOMP_OK) {
    return status;
  }

  // Copy all entries
  for (size_t i = 0; i < GCOMP_OPTIONS_HASH_SIZE; i++) {
    gcomp_option_entry_t * entry = options->buckets[i];
    while (entry) {
      switch (entry->type) {
      case GCOMP_OPT_INT64:
        status = gcomp_options_set_int64(cloned, entry->key, entry->value.i64);
        break;
      case GCOMP_OPT_UINT64:
        status =
            gcomp_options_set_uint64(cloned, entry->key, entry->value.ui64);
        break;
      case GCOMP_OPT_BOOL:
        status = gcomp_options_set_bool(cloned, entry->key, entry->value.b);
        break;
      case GCOMP_OPT_STRING:
        status = gcomp_options_set_string(cloned, entry->key, entry->value.str);
        break;
      case GCOMP_OPT_BYTES:
        status = gcomp_options_set_bytes(cloned, entry->key,
            entry->value.bytes.data, entry->value.bytes.size);
        break;
      case GCOMP_OPT_FLOAT:
        // Not yet implemented in public API
        status = GCOMP_ERR_UNSUPPORTED;
        break;
      default:
        status = GCOMP_ERR_INTERNAL;
        break;
      }

      if (status != GCOMP_OK) {
        gcomp_options_destroy(cloned);
        return status;
      }

      entry = entry->next;
    }
  }

  cloned->frozen = options->frozen;
  *cloned_out = cloned;
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_set_int64(
    gcomp_options_t * options, const char * key, int64_t value) {
  return set_entry(options, key, GCOMP_OPT_INT64, &value);
}

gcomp_status_t gcomp_options_set_uint64(
    gcomp_options_t * options, const char * key, uint64_t value) {
  return set_entry(options, key, GCOMP_OPT_UINT64, &value);
}

gcomp_status_t gcomp_options_set_bool(
    gcomp_options_t * options, const char * key, int value) {
  return set_entry(options, key, GCOMP_OPT_BOOL, &value);
}

gcomp_status_t gcomp_options_set_string(
    gcomp_options_t * options, const char * key, const char * value) {
  if (!value) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return set_entry(options, key, GCOMP_OPT_STRING, &value);
}

gcomp_status_t gcomp_options_set_bytes(gcomp_options_t * options,
    const char * key, const void * data, size_t size) {
  if (!data && size > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }
  struct {
    const void * data;
    size_t size;
  } bytes = {data, size};
  return set_entry(options, key, GCOMP_OPT_BYTES, &bytes);
}

gcomp_status_t gcomp_options_get_int64(
    const gcomp_options_t * options, const char * key, int64_t * value_out) {
  if (!options || !key || !value_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_option_entry_t * entry = find_entry((gcomp_options_t *)options, key);
  if (!entry || entry->type != GCOMP_OPT_INT64) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *value_out = entry->value.i64;
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_get_uint64(
    const gcomp_options_t * options, const char * key, uint64_t * value_out) {
  if (!options || !key || !value_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_option_entry_t * entry = find_entry((gcomp_options_t *)options, key);
  if (!entry || entry->type != GCOMP_OPT_UINT64) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *value_out = entry->value.ui64;
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_get_bool(
    const gcomp_options_t * options, const char * key, int * value_out) {
  if (!options || !key || !value_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_option_entry_t * entry = find_entry((gcomp_options_t *)options, key);
  if (!entry || entry->type != GCOMP_OPT_BOOL) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *value_out = entry->value.b;
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_get_string(const gcomp_options_t * options,
    const char * key, const char ** value_out) {
  if (!options || !key || !value_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_option_entry_t * entry = find_entry((gcomp_options_t *)options, key);
  if (!entry || entry->type != GCOMP_OPT_STRING) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *value_out = entry->value.str;
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_get_bytes(const gcomp_options_t * options,
    const char * key, const void ** data_out, size_t * size_out) {
  if (!options || !key || !data_out || !size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_option_entry_t * entry = find_entry((gcomp_options_t *)options, key);
  if (!entry || entry->type != GCOMP_OPT_BYTES) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *data_out = entry->value.bytes.data;
  *size_out = entry->value.bytes.size;
  return GCOMP_OK;
}

gcomp_status_t gcomp_options_freeze(gcomp_options_t * options) {
  if (!options) {
    return GCOMP_ERR_INVALID_ARG;
  }

  options->frozen = 1;
  return GCOMP_OK;
}

GCOMP_API gcomp_status_t gcomp_options_validate(
    const gcomp_options_t * options, const struct gcomp_method_s * method) {
  const gcomp_method_t * typed_method = (const gcomp_method_t *)method;

  if (!typed_method) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // No options means nothing to validate.
  if (!options) {
    return GCOMP_OK;
  }

  const gcomp_method_schema_t * schema = NULL;
  gcomp_status_t status = gcomp_method_get_all_schemas(typed_method, &schema);
  if (status != GCOMP_OK) {
    return status;
  }

  if (!schema) {
    return GCOMP_ERR_INTERNAL;
  }

  gcomp_unknown_key_policy_t policy = schema->unknown_key_policy;
  if (policy != GCOMP_UNKNOWN_KEY_ERROR && policy != GCOMP_UNKNOWN_KEY_IGNORE) {
    // Defensive default if schema provides an unknown policy.
    policy = GCOMP_UNKNOWN_KEY_ERROR;
  }

  // Iterate through all stored options and validate them.
  for (size_t i = 0; i < GCOMP_OPTIONS_HASH_SIZE; ++i) {
    gcomp_option_entry_t * entry = options->buckets[i];
    while (entry) {
      const gcomp_option_schema_t * opt_schema = NULL;
      status =
          gcomp_method_get_option_schema(typed_method, entry->key, &opt_schema);

      if (status != GCOMP_OK) {
        // If the key is unknown and the policy is IGNORE, skip it.
        if (status == GCOMP_ERR_INVALID_ARG &&
            policy == GCOMP_UNKNOWN_KEY_IGNORE) {
          entry = entry->next;
          continue;
        }
        return status;
      }

      if (!opt_schema) {
        return GCOMP_ERR_INTERNAL;
      }

      // Type must match the schema.
      if (opt_schema->type != entry->type) {
        return GCOMP_ERR_INVALID_ARG;
      }

      // Range checks for integer and unsigned integer options.
      if (entry->type == GCOMP_OPT_INT64) {
        if (opt_schema->has_min && entry->value.i64 < opt_schema->min_int) {
          return GCOMP_ERR_INVALID_ARG;
        }
        if (opt_schema->has_max && entry->value.i64 > opt_schema->max_int) {
          return GCOMP_ERR_INVALID_ARG;
        }
      }
      else if (entry->type == GCOMP_OPT_UINT64) {
        if (opt_schema->has_min && entry->value.ui64 < opt_schema->min_uint) {
          return GCOMP_ERR_INVALID_ARG;
        }
        if (opt_schema->has_max && entry->value.ui64 > opt_schema->max_uint) {
          return GCOMP_ERR_INVALID_ARG;
        }
      }

      entry = entry->next;
    }
  }

  return GCOMP_OK;
}

GCOMP_API gcomp_status_t gcomp_options_validate_key(
    const gcomp_options_t * options, const struct gcomp_method_s * method,
    const char * key) {
  const gcomp_method_t * typed_method = (const gcomp_method_t *)method;

  if (!options || !typed_method || !key) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_option_entry_t * entry = find_entry((gcomp_options_t *)options, key);
  if (!entry) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_option_schema_t * opt_schema = NULL;
  gcomp_status_t status =
      gcomp_method_get_option_schema(typed_method, key, &opt_schema);
  if (status != GCOMP_OK) {
    return status;
  }

  if (!opt_schema) {
    return GCOMP_ERR_INTERNAL;
  }

  if (opt_schema->type != entry->type) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (entry->type == GCOMP_OPT_INT64) {
    if (opt_schema->has_min && entry->value.i64 < opt_schema->min_int) {
      return GCOMP_ERR_INVALID_ARG;
    }
    if (opt_schema->has_max && entry->value.i64 > opt_schema->max_int) {
      return GCOMP_ERR_INVALID_ARG;
    }
  }
  else if (entry->type == GCOMP_OPT_UINT64) {
    if (opt_schema->has_min && entry->value.ui64 < opt_schema->min_uint) {
      return GCOMP_ERR_INVALID_ARG;
    }
    if (opt_schema->has_max && entry->value.ui64 > opt_schema->max_uint) {
      return GCOMP_ERR_INVALID_ARG;
    }
  }

  return GCOMP_OK;
}
