/**
 * @file test_lzw_core.cpp
 *
 * Unit tests for LZW core: dictionary build/lookup, decode stack,
 * KwKwK case, reset behavior.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <gtest/gtest.h>

extern "C" {
#include "methods/lzw/lzw_core.h"
}

class LzwCoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    alloc_ = gcomp_allocator_default();
    ASSERT_NE(alloc_, nullptr);
  }

  const gcomp_allocator_t * alloc_ = nullptr;
  static constexpr uint32_t CLEAR = 256u;
  static constexpr uint32_t EOI = 257u;
};

TEST_F(LzwCoreTest, DecoderInitDestroy) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  gcomp_status_t s = lzw_core_decoder_init(&dec, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_NE(dec.prefix_code, nullptr);
  EXPECT_NE(dec.append_char, nullptr);
  EXPECT_NE(dec.stack, nullptr);
  EXPECT_EQ(dec.capacity, 4096u);
  EXPECT_EQ(dec.next_code, 258u);
  EXPECT_EQ(dec.has_prev, 0);

  lzw_core_decoder_destroy(&dec);
  EXPECT_EQ(dec.prefix_code, nullptr);
  EXPECT_EQ(dec.append_char, nullptr);
  EXPECT_EQ(dec.stack, nullptr);
}

TEST_F(LzwCoreTest, DecoderInitInvalidArg) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  EXPECT_EQ(lzw_core_decoder_init(nullptr, alloc_, 12, CLEAR, EOI),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(lzw_core_decoder_init(&dec, nullptr, 12, CLEAR, EOI),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(lzw_core_decoder_init(&dec, alloc_, 0, CLEAR, EOI),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(lzw_core_decoder_init(&dec, alloc_, 13, CLEAR, EOI),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(LzwCoreTest, DecoderDecodeLiteral) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  gcomp_status_t s = lzw_core_decoder_init(&dec, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);

  uint8_t out[64];
  size_t used = 0;
  s = lzw_core_decoder_decode(&dec, 65, out, sizeof(out), &used);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 1u);
  EXPECT_EQ(out[0], 0x41); /* 'A' */

  lzw_core_decoder_destroy(&dec);
}

TEST_F(LzwCoreTest, DecoderDecodeSequenceThenNormalCode) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  gcomp_status_t s = lzw_core_decoder_init(&dec, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);

  uint8_t out[64];
  size_t used = 0;

  s = lzw_core_decoder_decode(&dec, 65, out, sizeof(out), &used);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 1u);
  EXPECT_EQ(out[0], 0x41);

  s = lzw_core_decoder_decode(&dec, 66, out, sizeof(out), &used);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 2u);
  EXPECT_EQ(out[1], 0x42);

  /* Code 259 was added as (65, 0x42) = "AB". */
  s = lzw_core_decoder_decode(&dec, 259, out, sizeof(out), &used);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 4u);
  EXPECT_EQ(out[2], 0x41);
  EXPECT_EQ(out[3], 0x42);

  lzw_core_decoder_destroy(&dec);
}

TEST_F(LzwCoreTest, DecoderDecodeKwKwK) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  gcomp_status_t s = lzw_core_decoder_init(&dec, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);

  uint8_t out[64];
  size_t used = 0;

  s = lzw_core_decoder_decode(&dec, 65, out, sizeof(out), &used);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 1u);
  s = lzw_core_decoder_decode(&dec, 66, out, sizeof(out), &used);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 2u);
  /* After 65,66 next_code is 260. Decode 260 (KwKwK): prev="B", first_byte='B',
   * output "BB". */
  s = lzw_core_decoder_decode(&dec, 260, out, sizeof(out), &used);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 4u);
  EXPECT_EQ(out[2], 0x42);
  EXPECT_EQ(out[3], 0x42);
  lzw_core_decoder_destroy(&dec);
}

TEST_F(LzwCoreTest, DecoderDecodeInvalidCode) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  gcomp_status_t s = lzw_core_decoder_init(&dec, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);

  uint8_t out[64];
  size_t used = 0;
  s = lzw_core_decoder_decode(&dec, 300, out, sizeof(out), &used);
  EXPECT_EQ(s, GCOMP_ERR_CORRUPT);

  lzw_core_decoder_destroy(&dec);
}

TEST_F(LzwCoreTest, DecoderKwKwKWithoutPrev) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  gcomp_status_t s = lzw_core_decoder_init(&dec, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);

  uint8_t out[64];
  size_t used = 0;
  /* 258 is next_code but we have no prev — invalid */
  s = lzw_core_decoder_decode(&dec, 258, out, sizeof(out), &used);
  EXPECT_EQ(s, GCOMP_ERR_CORRUPT);

  lzw_core_decoder_destroy(&dec);
}

TEST_F(LzwCoreTest, DecoderReset) {
  lzw_core_decoder_t dec;
  memset(&dec, 0, sizeof(dec));
  gcomp_status_t s = lzw_core_decoder_init(&dec, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);
  uint8_t dummy[8];
  size_t used = 0;
  s = lzw_core_decoder_decode(&dec, 65, dummy, sizeof(dummy), &used);
  ASSERT_EQ(s, GCOMP_OK);
  lzw_core_decoder_reset(&dec, CLEAR, EOI);
  EXPECT_EQ(dec.next_code, 258u);
  EXPECT_EQ(dec.has_prev, 0);

  used = 0;
  s = lzw_core_decoder_decode(&dec, 88, dummy, sizeof(dummy), &used);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(used, 1u);
  EXPECT_EQ(dummy[0], 0x58);

  lzw_core_decoder_destroy(&dec);
}

TEST_F(LzwCoreTest, EncoderInitDestroy) {
  lzw_core_encoder_t enc;
  memset(&enc, 0, sizeof(enc));
  gcomp_status_t s = lzw_core_encoder_init(&enc, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_NE(enc.prefix_code, nullptr);
  EXPECT_EQ(enc.next_code, 258u);
  lzw_core_encoder_destroy(&enc);
}

TEST_F(LzwCoreTest, EncoderFindAdd) {
  lzw_core_encoder_t enc;
  memset(&enc, 0, sizeof(enc));
  gcomp_status_t s = lzw_core_encoder_init(&enc, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);

  uint32_t code = 0;
  EXPECT_EQ(lzw_core_encoder_find(&enc, 65, 66, &code), 0);

  uint32_t added = lzw_core_encoder_add(&enc, 65, 66);
  EXPECT_EQ(added, 258u);
  EXPECT_EQ(lzw_core_encoder_find(&enc, 65, 66, &code), 1);
  EXPECT_EQ(code, 258u);

  lzw_core_encoder_destroy(&enc);
}

TEST_F(LzwCoreTest, EncoderIsFull) {
  lzw_core_encoder_t enc;
  memset(&enc, 0, sizeof(enc));
  gcomp_status_t s = lzw_core_encoder_init(&enc, alloc_, 12, CLEAR, EOI);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_FALSE(lzw_core_encoder_is_full(&enc));
  lzw_core_encoder_destroy(&enc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}