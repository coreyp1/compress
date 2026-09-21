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
 * 4. **Decode table** is built as a two-level structure of packed entries:
 *    - **Fast table** (2^FAST_BITS entries): for codes of length <=
 *      FAST_BITS, each possible bit pattern indexes directly to a finished
 *      entry. One code of length L fills 2^(FAST_BITS - L) entries.
 *    - **Long table**: for codes longer than FAST_BITS, the fast entry has
 *      nbits 0 and carries instead the width and base offset of a sub-table
 *      inside long_table, which the decoder indexes with the next few bits.
 *
 * ## What an entry says
 *
 * An entry is not a symbol. The symbol number is of no use to a decoder --
 * what it needs is what the symbol *means*, and for two of DEFLATE's three
 * alphabets that is a second lookup into a table of bases and extra-bit
 * counts (RFC 1951 3.2.5). Doing that lookup once per symbol decoded, when
 * the answer depends only on the code lengths the block declared, is work in
 * the wrong place. So it is done once per table build and the answer is in
 * the entry: a literal carries its byte, a length or a distance carries its
 * base and how many extra bits follow, end-of-block and the symbols the
 * format reserves carry nothing but their own identity.
 *
 * That is why ::gcomp_deflate_huffman_build_decode_table() has to be told
 * which alphabet it is building.
 *
 * ## How the table is used (decode algorithm)
 *
 * Bits are read LSB-first (DEFLATE convention). To decode one symbol:
 *
 *   1. Peek the next FAST_BITS bits from the bit stream -> index `idx`.
 *   2. Take `e = fast_table[idx]` and look at its nbits:
 *      - nbits > 0: `e` is the answer; consume nbits bits and act on its op.
 *      - nbits == 0 and extra > 0: `e` points at a sub-table. Read `extra`
 *        more bits -> `low`, then `e = long_table[value + low]` and act on
 *        that, consuming its nbits.
 *      - nbits == 0 and extra == 0: no code has this prefix; the stream is
 *        corrupt.
 *
 * This gives O(1) decode for most symbols (short codes) and one extra lookup
 * for long codes, avoiding a full tree walk per symbol.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_DEFLATE_HUFFMAN_H
#define GHOTI_IO_GCOMP_SRC_METHODS_DEFLATE_HUFFMAN_H

#include <ghoti.io/compress/allocator.h>
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
 * Fixed storage cost (not counting long_table) is one packed entry per
 * first-level slot: (1<<FAST_BITS) * 4 bytes, so 2048 bytes at FAST_BITS=9,
 * plus whatever long_table allocates for long codes. (It used to be 3584:
 * the sub-table's width and base offset lived in two side arrays of their
 * own, and now they live in the fast entry that points at it.)
 *
 * FAST_BITS can be as low as 1 in principle, but typical choices for DEFLATE
 * decoders are around 8-10 (9 is a common sweet spot).
 */
#define GCOMP_DEFLATE_HUFFMAN_FAST_BITS 9

/** First-level decode table size (2^FAST_BITS). */
#define GCOMP_DEFLATE_HUFFMAN_FAST_SIZE (1u << GCOMP_DEFLATE_HUFFMAN_FAST_BITS)

/**
 * @brief One entry of a decode table: everything the decoder needs, packed.
 *
 * Four fields in 32 bits, laid out so that the two the decoder always wants
 * are the cheapest to get at:
 *
 * | bits | field | meaning |
 * | ---: | --- | --- |
 * | 3:0 | nbits | code length 1..15, or 0 for "not a code" |
 * | 7:4 | extra | extra bits that follow the code, 0..13 |
 * | 9:8 | op | what the entry is: see the GCOMP_DEFLATE_OP_ values |
 * | 31:16 | value | the byte, the base, the symbol, or a sub-table offset |
 *
 * `value` sits at the top so that reading it is a shift with no mask.
 *
 * **nbits == 0** is the one case where the fields mean something else. In
 * the fast table it says the code is longer than FAST_BITS and this entry
 * points at a sub-table: `extra` is the width of the sub-table's index and
 * `value` its base offset into long_table. With `extra` also 0 there is no
 * sub-table either, and no code begins with these bits. In the long table
 * nbits == 0 only ever means the latter.
 *
 * A plain uint32_t and explicit shifts rather than a bitfield struct: bitfield
 * layout is implementation-defined, and this one is read in the decoder's
 * innermost loop.
 */
typedef uint32_t gcomp_deflate_huffman_entry_t;

/** A literal byte, or - for the code length alphabet - the symbol itself. */
#define GCOMP_DEFLATE_OP_LITERAL 0u
/** A length or a distance: @c value is the base, @c extra the bits to add. */
#define GCOMP_DEFLATE_OP_MATCH 1u
/** End of block (RFC 1951 3.2.3, symbol 256). */
#define GCOMP_DEFLATE_OP_END 2u
/** A code the alphabet defines and the format reserves. Always corrupt. */
#define GCOMP_DEFLATE_OP_BAD 3u

/** @brief Code length in bits, or 0 (see the entry documentation). */
static inline uint32_t gcomp_deflate_entry_nbits(
    gcomp_deflate_huffman_entry_t e) {
  return e & 0x0Fu;
}

/** @brief Extra bits following the code, or a sub-table's index width. */
static inline uint32_t gcomp_deflate_entry_extra(
    gcomp_deflate_huffman_entry_t e) {
  return (e >> 4u) & 0x0Fu;
}

/** @brief What the entry is: one of the GCOMP_DEFLATE_OP_ values. */
static inline uint32_t gcomp_deflate_entry_op(
    gcomp_deflate_huffman_entry_t e) {
  return (e >> 8u) & 0x03u;
}

