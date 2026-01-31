/**
 * @file huffman.h
 *
 * Canonical Huffman table builder for DEFLATE (RFC 1951). Builds codes from
 * code lengths, validates over-subscribed/incomplete trees, and builds
 * two-level fast decode tables.
 *
 * ## How the table is created
 *
 * 1. **Code lengths** come from the DEFLATE stream (fixed tables are
 *    predefined; dynamic blocks send a sequence of code lengths per symbol).
 * 2. **Validation** ensures the lengths form a valid prefix code (reject
 *    over-subscribed: more codes at a given length than 2^length allows).
 * 3. **Canonical code assignment** (RFC 1951, Section 3.2.2): from lengths
 *    we assign integer code values so that shorter codes have smaller
 *    values and same-length codes get consecutive values. This allows the
 *    stream to carry only lengths, not the full tree.
 * 4. **Decode table** is built as a two-level structure:
 *    - **Fast table** (2^FAST_BITS entries): for codes of length <=
 *      FAST_BITS, each possible bit pattern indexes directly to (symbol,
 *      nbits). One code of length L fills 2^(FAST_BITS - L) consecutive
 *      entries.
 *    - **Long table**: for codes longer than FAST_BITS, the first FAST_BITS
 *      bits index into fast_table (with nbits=0); the decoder then reads
 *      "extra" more bits and uses (long_base[fast_index] + extra_bits_value)
 *      to index long_table for the final (symbol, nbits).
 *
 * ## How the table is used (decode algorithm)
 *
 * Bits are read LSB-first (DEFLATE convention). To decode one symbol:
 *
 *   1. Peek the next FAST_BITS bits from the bit stream -> index `idx`.
 *   2. Look up fast_table[idx].nbits:
 *      - If nbits > 0: decoded symbol is fast_table[idx].symbol; consume
 *        nbits bits from the stream. Done.
 *      - If nbits == 0: read long_extra_bits[idx] more bits -> value `low`.
 *        long_idx = long_base[idx] + low. Decoded symbol is
 *        long_table[long_idx].symbol; consume long_table[long_idx].nbits
 *        bits. Done.
 *
 * This gives O(1) decode for most symbols (short codes) and one extra lookup
 * for long codes, avoiding a full tree walk per symbol.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_DEFLATE_HUFFMAN_H
#define GCOMP_DEFLATE_HUFFMAN_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum Huffman code length in DEFLATE (RFC 1951). */
#define GCOMP_DEFLATE_HUFFMAN_MAX_BITS 15

/**
 * Number of bits used for the first-level fast decode table.
 *
 * This is an implementation tradeoff, not mandated by RFC 1951.
 *
 * - Memory: the first-level table has 2^FAST_BITS entries, so each +1 bit
 *   doubles the table size and each -1 bit halves it.
 * - Speed: larger FAST_BITS increases the fraction of symbols that decode in
 *   a single lookup. Smaller FAST_BITS forces more symbols down the "long
 *   code" path (extra bit reads + one more lookup).
 *
 * Approximate fixed storage cost (not counting long_table):
 * - fast_table: (1<<FAST_BITS) * sizeof(gcomp_deflate_huffman_fast_entry_t)
 * - long_base: (1<<FAST_BITS) * sizeof(uint16_t)
 * - long_extra_bits: (1<<FAST_BITS) * sizeof(uint8_t)
 *
 * With FAST_BITS=9, (1<<FAST_BITS)=512. On typical ABIs
 * sizeof(gcomp_deflate_huffman_fast_entry_t) is 4 bytes (u16 + u8 + padding),
 * so the fixed first-level storage is about:
 *   512*4 + 512*2 + 512*1 = 3584 bytes (~3.5 KiB),
 * plus whatever long_table allocates for long codes.
 *
 * FAST_BITS can be as low as 1 in principle, but typical choices for DEFLATE
 * decoders are around 8-10 (9 is a common sweet spot).
 */
#define GCOMP_DEFLATE_HUFFMAN_FAST_BITS 9

/** First-level decode table size (2^FAST_BITS). */
#define GCOMP_DEFLATE_HUFFMAN_FAST_SIZE (1u << GCOMP_DEFLATE_HUFFMAN_FAST_BITS)

/**
 * @brief Single entry in the fast or long decode table.
 *
 * When @ref nbits is non-zero, the entry is a direct (symbol, nbits) decode:
 * the decoder emits @ref symbol and consumes @ref nbits bits from the stream.
 * When @ref nbits is zero in the fast table, the decoder must use the
 * long-code path (see ::gcomp_deflate_huffman_decode_table_t).
 */
typedef struct gcomp_deflate_huffman_fast_entry_s {
  uint16_t symbol; ///< Decoded symbol (meaningful when nbits > 0).
  uint8_t nbits;   ///< Number of bits consumed (0 in fast table = use long).
} gcomp_deflate_huffman_fast_entry_t;

// Forward declare allocator for storage in decode table
struct gcomp_allocator_s;

