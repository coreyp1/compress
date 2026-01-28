/**
 * @file errors.c
 *
 * Error helpers for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/errors.h>

const char * gcomp_status_to_string(gcomp_status_t status) {
  switch (status) {
  case GCOMP_OK:
    return "GCOMP_OK";
  case GCOMP_ERR_INVALID_ARG:
    return "GCOMP_ERR_INVALID_ARG";
  case GCOMP_ERR_MEMORY:
    return "GCOMP_ERR_MEMORY";
  case GCOMP_ERR_LIMIT:
    return "GCOMP_ERR_LIMIT";
  case GCOMP_ERR_CORRUPT:
    return "GCOMP_ERR_CORRUPT";
  case GCOMP_ERR_UNSUPPORTED:
    return "GCOMP_ERR_UNSUPPORTED";
  case GCOMP_ERR_INTERNAL:
    return "GCOMP_ERR_INTERNAL";
  case GCOMP_ERR_IO:
    return "GCOMP_ERR_IO";
  default:
    return "GCOMP_ERR_UNKNOWN";
  }
}
