/**
 * @file test_deflate_huffman.cpp
 *
 * Unit tests for DEFLATE canonical Huffman table builder in the Ghoti.io
 * Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <gtest/gtest.h>
#include <vector>

#include "../src/methods/deflate/huffman.h"

TEST(DeflateHuffmanValidate, Rfc1951Example) {
  // RFC 1951 example: alphabet ABCDEFGH, lengths (3,3,3,3,3,2,4,4)
  const uint8_t lengths[] = {3, 3, 3, 3, 3, 2, 4, 4};
  EXPECT_EQ(gcomp_deflate_huffman_validate(lengths, 8u, 15u), GCOMP_OK);
}

TEST(DeflateHuffmanValidate, OverSubscribedTree) {
  // Three symbols all with length 1: only 2 one-bit codes exist.
  const uint8_t lengths[] = {1, 1, 1};
  EXPECT_EQ(
      gcomp_deflate_huffman_validate(lengths, 3u, 15u), GCOMP_ERR_CORRUPT);
}

TEST(DeflateHuffmanValidate, IncompleteTreeAllowed) {
  // One symbol with length 1: Kraft sum = 1/2. DEFLATE allows this
  // (e.g. one unused distance code).
  const uint8_t lengths[] = {1};
  EXPECT_EQ(gcomp_deflate_huffman_validate(lengths, 1u, 15u), GCOMP_OK);
}

TEST(DeflateHuffmanValidate, NullLengths) {
  EXPECT_EQ(
      gcomp_deflate_huffman_validate(nullptr, 8u, 15u), GCOMP_ERR_INVALID_ARG);
}

TEST(DeflateHuffmanValidate, InvalidMaxBitsZero) {
  const uint8_t lengths[] = {1};
  EXPECT_EQ(
      gcomp_deflate_huffman_validate(lengths, 1u, 0u), GCOMP_ERR_INVALID_ARG);
}

TEST(DeflateHuffmanValidate, InvalidMaxBitsTooLarge) {
  const uint8_t lengths[] = {1};
  EXPECT_EQ(
      gcomp_deflate_huffman_validate(lengths, 1u, 16u), GCOMP_ERR_INVALID_ARG);
}

TEST(DeflateHuffmanValidate, LengthExceedsMaxBits) {
  const uint8_t lengths[] = {5, 16}; // 16 > 15
  EXPECT_EQ(
      gcomp_deflate_huffman_validate(lengths, 2u, 15u), GCOMP_ERR_CORRUPT);
}

TEST(DeflateHuffmanBuildCodes, Rfc1951Example) {
  // RFC 1951: lengths (3,3,3,3,3,2,4,4) -> codes A=2(010), B=3, C=4, D=5, E=6,
  // F=0(00), G=14(1110), H=15(1111)
  const uint8_t lengths[] = {3, 3, 3, 3, 3, 2, 4, 4};
  uint16_t codes[8];
  uint8_t code_lens[8];

  ASSERT_EQ(
      gcomp_deflate_huffman_build_codes(lengths, 8u, 15u, codes, code_lens),
      GCOMP_OK);

  EXPECT_EQ(codes[0], 2u);  // A
  EXPECT_EQ(codes[1], 3u);  // B
  EXPECT_EQ(codes[2], 4u);  // C
  EXPECT_EQ(codes[3], 5u);  // D
  EXPECT_EQ(codes[4], 6u);  // E
  EXPECT_EQ(codes[5], 0u);  // F
  EXPECT_EQ(codes[6], 14u); // G
  EXPECT_EQ(codes[7], 15u); // H

  EXPECT_EQ(code_lens[0], 3u);
  EXPECT_EQ(code_lens[5], 2u);
  EXPECT_EQ(code_lens[6], 4u);
}

TEST(DeflateHuffmanBuildCodes, CodeLensNullOptional) {
  const uint8_t lengths[] = {2, 2};
  uint16_t codes[2];

  ASSERT_EQ(gcomp_deflate_huffman_build_codes(lengths, 2u, 15u, codes, nullptr),
      GCOMP_OK);
  EXPECT_EQ(codes[0], 0u);
  EXPECT_EQ(codes[1], 1u);
}

TEST(DeflateHuffmanBuildCodes, NullPointers) {
  const uint8_t lengths[] = {1};
  uint16_t codes[1];
  uint8_t code_lens[1];

  EXPECT_EQ(
      gcomp_deflate_huffman_build_codes(nullptr, 1u, 15u, codes, code_lens),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_deflate_huffman_build_codes(lengths, 1u, 15u, nullptr, code_lens),
      GCOMP_ERR_INVALID_ARG);
}

TEST(DeflateHuffmanBuildCodes, OverSubscribedReturnsCorrupt) {
  const uint8_t lengths[] = {1, 1, 1};
  uint16_t codes[3];
  uint8_t code_lens[3];

  EXPECT_EQ(
      gcomp_deflate_huffman_build_codes(lengths, 3u, 15u, codes, code_lens),
      GCOMP_ERR_CORRUPT);
}

TEST(DeflateHuffmanBuildCodes, SingleSymbol) {
  const uint8_t lengths[] = {1};
  uint16_t codes[1];
  uint8_t code_lens[1];

  ASSERT_EQ(
      gcomp_deflate_huffman_build_codes(lengths, 1u, 15u, codes, code_lens),
      GCOMP_OK);
  EXPECT_EQ(codes[0], 0u);
  EXPECT_EQ(code_lens[0], 1u);
}

TEST(DeflateHuffmanBuildCodes, ZeroLengthSymbolsSkipped) {
  // Symbols 0 and 2 have length 0; only symbol 1 gets a code.
  const uint8_t lengths[] = {0, 1, 0};
  uint16_t codes[3];
  uint8_t code_lens[3];
  std::memset(codes, 0xFF, sizeof(codes));
  std::memset(code_lens, 0xFF, sizeof(code_lens));

  ASSERT_EQ(
      gcomp_deflate_huffman_build_codes(lengths, 3u, 15u, codes, code_lens),
      GCOMP_OK);
  // Symbol 1 gets code 0, length 1. Symbols 0 and 2 unchanged (0xFF).
  EXPECT_EQ(codes[1], 0u);
  EXPECT_EQ(code_lens[1], 1u);
}

TEST(DeflateHuffmanDecodeTable, BuildFromRfcExample) {
  // RFC 1951 section 3.2.2's worked example, symbols A..H.
  const uint8_t lengths[] = {3, 3, 3, 3, 3, 2, 4, 4};
  gcomp_deflate_huffman_decode_table_t table;

  ASSERT_EQ(
      gcomp_deflate_huffman_build_decode_table(nullptr, lengths, 8u, 15u,
          GCOMP_DEFLATE_ALPHABET_CODELEN, &table),
      GCOMP_OK);

  // The table is indexed by the bits as they arrive, which is the code
  // REVERSED -- DEFLATE packs bits low end first (section 3.1.1) while the
  // codes themselves are high end first.  So a code of length L sits at
  // index reverse(code, L) and repeats every 2^L, rather than at
  // code << (FAST_BITS - L) as a run.
  //
  //   sym  letter  len  code    index  stride
  //     5    F      2   00        0       4
  //     2    C      3   100       1       8
  //     0    A      3   010       2       8
  //     4    E      3   110       3       8
  //     3    D      3   101       5       8
  //     1    B      3   011       6       8
  //     6    G      4   1110      7      16
  //     7    H      4   1111     15      16
  struct {
    unsigned index;
    unsigned stride;
    uint32_t symbol;
    uint32_t nbits;
  } const expected[] = {
      {0u, 4u, 5u, 2u},
      {1u, 8u, 2u, 3u},
      {2u, 8u, 0u, 3u},
      {3u, 8u, 4u, 3u},
      {5u, 8u, 3u, 3u},
      {6u, 8u, 1u, 3u},
      {7u, 16u, 6u, 4u},
      {15u, 16u, 7u, 4u},
  };

  for (const auto & e : expected) {
    // Every slot the code matches, not just the first: a short code is
    // replicated across the table and decoding relies on all of them.
    for (unsigned i = e.index; i < GCOMP_DEFLATE_HUFFMAN_FAST_SIZE;
         i += e.stride) {
      EXPECT_EQ(gcomp_deflate_entry_value(table.fast_table[i]), e.symbol)
          << "fast_table[" << i << "]";
      EXPECT_EQ(gcomp_deflate_entry_nbits(table.fast_table[i]), e.nbits)
          << "fast_table[" << i << "]";
    }
  }

  // Nothing landed in the long table: the longest code here is 4 bits.
  EXPECT_EQ(table.long_table_count, 0u);

  gcomp_deflate_huffman_decode_table_cleanup(&table);
}

TEST(DeflateHuffmanDecodeTable, LongCodesUseLongTable) {
  // One symbol with length 10 so it goes to long table.
  const uint8_t lengths[] = {10};
  gcomp_deflate_huffman_decode_table_t table;

  ASSERT_EQ(
      gcomp_deflate_huffman_build_decode_table(nullptr, lengths, 1u, 15u,
          GCOMP_DEFLATE_ALPHABET_CODELEN, &table),
      GCOMP_OK);

  // Code 0, length 10. The first 9 bits give index 0, so fast_table[0] is a
  // sub-table pointer: nbits 0, one bit of index, based at 0.  The sub-table
  // has two entries and both hold the symbol.
  EXPECT_EQ(gcomp_deflate_entry_nbits(table.fast_table[0]), 0u);
  EXPECT_EQ(gcomp_deflate_entry_extra(table.fast_table[0]), 1u);
  EXPECT_EQ(gcomp_deflate_entry_value(table.fast_table[0]), 0u);
  EXPECT_EQ(table.long_table_count, 2u);
  EXPECT_NE(table.long_table, nullptr);
  EXPECT_EQ(gcomp_deflate_entry_value(table.long_table[0]), 0u);
  EXPECT_EQ(gcomp_deflate_entry_nbits(table.long_table[0]), 10u);

  gcomp_deflate_huffman_decode_table_cleanup(&table);
}

/**
 * @brief The entry for one symbol of an alphabet.
 *
 * Two symbols of length one make a complete code, and equal lengths are
 * assigned in symbol order (RFC 1951 3.2.2), so the lower of the two gets
 * code 0 and fills the even slots while the higher gets code 1 and the odd
 * ones.  That puts any one symbol's entry somewhere findable without
 * depending on the rest of the alphabet.
 */
