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
  const uint8_t lengths[] = {3, 3, 3, 3, 3, 2, 4, 4};
  gcomp_deflate_huffman_decode_table_t table;

  ASSERT_EQ(gcomp_deflate_huffman_build_decode_table(lengths, 8u, 15u, &table),
      GCOMP_OK);

  // F has code 0, length 2. Fast table index for 2-bit code 0: indices 0 and 1
  // (code << (9-2) = 0, step = 128). So fast_table[0] and fast_table[1] =
  // (symbol 5, 2).
  EXPECT_EQ(table.fast_table[0].symbol, 5u);
  EXPECT_EQ(table.fast_table[0].nbits, 2u);
  EXPECT_EQ(table.fast_table[1].symbol, 5u);
  EXPECT_EQ(table.fast_table[1].nbits, 2u);

  // A has code 2, length 3. Indices 2<<6 = 128 .. 128+63 = 191.
  EXPECT_EQ(table.fast_table[128].symbol, 0u);
  EXPECT_EQ(table.fast_table[128].nbits, 3u);

  // G has code 14, length 4 (long code). Should be in long_table.
  EXPECT_EQ(table.long_extra_bits[14], 0u); // 4 - 9 is negative, so G is
  // actually in fast table: 4 <= 9. Code 14, len 4 -> start = 14<<5 = 448, step
  // = 32. So fast_table[448..479] = (symbol 6, 4).
  EXPECT_EQ(table.fast_table[448].symbol, 6u);
  EXPECT_EQ(table.fast_table[448].nbits, 4u);

  gcomp_deflate_huffman_decode_table_cleanup(&table);
}

TEST(DeflateHuffmanDecodeTable, LongCodesUseLongTable) {
  // One symbol with length 10 so it goes to long table.
  const uint8_t lengths[] = {10};
  gcomp_deflate_huffman_decode_table_t table;

  ASSERT_EQ(gcomp_deflate_huffman_build_decode_table(lengths, 1u, 15u, &table),
      GCOMP_OK);

  // Code 0, length 10. High 9 bits = 0, extra = 1. So fast_table[0].nbits = 0,
  // long_base[0] = 0, long_extra_bits[0] = 1. long_table has 2 entries.
  EXPECT_EQ(table.fast_table[0].nbits, 0u);
  EXPECT_EQ(table.long_extra_bits[0], 1u);
  EXPECT_EQ(table.long_table_count, 2u);
  EXPECT_NE(table.long_table, nullptr);
  EXPECT_EQ(table.long_table[0].symbol, 0u);
  EXPECT_EQ(table.long_table[0].nbits, 10u);

  gcomp_deflate_huffman_decode_table_cleanup(&table);
}

TEST(DeflateHuffmanDecodeTable, NullPointers) {
  const uint8_t lengths[] = {1};
  gcomp_deflate_huffman_decode_table_t table;

  EXPECT_EQ(gcomp_deflate_huffman_build_decode_table(nullptr, 1u, 15u, &table),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_deflate_huffman_build_decode_table(lengths, 1u, 15u, nullptr),
      GCOMP_ERR_INVALID_ARG);
}

TEST(DeflateHuffmanDecodeTable, CleanupNullSafe) {
  gcomp_deflate_huffman_decode_table_cleanup(nullptr);
}

TEST(DeflateHuffmanDecodeTable, CleanupIdempotent) {
  const uint8_t lengths[] = {10};
  gcomp_deflate_huffman_decode_table_t table;

  ASSERT_EQ(gcomp_deflate_huffman_build_decode_table(lengths, 1u, 15u, &table),
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
      gcomp_deflate_huffman_build_decode_table(lengths, 289u, 15u, &table),
      GCOMP_ERR_INVALID_ARG);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
