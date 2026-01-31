/**
 * @file test_deflate_bitio.cpp
 *
 * Unit tests for DEFLATE bit reader/writer utilities in the Ghoti.io
 * Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <gtest/gtest.h>
#include <vector>

// Include internal DEFLATE bit I/O headers
#include "../src/methods/deflate/bitreader.h"
#include "../src/methods/deflate/bitwriter.h"

TEST(DeflateBitReaderTest, ReadSingleByteBitsLsbFirst) {
  // 0b11001010 = 0xCA
  const uint8_t data[] = {0xCA};
  gcomp_deflate_bitreader_t reader;
  uint32_t value = 0;

  ASSERT_EQ(
      gcomp_deflate_bitreader_init(&reader, data, sizeof(data)), GCOMP_OK);

  // Read 3 bits: should get 0b010 (LSB-first: 0,1,0)
  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 3, &value), GCOMP_OK);
  EXPECT_EQ(value, 0b010u);

  // Next 5 bits should be the remaining bits of the byte.
  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 5, &value), GCOMP_OK);
  EXPECT_EQ(value, 0b11001u);

  EXPECT_TRUE(gcomp_deflate_bitreader_is_eof(&reader));
}

TEST(DeflateBitReaderTest, CrossByteBoundary) {
  // Two bytes: 0b10101100 (0xAC), 0b00110101 (0x35)
  const uint8_t data[] = {0xAC, 0x35};
  gcomp_deflate_bitreader_t reader;
  uint32_t value = 0;

  ASSERT_EQ(
      gcomp_deflate_bitreader_init(&reader, data, sizeof(data)), GCOMP_OK);

  // Read 7 bits from first byte.
  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 7, &value), GCOMP_OK);
  // Expected: lower 7 bits of 0xAC -> 0b0101100 (LSB-first representation)
  EXPECT_EQ(value, (0xACu & 0x7Fu));

  // Now read 5 more bits, which will cross into the second byte.
  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 5, &value), GCOMP_OK);
  // We don't assert a specific pattern here; we just ensure it succeeds and
  // we are not at EOF yet.
  EXPECT_FALSE(gcomp_deflate_bitreader_is_eof(&reader));
}

TEST(DeflateBitReaderTest, AlignToByteBoundary) {
  const uint8_t data[] = {0xFF, 0x12};
  gcomp_deflate_bitreader_t reader;
  uint32_t value = 0;

  ASSERT_EQ(
      gcomp_deflate_bitreader_init(&reader, data, sizeof(data)), GCOMP_OK);

  // Read 3 bits from first byte.
  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 3, &value), GCOMP_OK);

  // Align to next byte boundary - should discard remaining 5 bits of first
  // byte.
  ASSERT_EQ(gcomp_deflate_bitreader_align_to_byte(&reader), GCOMP_OK);

  // Next 8 bits should be exactly the second byte.
  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 8, &value), GCOMP_OK);
  EXPECT_EQ(value, 0x12u);
}

TEST(DeflateBitReaderTest, EofHandling) {
  const uint8_t data[] = {0x01};
  gcomp_deflate_bitreader_t reader;
  uint32_t value = 0;

  ASSERT_EQ(
      gcomp_deflate_bitreader_init(&reader, data, sizeof(data)), GCOMP_OK);

  // There are only 8 bits available. Requesting 9 should fail.
  ASSERT_EQ(
      gcomp_deflate_bitreader_read_bits(&reader, 9, &value), GCOMP_ERR_CORRUPT);
}

TEST(DeflateBitWriterTest, WriteAndFlushSingleByte) {
  uint8_t buffer[4] = {0};
  gcomp_deflate_bitwriter_t writer;

  ASSERT_EQ(
      gcomp_deflate_bitwriter_init(&writer, buffer, sizeof(buffer)), GCOMP_OK);

  // Write 3 bits: 0b101.
  ASSERT_EQ(gcomp_deflate_bitwriter_write_bits(&writer, 0b101u, 3), GCOMP_OK);

  // Flush to byte: expect 0b00000101 (LSB-first).
  ASSERT_EQ(gcomp_deflate_bitwriter_flush_to_byte(&writer), GCOMP_OK);
  EXPECT_EQ(gcomp_deflate_bitwriter_bytes_written(&writer), 1u);
  EXPECT_EQ(buffer[0], 0b00000101u);
}

TEST(DeflateBitWriterTest, WriteCrossByteBoundary) {
  uint8_t buffer[4] = {0};
  gcomp_deflate_bitwriter_t writer;

  ASSERT_EQ(
      gcomp_deflate_bitwriter_init(&writer, buffer, sizeof(buffer)), GCOMP_OK);

  // Write 12 bits: lower 12 bits of pattern.
  ASSERT_EQ(gcomp_deflate_bitwriter_write_bits(&writer, 0xABCu, 12), GCOMP_OK);
  ASSERT_EQ(gcomp_deflate_bitwriter_flush_to_byte(&writer), GCOMP_OK);

  // Now read them back with the bit reader to verify round-trip.
  gcomp_deflate_bitreader_t reader;
  uint32_t value = 0;
  ASSERT_EQ(gcomp_deflate_bitreader_init(&reader, buffer,
                gcomp_deflate_bitwriter_bytes_written(&writer)),
      GCOMP_OK);

  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 12, &value), GCOMP_OK);
  EXPECT_EQ(value, 0xABCu & ((1u << 12) - 1u));
}

TEST(DeflateBitWriterTest, BufferTooSmall) {
  uint8_t buffer[1] = {0};
  gcomp_deflate_bitwriter_t writer;

  ASSERT_EQ(
      gcomp_deflate_bitwriter_init(&writer, buffer, sizeof(buffer)), GCOMP_OK);

  // This will fill one byte.
  ASSERT_EQ(gcomp_deflate_bitwriter_write_bits(&writer, 0xFFu, 8), GCOMP_OK);

  // Writing more bits will succeed in buffering, but flushing to a byte
  // boundary should detect that the buffer is too small.
  ASSERT_EQ(gcomp_deflate_bitwriter_write_bits(&writer, 0x1u, 4), GCOMP_OK);
  ASSERT_EQ(gcomp_deflate_bitwriter_flush_to_byte(&writer), GCOMP_ERR_LIMIT);
}

TEST(DeflateBitIoTest, RoundTripBits) {
  // Use an arbitrary pattern of bits and verify round-trip.
  const uint32_t pattern = 0x00DEADu; // 24-bit pattern
  uint8_t buffer[8] = {0};

  gcomp_deflate_bitwriter_t writer;
  ASSERT_EQ(
      gcomp_deflate_bitwriter_init(&writer, buffer, sizeof(buffer)), GCOMP_OK);

  ASSERT_EQ(gcomp_deflate_bitwriter_write_bits(&writer, pattern, 24), GCOMP_OK);
  ASSERT_EQ(gcomp_deflate_bitwriter_flush_to_byte(&writer), GCOMP_OK);

  gcomp_deflate_bitreader_t reader;
  uint32_t value = 0;
  ASSERT_EQ(gcomp_deflate_bitreader_init(&reader, buffer,
                gcomp_deflate_bitwriter_bytes_written(&writer)),
      GCOMP_OK);

  ASSERT_EQ(gcomp_deflate_bitreader_read_bits(&reader, 24, &value), GCOMP_OK);
  EXPECT_EQ(value, pattern & 0xFFFFFFu);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