static gcomp_deflate_huffman_entry_t EntryFor(
    gcomp_deflate_alphabet_t alphabet, size_t num_symbols, unsigned sym) {
  std::vector<uint8_t> lengths(num_symbols, 0u);
  const unsigned other = (sym == 0u) ? 1u : 0u;
  lengths[sym] = 1u;
  lengths[other] = 1u;

  gcomp_deflate_huffman_decode_table_t table;
  EXPECT_EQ(gcomp_deflate_huffman_build_decode_table(nullptr, lengths.data(),
                num_symbols, 15u, alphabet, &table),
      GCOMP_OK);
  const unsigned slot = (sym < other) ? 0u : 1u;
  const gcomp_deflate_huffman_entry_t e = table.fast_table[slot];
  gcomp_deflate_huffman_decode_table_cleanup(&table);
  return e;
}

/**
 * An entry says what the symbol means, not which symbol it was.
 *
 * The decoder has no use for a symbol number: what it needs is a byte to
 * emit, or a base and an extra-bit count, or the fact that the block has
 * ended.  Working that out belongs to the table build, which happens once per
 * block, and not to the decode, which happens hundreds of thousands of times.
 * These tests pin the translation, because a decoder that reads the entry and
 * acts on it has no second copy of RFC 1951 section 3.2.5 to check it against.
 */
