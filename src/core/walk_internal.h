/**
 * @file walk_internal.h
 *
 * Walking a compressed stream without decoding it.
 *
 * ## What a walker is for
 *
 * Two things need to know where a stream's frames and blocks begin and end
 * without paying to decompress them:
 *
 * - **Parallel decode.** A unit that can be decoded on its own is a job. For
 *   LZ4 with independent blocks that is every block; for Zstandard it is a
 *   whole frame, because blocks inside one frame share a window (RFC 8878
 *   section 3.1.1.1.2) and cannot be split.
 * - **Random access.** A seek table is a list of these boundaries, so an index
 *   is built by walking once.
 *
 * Both want the same answers, so the walk is written once per format and used
 * twice rather than being reimplemented on each side.
 *
 * ## Resumable, and cheap
 *
 * A walker is fed whatever bytes have arrived and reports the units that are
 * wholly described by them. It carries its position across calls, so a caller
 * streaming a file in 4 KB pieces gets the same events as one handing over the
 * whole thing - which is the property the tests check, because a state machine
 * that is only ever given complete input is a state machine whose resumption
 * has never been exercised.
 *
 * It does not need a block's payload in memory: a payload is stepped over by
 * counting, so walking a gigabyte costs the headers and nothing else.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_WALK_INTERNAL_H
#define GHOTI_IO_GCOMP_WALK_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a walker found.
 */
typedef enum {
  /**
   * @brief One complete frame.
   *
   * Reported when its last byte has been accounted for, so
   * @ref gcomp_walk_event_t::size is the whole frame including its header and
   * any trailing checksum. This is the unit a Zstandard decode job takes.
   */
  GCOMP_WALK_FRAME,

  /**
   * @brief One block inside the frame being walked.
   *
   * The unit an LZ4 decode job takes when the frame sets B.Indep. Reported for
   * Zstandard too, where it is what a seek table indexes rather than something
   * that can be decoded alone.
   */
  GCOMP_WALK_BLOCK,

  /**
   * @brief A skippable frame, which carries no compressed data.
   *
   * RFC 8878 section 3.1.2, and the same magic range in the LZ4 frame format.
   * Reported rather than swallowed: a caller reassembling a stream has to put
   * it back where it was, and a caller indexing one needs its length.
   */
  GCOMP_WALK_SKIPPABLE,
} gcomp_walk_kind_t;

/**
 * @brief One thing a walker found, and where it is.
 */
typedef struct {
  gcomp_walk_kind_t kind;

  /// Byte offset from the start of the stream.
  uint64_t offset;

  /// Bytes it occupies on the wire, header and any trailing checksum included.
  uint64_t size;

  /**
   * @brief Where this unit's payload begins, and how much of it there is.
   *
   * @ref offset and @ref size describe the unit as it sits in the stream;
   * these describe the part a decoder is given. They differ by the unit's own
   * header and by any checksum after it - four bytes per block when an LZ4
   * frame sets B.Checksum, four after a Zstandard frame's last block when it
   * sets Content_Checksum_Flag.
   *
   * Without these a consumer has to know each format's framing to find the
   * bytes, which is the thing the walker exists to save it from. Leaving them
   * out was a real gap: a test that sliced `offset + header` and
   * `size - header` was right only for streams with no checksums and no
   * stored blocks, and silently wrong for everything else.
   */
  uint64_t payload_offset;
  uint64_t payload_size;

  /**
   * @brief Non-zero when the payload is the output bytes verbatim.
   *
   * An LZ4 block with the high bit set in its size, or a Zstandard
   * `Raw_Block`. A consumer can copy those instead of calling a decoder.
   *
   * A Zstandard `RLE_Block` is *not* stored: its single byte is not the
   * output, it is what the output repeats - see @ref content_size.
   */
  int stored;

  /**
   * @brief Decompressed bytes it stands for, or 0 when the stream does not
   *        say.
   *
   * For a frame this is the declared content size, which most streams omit.
   * For a block it is what the block header states where the format states it
   * and 0 where it does not - so it is a hint for sizing a buffer, never a
   * promise, and never a substitute for the output limits.
   */
  uint64_t content_size;

  /// Non-zero when this is the last block of its frame.
  int last;
} gcomp_walk_event_t;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_WALK_INTERNAL_H
