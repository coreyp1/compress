/**
 * @file adler32.h
 *
 * Adler-32 checksum for the Ghoti.io Compress library.
 *
 * Adler-32 is the checksum RFC 1950 puts in the zlib trailer, and by
 * extension the one PNG's IDAT stream carries.  It is defined in RFC 1950
 * section 9: two running sums modulo 65521, the largest prime below 2^16,
 * packed as `(s2 << 16) | s1`.
 *
 * ## How it differs from the CRC-32 next door
 *
 * It is weaker and faster.  CRC-32 detects every burst error up to 32 bits;
 * Adler-32 detects far less, and is notably poor on short inputs -- for a few
 * bytes, `s1` barely moves and `s2` is nearly its sum.  That is a property of
 * the checksum, not of this implementation, and it is why gzip uses CRC-32
 * and why zlib's own documentation calls Adler-32 the faster of the two
 * rather than the stronger.  It is here because RFC 1950 requires it, and it
 * should be used where a format asks for it and not as a general-purpose
 * integrity check.
 *
 * ## API shape
 *
 * The same three-call shape as @ref crc32.h, with one difference that matters:
 * the running value here is the *finished* value at every step, so there is
 * no finalize.  The initial value is 1, not 0 -- @ref GCOMP_ADLER32_INIT --
 * because `s1` starts at 1.  Starting from 0 produces a checksum that is
 * wrong in a way nothing downstream will notice until a decoder rejects it.
 *
 * ```c
 * uint32_t sum = GCOMP_ADLER32_INIT;
 * sum = gcomp_adler32_update(sum, chunk1, len1);
 * sum = gcomp_adler32_update(sum, chunk2, len2);
 * // sum is the checksum; nothing further to do.
 * ```
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ADLER32_H
#define GHOTI_IO_GCOMP_ADLER32_H

#include <ghoti.io/compress/macros.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Adler-32 initial value.
 *
 * RFC 1950 section 9: s1 starts at 1 and s2 at 0, so the packed initial value
 * is 1.  A checksum begun from 0 is wrong by a constant and will be rejected
 * by any decoder that checks it.
 */
#define GCOMP_ADLER32_INIT 1u

/**
 * @brief The modulus Adler-32 sums are taken over.
 *
 * 65521, the largest prime below 2^16 (RFC 1950 section 9).
 */
#define GCOMP_ADLER32_BASE 65521u

/**
 * @brief Compute the Adler-32 checksum of a buffer.
 *
 * Equivalent to gcomp_adler32_update(GCOMP_ADLER32_INIT, data, len).
 *
 * @param data Bytes to sum; may be NULL when @p len is 0.
 * @param len How many bytes.
 * @return The checksum, ready to write out.
 *
 * Example:
 * @code
 * uint32_t sum = gcomp_adler32((const uint8_t *)"abc", 3); // 0x024D0127
 * @endcode
 */
GCOMP_API uint32_t gcomp_adler32(const uint8_t * data, size_t len);

/**
 * @brief Continue an Adler-32 checksum with more data.
 *
 * @param adler Running value, from @ref GCOMP_ADLER32_INIT or a previous call.
 * @param data Bytes to add; may be NULL when @p len is 0.
 * @param len How many bytes.
 * @return The updated checksum.
 *
 * Example:
 * @code
 * uint32_t sum = GCOMP_ADLER32_INIT;
 * sum = gcomp_adler32_update(sum, chunk1, len1);
 * sum = gcomp_adler32_update(sum, chunk2, len2);
 * @endcode
 */
GCOMP_API uint32_t gcomp_adler32_update(
    uint32_t adler, const uint8_t * data, size_t len);

/**
 * @brief Combine two Adler-32 checksums into the checksum of the whole.
 *
 * Given the checksum of A, the checksum of B and the length of B, produces
 * the checksum of A followed by B -- without seeing either again.  Useful
 * when parts of a stream were summed separately, as a parallel encoder's are.
 *
 * @param adler1 Checksum of the first part.
 * @param adler2 Checksum of the second part.
 * @param len2 Length in bytes of the second part.
 * @return Checksum of the two parts concatenated.
 */
GCOMP_API uint32_t gcomp_adler32_combine(
    uint32_t adler1, uint32_t adler2, uint64_t len2);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ADLER32_H