TEST(DeflateHuffmanEntry, LiteralLengthAlphabet) {
  const size_t n = 288u;
  for (unsigned sym : {0u, 1u, 128u, 255u}) {
    const gcomp_deflate_huffman_entry_t e =
        EntryFor(GCOMP_DEFLATE_ALPHABET_LITLEN, n, sym);
    EXPECT_EQ(gcomp_deflate_entry_op(e), GCOMP_DEFLATE_OP_LITERAL) << sym;
    EXPECT_EQ(gcomp_deflate_entry_value(e), sym) << sym;
    EXPECT_EQ(gcomp_deflate_entry_extra(e), 0u) << sym;
  }

  const gcomp_deflate_huffman_entry_t eob =
      EntryFor(GCOMP_DEFLATE_ALPHABET_LITLEN, n, 256u);
  EXPECT_EQ(gcomp_deflate_entry_op(eob), GCOMP_DEFLATE_OP_END);

  // RFC 1951 section 3.2.5's length table: both ends and a few in between.
  struct {
    unsigned sym;
    uint32_t base;
    uint32_t extra;
  } const lengths[] = {
      {257u, 3u, 0u},
      {264u, 10u, 0u},
      {265u, 11u, 1u},
      {269u, 19u, 2u},
      {280u, 115u, 4u},
      {284u, 227u, 5u},
      {285u, 258u, 0u}, // The one length at the top with no extra bits.
  };
  for (const auto & l : lengths) {
    const gcomp_deflate_huffman_entry_t e =
        EntryFor(GCOMP_DEFLATE_ALPHABET_LITLEN, n, l.sym);
    EXPECT_EQ(gcomp_deflate_entry_op(e), GCOMP_DEFLATE_OP_MATCH) << l.sym;
    EXPECT_EQ(gcomp_deflate_entry_value(e), l.base) << l.sym;
    EXPECT_EQ(gcomp_deflate_entry_extra(e), l.extra) << l.sym;
  }
}