/** @brief The byte, the base, the symbol, or a sub-table's base offset. */
static inline uint32_t gcomp_deflate_entry_value(
    gcomp_deflate_huffman_entry_t e) {
  return e >> 16u;
}

/**
 * @brief Assemble an entry.
 *
 * @param op    One of the GCOMP_DEFLATE_OP_ values (0..3).
 * @param value 0..65535.
 * @param extra 0..15.
 * @param nbits 0..15.
 */
static inline gcomp_deflate_huffman_entry_t gcomp_deflate_entry_make(
    uint32_t op, uint32_t value, uint32_t extra, uint32_t nbits) {
  return ((value & 0xFFFFu) << 16u) | ((op & 0x03u) << 8u) |
      ((extra & 0x0Fu) << 4u) | (nbits & 0x0Fu);
}

/**
 * @brief Which of DEFLATE's three alphabets a table is being built for.
 *
 * The symbol numbers are the same shape in all three; what they mean is not,
 * and the meaning is what goes in the entry.
 */
typedef enum gcomp_deflate_alphabet_e {
  /** RFC 1951 3.2.7's code length alphabet: 19 symbols, value = the symbol. */
  GCOMP_DEFLATE_ALPHABET_CODELEN = 0,
  /** Literal/length: 0-255 literals, 256 end of block, 257-285 lengths. */
  GCOMP_DEFLATE_ALPHABET_LITLEN = 1,
  /** Distance: 0-29 distances; 30 and 31 exist and are reserved. */
  GCOMP_DEFLATE_ALPHABET_DISTANCE = 2
} gcomp_deflate_alphabet_t;

// Forward declare allocator for storage in decode table

/**
 * @brief Two-level Huffman decode table for fast decoding.
 *
 * **Creation**: Built from code lengths by
 * gcomp_deflate_huffman_build_decode_table(). Short codes (length <= FAST_BITS)
 * fill fast_table; longer codes use long_table, and the fast entry that got
 * the decoder there says where and how wide.
 *
 * **Usage**: see the entry documentation and the file header.
 */
typedef struct gcomp_deflate_huffman_decode_table_s {
  /** First-level table (one entry per possible FAST_BITS-bit value). */
  gcomp_deflate_huffman_entry_t fast_table[GCOMP_DEFLATE_HUFFMAN_FAST_SIZE];
  /** Sub-tables for codes longer than FAST_BITS, laid end to end. */
  gcomp_deflate_huffman_entry_t * long_table;
  /** Number of entries in long_table. */
  size_t long_table_count;
  /** Allocator used for long_table (stored for cleanup). */
  const gcomp_allocator_t * allocator;
} gcomp_deflate_huffman_decode_table_t;

/**
 * @brief Validate code lengths for a canonical Huffman tree.
 *
 * Rejects over-subscribed trees (too many codes at a given length: would
 * exceed 2^bits slots). Incomplete trees (Kraft sum < 1) pass: they can be
 * built, and an encoder may legitimately emit one (RFC 1951 3.2.7's single
 * distance code). Whether a *decoded* stream is allowed to contain one is a
 * separate question, and a stricter one - see
 * ::gcomp_deflate_huffman_check_complete(). Code length 0 means the symbol is
 * not used.
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
 * @brief Check that code lengths form a *complete* Huffman code (RFC 1951).
 *
 * ::gcomp_deflate_huffman_validate() asks only whether the lengths can be
 * turned into a table at all - that is a structural question, and an encoder
 * building its own codes never needs to ask any other. A decoder does: RFC
 * 1951 3.2.2 constructs the code from the lengths in a way that assumes the
 * Kraft sum is exactly 1, so a stream whose lengths leave the sum short
 * describes bit patterns the code does not define. Such a stream is not a
 * DEFLATE stream, and accepting it means decoding whatever those undefined
 * patterns happen to land on and calling the result a success.
 *
 * RFC 1951 3.2.7 names the one exception: "If only one distance code is used,
 * it is encoded using one bit ... Note that in this case there is an
 * incomplete Huffman tree". @p allow_single_code admits it. Pass 0 for the
 * code length alphabet, which has no such exception, and 1 for the
 * literal/length and distance alphabets.
 *
 * An alphabet with no used symbols at all is accepted whatever @p
 * allow_single_code says: a block that codes only literals carries an empty
 * distance alphabet, and the absence of a code is not an incomplete one. The
 * caller decides whether it may then read a symbol from it.
 *
 * @param lengths           Code length per symbol (0 = unused). Not NULL.
 * @param num_symbols       Number of symbols (lengths[0 .. num_symbols-1]).
 * @param max_bits          Maximum allowed code length (15 for DEFLATE).
 * @param allow_single_code Non-zero to admit 3.2.7's one-bit single-code case.
 * @return ::GCOMP_OK if complete (or empty, or the permitted single code),
 *         ::GCOMP_ERR_CORRUPT if incomplete or over-subscribed,
 *         ::GCOMP_ERR_INVALID_ARG if parameters invalid.
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_huffman_check_complete(
    const uint8_t * lengths, size_t num_symbols, unsigned max_bits,
    int allow_single_code);

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
 * @param alphabet    What the symbols mean; decides what each entry carries.
 * @param table       Decode table to fill. Must not be NULL. fast_table is
 *                    always filled; long_table may be allocated and set.
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG or ::GCOMP_ERR_CORRUPT
 *         on failure. On failure, table state is undefined.
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_deflate_huffman_build_decode_table(
    const gcomp_allocator_t * allocator, const uint8_t * lengths,
    size_t num_symbols, unsigned max_bits, gcomp_deflate_alphabet_t alphabet,
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

#endif // GHOTI_IO_GCOMP_SRC_METHODS_DEFLATE_HUFFMAN_H