/**
 * @brief Two-level Huffman decode table for fast decoding.
 *
 * **Creation**: Built from code lengths by
 * gcomp_deflate_huffman_build_decode_table(). Short codes (length <= FAST_BITS)
 * fill fast_table; longer codes use long_table indexed by the first FAST_BITS
 * bits plus extra bits.
 *
 * **Usage**: Decoder peeks FAST_BITS bits -> fast_table index. If entry.nbits >
 * 0, symbol and consume nbits. Else read long_extra_bits[index] more bits,
 * long_idx = long_base[index] + those bits, then symbol =
 * long_table[long_idx].symbol and consume long_table[long_idx].nbits.
 */
typedef struct gcomp_deflate_huffman_decode_table_s {
  /** First-level table (one entry per possible FAST_BITS-bit value). */
  gcomp_deflate_huffman_fast_entry_t
      fast_table[GCOMP_DEFLATE_HUFFMAN_FAST_SIZE];
  /** For each first-level index with long codes: base index into long_table. */
  uint16_t long_base[GCOMP_DEFLATE_HUFFMAN_FAST_SIZE];
  /** For each first-level index: extra bits to read (0 if no long codes). */
  uint8_t long_extra_bits[GCOMP_DEFLATE_HUFFMAN_FAST_SIZE];
  /** Long-code entries: (symbol, nbits) for codes longer than FAST_BITS. */
  gcomp_deflate_huffman_fast_entry_t * long_table;
  /** Number of entries in long_table. */
  size_t long_table_count;
  /** Allocator used for long_table (stored for cleanup). */
  const struct gcomp_allocator_s * allocator;
} gcomp_deflate_huffman_decode_table_t;

/**
 * @brief Validate code lengths for a canonical Huffman tree.
 *
 * Rejects over-subscribed trees (too many codes at a given length: would
 * exceed 2^bits slots). Incomplete trees (Kraft sum < 1) are allowed per
 * RFC 1951 (e.g. one unused distance code). Code length 0 means the symbol
 * is not used.
 *
 * @param lengths    Code length per symbol (0 = unused). Must not be NULL.
 * @param num_symbols Number of symbols (lengths[0 .. num_symbols-1]).
 * @param max_bits   Maximum allowed code length (e.g. 15 for DEFLATE).
 * @return ::GCOMP_OK if valid, ::GCOMP_ERR_CORRUPT if over-subscribed or
 *         incomplete, ::GCOMP_ERR_INVALID_ARG if parameters invalid.
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_huffman_validate(
    const uint8_t * lengths, size_t num_symbols, unsigned max_bits);

/**
 * @brief Build canonical code values from code lengths (RFC 1951 algorithm).
 *
 * Fills @p codes and @p code_lens for each symbol. Symbols with length 0
 * are not assigned a code (codes[i] and code_lens[i] are left unchanged).
 * Call ::gcomp_deflate_huffman_validate() first.
 *
 * @param lengths     Code length per symbol (0 = unused). Must not be NULL.
 * @param num_symbols Number of symbols.
 * @param max_bits    Maximum code length (e.g. 15).
 * @param codes       Output canonical code values (only for symbols with
 *                    length > 0). Must not be NULL; array size >= num_symbols.
 * @param code_lens   Output code length per symbol (copy of lengths for
 *                    symbols with length > 0). Can be NULL to ignore.
 *                    If non-NULL, size >= num_symbols.
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if parameters
 *         invalid, ::GCOMP_ERR_CORRUPT if lengths are invalid (e.g.
 *         over-subscribed).
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_huffman_build_codes(
    const uint8_t * lengths, size_t num_symbols, unsigned max_bits,
    uint16_t * codes, uint8_t * code_lens);

/**
 * @brief Build a two-level fast decode table from code lengths.
 *
 * Validates lengths, builds canonical codes, then fills the decode table.
 * The caller must not free @p table; it does not allocate the table
 * structure itself, but @p table->long_table may be allocated by this
 * function (caller must call ::gcomp_deflate_huffman_decode_table_cleanup()).
 *
 * @param allocator   Allocator for long_table memory. If NULL, uses default.
 * @param lengths     Code length per symbol (0 = unused). Must not be NULL.
 * @param num_symbols Number of symbols.
 * @param max_bits    Maximum code length (e.g. 15).
 * @param table       Decode table to fill. Must not be NULL. fast_table,
 *                    long_base, long_extra_bits are always filled;
 *                    long_table may be allocated and set.
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG or ::GCOMP_ERR_CORRUPT
 *         on failure. On failure, table state is undefined.
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_huffman_build_decode_table(
    const struct gcomp_allocator_s * allocator, const uint8_t * lengths,
    size_t num_symbols, unsigned max_bits,
    gcomp_deflate_huffman_decode_table_t * table);

/**
 * @brief Release any heap memory used by a decode table.
 *
 * Only @p table->long_table is freed (if non-NULL). The table structure
 * itself is not freed.
 *
 * @param table Decode table whose long_table was built by
 *              ::gcomp_deflate_huffman_build_decode_table(). Can be NULL.
 */
GCOMP_INTERNAL_API void gcomp_deflate_huffman_decode_table_cleanup(
    gcomp_deflate_huffman_decode_table_t * table);

#ifdef __cplusplus
}
#endif

#endif // GCOMP_DEFLATE_HUFFMAN_H