TEST(DeflateHuffmanEntry, DistanceAlphabet) {
  const size_t n = 32u;
  struct {
    unsigned sym;
    uint32_t base;
    uint32_t extra;
  } const dists[] = {
      {0u, 1u, 0u},
      {3u, 4u, 0u},
      {4u, 5u, 1u},
      {15u, 193u, 6u},
      {28u, 16385u, 13u},
      {29u, 24577u, 13u},
  };
  for (const auto & d : dists) {
    const gcomp_deflate_huffman_entry_t e =
        EntryFor(GCOMP_DEFLATE_ALPHABET_DISTANCE, n, d.sym);
    EXPECT_EQ(gcomp_deflate_entry_op(e), GCOMP_DEFLATE_OP_MATCH) << d.sym;
    EXPECT_EQ(gcomp_deflate_entry_value(e), d.base) << d.sym;
    EXPECT_EQ(gcomp_deflate_entry_extra(e), d.extra) << d.sym;
  }
}

/**
 * The symbols the format reserves are marked, not left out.
 *
 * A stream may give them a code -- HLIT reaches 286, HDIST reaches 32, and
 * the fixed alphabets define all four -- so a decoder can arrive at one and
 * must refuse.  Marking them in the table is what makes that one test in the
 * decoder rather than a comparison against a number from the specification,
 * written out once in each of the two decode paths and forgettable in either.
 *
 * They keep their code length, which is not a detail.  A decoder holding
 * fewer bits than the code is long must be able to say "not yet" rather than
 * "corrupt": a short buffer's zero padding can land on any slot, and a
 * reserved entry that looked like an empty one would turn a good stream into
 * a refused one.
 */
