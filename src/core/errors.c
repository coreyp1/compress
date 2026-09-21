/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file errors.c
 *
 * Error helpers for the Ghoti.io Compress library.
 */

#include <ghoti.io/compress/macros.h>
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
