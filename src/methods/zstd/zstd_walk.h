/**
 * @file zstd_walk.h
 *
 * Declarations for the Zstandard stream walker; see zstd_walk.c.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ZSTD_WALK_H
#define GHOTI_IO_GCOMP_ZSTD_WALK_H

#include <ghoti.io/compress/macros.h>

#include "../../core/walk_internal.h"
#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A Zstandard walk in progress. Zero it before the first call.
 *
 * Nothing here is allocated, so it can live on the stack and be abandoned
 * without a teardown.
 */
typedef struct {
  uint64_t pos;                ///< Where the next byte handed in sits.
  int in_frame;                ///< Inside a frame's block sequence.

  /**
   * @brief A unit whose header has been read but whose bytes are not all in.
   *
   * An event is reported when the unit it describes has been wholly accounted
   * for, never merely when its header says how big it is - see zstd_walk.c for
   * why that distinction is the whole contract.
   */
  int has_pending;
  gcomp_walk_event_t pending;

  int frame_pending;           ///< A frame is owed its event too.
  uint64_t frame_start;        ///< Offset of the frame being walked.
  int frame_checksum;          ///< Frame declares a content checksum.
  uint64_t frame_content_size; ///< Declared content size, 0 when absent.
  uint64_t skip;               ///< Payload bytes still to step over.
} zstd_walk_t;

/// Feed bytes; report the units they complete. See zstd_walk.c.
gcomp_status_t zstd_walk_update(zstd_walk_t * w, const uint8_t * data,
    size_t size, size_t * used_out, gcomp_walk_event_t * events,
    size_t max_events, size_t * n_events_out);

/// Walk a whole stream that is already in memory. See zstd_walk.c.
gcomp_status_t zstd_walk_all(const uint8_t * data, size_t size,
    gcomp_walk_event_t * events, size_t max_events, size_t * n_events_out,
    size_t * used_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ZSTD_WALK_H