TEST(DeflateHuffmanEntry, ReservedSymbolsAreMarkedAndKeepTheirLength) {
  for (unsigned sym : {286u, 287u}) {
    const gcomp_deflate_huffman_entry_t e =
        EntryFor(GCOMP_DEFLATE_ALPHABET_LITLEN, 288u, sym);
    EXPECT_EQ(gcomp_deflate_entry_op(e), GCOMP_DEFLATE_OP_BAD) << sym;
    EXPECT_EQ(gcomp_deflate_entry_nbits(e), 1u) << sym;
  }
  for (unsigned sym : {30u, 31u}) {
    const gcomp_deflate_huffman_entry_t e =
        EntryFor(GCOMP_DEFLATE_ALPHABET_DISTANCE, 32u, sym);
    EXPECT_EQ(gcomp_deflate_entry_op(e), GCOMP_DEFLATE_OP_BAD) << sym;
    EXPECT_EQ(gcomp_deflate_entry_nbits(e), 1u) << sym;
  }
}

TEST(DeflateHuffmanEntry, CodeLengthAlphabetCarriesTheSymbol) {
  for (unsigned sym = 0u; sym < 19u; sym++) {
    const gcomp_deflate_huffman_entry_t e =
        EntryFor(GCOMP_DEFLATE_ALPHABET_CODELEN, 19u, sym);
    EXPECT_EQ(gcomp_deflate_entry_op(e), GCOMP_DEFLATE_OP_LITERAL) << sym;
    EXPECT_EQ(gcomp_deflate_entry_value(e), sym) << sym;
  }
}

TEST(DeflateHuffmanEntry, PackAndUnpackRoundTrip) {
  for (uint32_t op = 0u; op < 4u; op++) {
    for (uint32_t nbits = 0u; nbits <= 15u; nbits++) {
      for (uint32_t extra = 0u; extra <= 15u; extra++) {
        for (uint32_t value : {0u, 1u, 258u, 24577u, 65535u}) {
          const gcomp_deflate_huffman_entry_t e =
              gcomp_deflate_entry_make(op, value, extra, nbits);
          EXPECT_EQ(gcomp_deflate_entry_op(e), op);
          EXPECT_EQ(gcomp_deflate_entry_nbits(e), nbits);
          EXPECT_EQ(gcomp_deflate_entry_extra(e), extra);
          EXPECT_EQ(gcomp_deflate_entry_value(e), value);
        }
      }
    }
  }
}

TEST(DeflateHuffmanDecodeTable, UnknownAlphabetRefused) {
  const uint8_t lengths[] = {1};
  gcomp_deflate_huffman_decode_table_t table;
  EXPECT_EQ(gcomp_deflate_huffman_build_decode_table(nullptr, lengths, 1u, 15u,
                (gcomp_deflate_alphabet_t)7, &table),
      GCOMP_ERR_INVALID_ARG);
}

TEST(DeflateHuffmanDecodeTable, NullPointers) {
  const uint8_t lengths[] = {1};
  gcomp_deflate_huffman_decode_table_t table;

  EXPECT_EQ(
      gcomp_deflate_huffman_build_decode_table(nullptr, nullptr, 1u, 15u,
          GCOMP_DEFLATE_ALPHABET_CODELEN, &table),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_deflate_huffman_build_decode_table(nullptr, lengths, 1u, 15u,
          GCOMP_DEFLATE_ALPHABET_CODELEN, nullptr),
      GCOMP_ERR_INVALID_ARG);
}

TEST(DeflateHuffmanDecodeTable, CleanupNullSafe) {
  gcomp_deflate_huffman_decode_table_cleanup(nullptr);
}

TEST(DeflateHuffmanDecodeTable, CleanupIdempotent) {
  const uint8_t lengths[] = {10};
  gcomp_deflate_huffman_decode_table_t table;

  ASSERT_EQ(
      gcomp_deflate_huffman_build_decode_table(nullptr, lengths, 1u, 15u,
          GCOMP_DEFLATE_ALPHABET_CODELEN, &table),
      GCOMP_OK);
  gcomp_deflate_huffman_decode_table_cleanup(&table);
  gcomp_deflate_huffman_decode_table_cleanup(&table);
}

