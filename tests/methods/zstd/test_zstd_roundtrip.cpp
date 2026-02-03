/**
 * @file test_zstd_roundtrip.cpp
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

class ZstdRoundtripTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  std::vector<uint8_t> encode(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    if (gcomp_encoder_create(registry_, "zstd", opts, &enc) != GCOMP_OK)
      return {};
    std::vector<uint8_t> out(len + 1024);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    if (gcomp_encoder_update(enc, &in, &ob) != GCOMP_OK) {
      gcomp_encoder_destroy(enc);
      return {};
    }
    if (gcomp_encoder_finish(enc, &ob) != GCOMP_OK) {
      gcomp_encoder_destroy(enc);
      return {};
    }
    gcomp_encoder_destroy(enc);
    out.resize(ob.used);
    return out;
  }

  std::vector<uint8_t> decode(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK)
      return {};
    std::vector<uint8_t> out(len * 1000 + 65536);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    if (gcomp_decoder_update(dec, &in, &ob) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return {};
    }
    if (gcomp_decoder_finish(dec, &ob) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return {};
    }
    gcomp_decoder_destroy(dec);
    out.resize(ob.used);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdRoundtripTest, BasicRoundtrip) {
  const char data[] = "Hello, Zstd roundtrip test!";
  auto compressed = encode(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdRoundtripTest, EmptyRoundtrip) {
  auto compressed = encode(nullptr, 0);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), 0u);
}

TEST_F(ZstdRoundtripTest, RLERoundtrip) {
  std::vector<uint8_t> data(1000, 'X');
  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_LT(compressed.size(), 50u) << "RLE should compress well";
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdRoundtripTest, LargeRandomRoundtrip) {
  std::vector<uint8_t> data(100 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 54321);
  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdRoundtripTest, WithChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);
  const char data[] = "Data with checksum verification!";
  auto compressed = encode(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdRoundtripTest, SingleByte) {
  uint8_t data = 0x42;
  auto compressed = encode(&data, 1);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), 1u);
  EXPECT_EQ(decompressed[0], 0x42);
}

TEST_F(ZstdRoundtripTest, SequentialData) {
  std::vector<uint8_t> data(1024);
  test_helpers_generate_sequential(data.data(), data.size());
  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdRoundtripTest, VariousCompressionLevels) {
  const char data[] = "Roundtrip with different compression levels.";
  const int levels[] = {1, 3, 9, 19, 22};
  for (int level : levels) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_int64(opts, "zstd.level", level);
    auto compressed = encode(data, strlen(data), opts);
    ASSERT_GT(compressed.size(), 0u) << "level " << level;
    auto decompressed = decode(compressed.data(), compressed.size(), opts);
    ASSERT_EQ(decompressed.size(), strlen(data)) << "level " << level;
    EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0)
        << "level " << level;
    gcomp_options_destroy(opts);
  }
}

TEST_F(ZstdRoundtripTest, VariousWindowSizes) {
  // Use window_log values that are well exercised (encoder window >= 64KB
  // avoids edge cases with very small windows).
  std::vector<uint8_t> data(4096);
  test_helpers_generate_random(data.data(), data.size(), 9999);
  const unsigned window_logs[] = {16, 20};
  for (unsigned wlog : window_logs) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_uint64(opts, "zstd.window_log", wlog);
    auto compressed = encode(data.data(), data.size(), opts);
    ASSERT_GT(compressed.size(), 0u) << "window_log " << wlog;
    auto decompressed = decode(compressed.data(), compressed.size(), nullptr);
    ASSERT_EQ(decompressed.size(), data.size()) << "window_log " << wlog;
    EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0)
        << "window_log " << wlog;
    gcomp_options_destroy(opts);
  }
}

TEST_F(ZstdRoundtripTest, RoundtripWithRawDictionary) {
  // Raw content dictionary (>= 8 bytes, no magic); both sides use same bytes
  const char dict[] = "raw_dict_content!";
  const size_t dict_len = strlen(dict);
  ASSERT_GE(dict_len, 8u);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(opts, "zstd.dictionary", dict, dict_len),
      GCOMP_OK);

  const char data[] = "Data compressed with dictionary context.";
  auto compressed = encode(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size(), opts);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);

  gcomp_options_destroy(opts);
}

// Dictionary error tests — missing dictionary, corrupt dictionary
TEST_F(ZstdRoundtripTest, DictionaryMissing_WhenFrameRequiresDict) {
  // Encode with dictionary so frame header has Dictionary_ID set
  const char dict[] = "raw_dict_content!";
  const size_t dict_len = strlen(dict);
  ASSERT_GE(dict_len, 8u);

  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bytes(enc_opts, "zstd.dictionary", dict, dict_len),
      GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(enc_opts, "zstd.dictionary_id", 1), GCOMP_OK);

  const char data[] = "data";
  std::vector<uint8_t> compressed = encode(data, strlen(data), enc_opts);
  gcomp_options_destroy(enc_opts);
  ASSERT_GT(compressed.size(), 0u);

  // Decode without providing dictionary — must fail with UNSUPPORTED
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(4096);
  gcomp_buffer_t in = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
  EXPECT_EQ(st, GCOMP_ERR_UNSUPPORTED)
      << "Decoder must require dictionary when frame has Dictionary_ID";
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdRoundtripTest, DictionaryCorrupt_InitFails) {
  // Corrupt/short dictionary: parser requires >= 8 bytes for raw; formatted
  // requires valid tables. Short buffer causes parse failure at init.
  uint8_t short_dict[4] = {0x00, 0x01, 0x02, 0x03};

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(
                opts, "zstd.dictionary", short_dict, sizeof(short_dict)),
      GCOMP_OK);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t st = gcomp_decoder_create(registry_, "zstd", opts, &dec);
  EXPECT_NE(st, GCOMP_OK)
      << "Decoder create must fail with corrupt/short dictionary";
  if (dec)
    gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdRoundtripTest, DictionaryCorrupt_FormattedMagicOnly) {
  // Formatted dict magic but no entropy tables — parse fails
  uint8_t magic_only[8];
  magic_only[0] = 0x37;
  magic_only[1] = 0xA4;
  magic_only[2] = 0x30;
  magic_only[3] = 0xEC;
  magic_only[4] = 1;
  magic_only[5] = 0;
  magic_only[6] = 0;
  magic_only[7] = 0;

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(
                opts, "zstd.dictionary", magic_only, sizeof(magic_only)),
      GCOMP_OK);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t st = gcomp_decoder_create(registry_, "zstd", opts, &dec);
  EXPECT_NE(st, GCOMP_OK)
      << "Decoder create must fail with truncated formatted dictionary";
  if (dec)
    gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);
}

// Dictionary ID validation — frame dict_id vs provided dict
TEST_F(ZstdRoundtripTest, DictionaryIdValidation_MatchingDictSucceeds) {
  const char dict[] = "raw_dict_content!";
  const size_t dict_len = strlen(dict);
  ASSERT_GE(dict_len, 8u);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(opts, "zstd.dictionary", dict, dict_len),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "zstd.dictionary_id", 42), GCOMP_OK);

  const char data[] = "Data with dict_id 42.";
  auto compressed = encode(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size(), opts);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(ZstdRoundtripTest, DictionaryIdValidation_NoDictWhenFrameHasIdFails) {
  const char dict[] = "raw_dict_content!";
  const size_t dict_len = strlen(dict);
  ASSERT_GE(dict_len, 8u);

  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bytes(enc_opts, "zstd.dictionary", dict, dict_len),
      GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(enc_opts, "zstd.dictionary_id", 99), GCOMP_OK);

  const char data[] = "data";
  std::vector<uint8_t> compressed = encode(data, strlen(data), enc_opts);
  gcomp_options_destroy(enc_opts);
  ASSERT_GT(compressed.size(), 0u);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(4096);
  gcomp_buffer_t in = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
  EXPECT_EQ(st, GCOMP_ERR_UNSUPPORTED)
      << "Decode without dictionary when frame has dict_id must fail";
  gcomp_decoder_destroy(dec);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
