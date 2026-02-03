/**
 * @file test_zstd_encoder.cpp
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

class ZstdEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }
  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdEncoderTest, CreateSuccess) {
  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, BasicEncode) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  const char data[] = "Hello, Zstd!";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, EncodeEmpty) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {nullptr, 0, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, CreateWithInvalidLevel) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 0);
  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdEncoderTest, EncodeWithChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  const char data[] = "Checksum test!";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_TRUE(out[4] & 0x04) << "Checksum flag should be set";
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdEncoderTest, EncodeWithContentSizeInHeader) {
  const char data[] = "Content size in header test.";
  const size_t data_len = strlen(data);
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.content_size", data_len);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, data_len, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);

  // Decoder validates content size when present; roundtrip should succeed
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> decoded(4096);
  gcomp_buffer_t din = {out.data(), ob.used, 0};
  gcomp_buffer_t dout = {decoded.data(), decoded.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dout), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dout), GCOMP_OK);
  EXPECT_EQ(dout.used, data_len);
  EXPECT_EQ(memcmp(decoded.data(), data, data_len), 0);

  gcomp_decoder_destroy(dec);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdEncoderTest, ResetAndReuse) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  const char d1[] = "First";
  std::vector<uint8_t> o1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t b1 = {o1.data(), o1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i1, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
  const char d2[] = "Second";
  std::vector<uint8_t> o2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t b2 = {o2.data(), o2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i2, &b2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b2), GCOMP_OK);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, DestroyWithoutFinish) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  const char data[] = "Unfinished";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, EncodeRLE) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> data(1000, 'A');
  std::vector<uint8_t> out(2000);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_LT(ob.used, 50u) << "RLE should compress well";
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, NullInputWithSize) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {nullptr, 100, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_ERR_INVALID_ARG);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, EncodeWithVariousLevels) {
  const char data[] = "Encode with different compression levels.";
  const int levels[] = {1, 3, 9, 19, 22};
  for (int level : levels) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_int64(opts, "zstd.level", level);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
    std::vector<uint8_t> out(512);
    gcomp_buffer_t in = {(void *)data, strlen(data), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK)
        << "level " << level;
    EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK) << "level " << level;
    EXPECT_GT(ob.used, 0u) << "level " << level;
    gcomp_encoder_destroy(enc);
    gcomp_options_destroy(opts);
  }
}

TEST_F(ZstdEncoderTest, EncodeWithVariousWindowSizes) {
  std::vector<uint8_t> data(4096);
  for (size_t i = 0; i < data.size(); i++)
    data[i] = (uint8_t)(i * 17 + i / 256);
  const unsigned window_logs[] = {16, 20};
  for (unsigned wlog : window_logs) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_uint64(opts, "zstd.window_log", wlog);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
    std::vector<uint8_t> out(data.size() + 256);
    gcomp_buffer_t in = {data.data(), data.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK)
        << "window_log " << wlog;
    EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK)
        << "window_log " << wlog;
    EXPECT_GT(ob.used, 0u) << "window_log " << wlog;
    gcomp_encoder_destroy(enc);
    gcomp_options_destroy(opts);
  }
}

TEST_F(ZstdEncoderTest, Encode1ByteInputChunks) {
  const char data[] = "One byte at a time";
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> out(strlen(data) + 256);
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  size_t offset = 0;
  while (offset < strlen(data)) {
    gcomp_buffer_t in = {(void *)(data + offset), 1, 0};
    gcomp_status_t st = gcomp_encoder_update(enc, &in, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    offset += in.used;
  }
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, Encode1ByteOutputBuffer) {
  const char data[] = "Test";
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> result;
  uint8_t one_byte[1];
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};

  while (in.used < in.size) {
    gcomp_buffer_t ob = {one_byte, 1, 0};
    gcomp_status_t st = gcomp_encoder_update(enc, &in, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    if (ob.used > 0)
      result.insert(result.end(), one_byte, one_byte + ob.used);
  }

  bool done = false;
  while (!done) {
    gcomp_buffer_t ob = {one_byte, 1, 0};
    gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    if (ob.used > 0)
      result.insert(result.end(), one_byte, one_byte + ob.used);
    else
      done = true;
  }
  EXPECT_GT(result.size(), 0u);
  gcomp_encoder_destroy(enc);
}

//
// Parallel Encoding Tests
//

class ZstdParallelEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  // Helper to decode data with concat support
  std::vector<uint8_t> decode(const void * data, size_t len) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_bool(opts, "zstd.concat", true);
    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK) {
      gcomp_options_destroy(opts);
      return {};
    }
    // Use large buffer to handle decompressed data (compressed len * expansion)
    std::vector<uint8_t> out(len * 100 + 65536);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};

    // Loop until all input is consumed
    gcomp_status_t status;
    while (in.used < in.size) {
      status = gcomp_decoder_update(dec, &in, &ob);
      if (status != GCOMP_OK) {
        gcomp_decoder_destroy(dec);
        gcomp_options_destroy(opts);
        return {};
      }
    }

    status = gcomp_decoder_finish(dec, &ob);
    gcomp_decoder_destroy(dec);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    out.resize(ob.used);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdParallelEncoderTest, ParallelEncodeBasic) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "threads.count", 2);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  const char data[] = "Hello, parallel zstd!";
  std::vector<uint8_t> out(4096);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};

  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(in.used, strlen(data));
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);

  // Verify output can be decoded
  auto decoded = decode(out.data(), ob.used);
  ASSERT_EQ(decoded.size(), strlen(data));
  EXPECT_EQ(memcmp(decoded.data(), data, strlen(data)), 0);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdParallelEncoderTest, ParallelEncodeLarge) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "threads.count", 4);
  // Use smaller job size to force multiple jobs
  gcomp_options_set_uint64(opts, "zstd.job_size", 64 * 1024);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  // Create data larger than job size to force multiple jobs
  std::vector<uint8_t> data(200 * 1024); // 200 KB
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)(i * 17 + i / 256);
  }

  std::vector<uint8_t> out(data.size() + 65536);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};

  gcomp_status_t status = gcomp_encoder_update(enc, &in, &ob);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(in.used, data.size());

  status = gcomp_encoder_finish(enc, &ob);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_GT(ob.used, 0u);

  // Verify output can be decoded by our decoder (uses concat mode)
  auto decoded = decode(out.data(), ob.used);
  ASSERT_EQ(decoded.size(), data.size())
      << "Decoded size should match original data size";
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0)
      << "Decoded content should match original data";

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdParallelEncoderTest, ParallelEncodeWithChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "threads.count", 2);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  const char data[] = "Parallel checksum test data!";
  std::vector<uint8_t> out(4096);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};

  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);

  // Verify output can be decoded
  auto decoded = decode(out.data(), ob.used);
  ASSERT_EQ(decoded.size(), strlen(data));
  EXPECT_EQ(memcmp(decoded.data(), data, strlen(data)), 0);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdParallelEncoderTest, ParallelEncodeReset) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "threads.count", 2);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  // First compression
  const char data1[] = "First parallel stream";
  std::vector<uint8_t> out1(4096);
  gcomp_buffer_t in1 = {(void *)data1, strlen(data1), 0};
  gcomp_buffer_t ob1 = {out1.data(), out1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in1, &ob1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob1), GCOMP_OK);

  auto decoded1 = decode(out1.data(), ob1.used);
  ASSERT_EQ(decoded1.size(), strlen(data1));

  // Reset and compress again
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  const char data2[] = "Second parallel stream after reset";
  std::vector<uint8_t> out2(4096);
  gcomp_buffer_t in2 = {(void *)data2, strlen(data2), 0};
  gcomp_buffer_t ob2 = {out2.data(), out2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in2, &ob2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob2), GCOMP_OK);

  auto decoded2 = decode(out2.data(), ob2.used);
  ASSERT_EQ(decoded2.size(), strlen(data2));
  EXPECT_EQ(memcmp(decoded2.data(), data2, strlen(data2)), 0);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdParallelEncoderTest, ParallelEncodeEmpty) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "threads.count", 2);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  std::vector<uint8_t> out(4096);
  gcomp_buffer_t in = {nullptr, 0, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};

  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);

  // Empty input should produce empty or minimal output
  // (no jobs submitted, just finish returns OK with no output)

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdParallelEncoderTest, ParallelDestroyWithoutFinish) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "threads.count", 2);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  const char data[] = "Unfinished parallel data";
  std::vector<uint8_t> out(4096);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};

  gcomp_encoder_update(enc, &in, &ob);
  // Destroy without calling finish - should not crash or leak
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