TEST(DeflateHuffmanDecodeTable, TooManySymbols) {
  uint8_t lengths[289];
  std::memset(lengths, 1, sizeof(lengths));
  gcomp_deflate_huffman_decode_table_t table;

  // 289 symbols exceeds internal limit (288).
  EXPECT_EQ(
      gcomp_deflate_huffman_build_decode_table(nullptr, lengths, 289u, 15u,
          GCOMP_DEFLATE_ALPHABET_CODELEN, &table),
      GCOMP_ERR_INVALID_ARG);
}

//
// Completeness, as distinct from buildability (RFC 1951 3.2.2 / 3.2.7)
//
// gcomp_deflate_huffman_validate() answers "can these lengths be turned into a
// table", which is all an encoder needs. A decoder needs the stricter question,
// and these say where the line is.
//

TEST(DeflateHuffmanComplete, ACompleteCodePasses) {
  // Two one-bit codes: Kraft sum exactly 1.
  const uint8_t two[2] = {1, 1};
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(two, 2u, 15u, 0), GCOMP_OK);
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(two, 2u, 15u, 1), GCOMP_OK);

  // 1 + 2 + 3 + 3: 1/2 + 1/4 + 1/8 + 1/8 = 1.
  const uint8_t mixed[4] = {1, 2, 3, 3};
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(mixed, 4u, 15u, 0), GCOMP_OK);
}

TEST(DeflateHuffmanComplete, AnIncompleteCodeIsRejected) {
  // 1 + 2: Kraft sum 3/4. Buildable, but not a complete code.
  const uint8_t lengths[2] = {1, 2};
  EXPECT_EQ(gcomp_deflate_huffman_validate(lengths, 2u, 15u), GCOMP_OK)
      << "validate() only asks whether the table can be built";
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(lengths, 2u, 15u, 0),
      GCOMP_ERR_CORRUPT);
  // The longest code is two bits, so 3.2.7's one-bit exception cannot save it
  // even where the exception is allowed.
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(lengths, 2u, 15u, 1),
      GCOMP_ERR_CORRUPT);
}

TEST(DeflateHuffmanComplete, TheSingleOneBitCodeIsAllowedOnlyWhereTheSpecSaysSo) {
  // RFC 1951 3.2.7: one distance code, encoded in one bit, incomplete tree.
  const uint8_t one[1] = {1};
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(one, 1u, 15u, 1), GCOMP_OK);
  // The code length alphabet gets no such exception.
  EXPECT_EQ(
      gcomp_deflate_huffman_check_complete(one, 1u, 7u, 0), GCOMP_ERR_CORRUPT);
}

TEST(DeflateHuffmanComplete, AnAlphabetWithNoSymbolsIsAbsentNotIncomplete) {
  // Every length zero: a block of literals carries no distance codes. That is
  // not an incomplete code, and the caller decides whether a symbol may then
  // be read from it.
  const uint8_t none[30] = {};
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(none, 30u, 15u, 0), GCOMP_OK);
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(none, 30u, 15u, 1), GCOMP_OK);
}

TEST(DeflateHuffmanComplete, AnOverSubscribedCodeIsRejectedHereToo) {
  // Three one-bit codes: only two exist. Rejected by both questions, so that
  // this one stands on its own rather than relying on its caller.
  const uint8_t over[3] = {1, 1, 1};
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(over, 3u, 15u, 1),
      GCOMP_ERR_CORRUPT);
}

TEST(DeflateHuffmanComplete, RejectsBadParameters) {
  const uint8_t lengths[2] = {1, 1};
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(nullptr, 2u, 15u, 0),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(lengths, 2u, 0u, 0),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_deflate_huffman_check_complete(lengths, 2u, 16u, 0),
      GCOMP_ERR_INVALID_ARG);
}

// A length above max_bits is refused before any arithmetic on it.
TEST(DeflateHuffmanComplete, ALengthBeyondMaxBitsIsRejected) {
  const uint8_t lengths[2] = {1, 8};
  EXPECT_EQ(
      gcomp_deflate_huffman_check_complete(lengths, 2u, 7u, 1), GCOMP_ERR_CORRUPT);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
