/**
 * @file test_zstd_encoder.cpp
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include "../../../src/methods/zstd/zstd_internal.h"
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

  // GCOMP_ERR_LIMIT means the remaining output did not fit and finish must be
  // called again; GCOMP_OK means the stream is complete.
  for (;;) {
    gcomp_buffer_t ob = {one_byte, 1, 0};
    gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
    if (ob.used > 0)
      result.insert(result.end(), one_byte, one_byte + ob.used);
    if (st == GCOMP_OK)
      break;
    ASSERT_EQ(st, GCOMP_ERR_LIMIT);
    ASSERT_GT(ob.used, 0u) << "finish made no progress";
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

//
// History across blocks
//
// A Zstandard block holds at most 128 KB (RFC 8878 section 3.1.1), and the
// window size in the frame header is what tells the decoder how far back a
// sequence may point - across as many blocks as fit in it.  The encoder used
// to reset its match finder for every block, so no sequence could reach past
// the start of the block it was in and a repeat further back than 128 KB was
// simply re-emitted as literals.
//

namespace {

// Bytes that cannot be compressed on their own, so any size difference is the
// repeat being found or missed.
std::vector<uint8_t> ZstdNoiseBytes(size_t n, uint32_t seed) {
  std::vector<uint8_t> v;
  v.reserve(n);
  uint32_t x = seed * 2654435761u + 1u;
  while (v.size() < n) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    v.push_back((uint8_t)(x >> 19));
  }
  return v;
}

size_t ZstdEncodeWithWindow(gcomp_registry_t * reg, unsigned window_log,
    const std::vector<uint8_t> & in) {
  gcomp_options_t * opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(opts, "zstd.window_log", window_log),
      GCOMP_OK);
  std::vector<uint8_t> out(in.size() * 2 + 4096);
  size_t used = out.size();
  EXPECT_EQ(gcomp_encode_buffer(reg, "zstd", opts, in.data(), in.size(),
                out.data(), out.size(), &used),
      GCOMP_OK);
  gcomp_options_destroy(opts);

  // Whatever it produced has to read back, since a sequence pointing further
  // back than the declared window would not.
  std::vector<uint8_t> back(in.size() + 64);
  size_t back_used = back.size();
  EXPECT_EQ(gcomp_decode_buffer(reg, "zstd", nullptr, out.data(), used,
                back.data(), back.size(), &back_used),
      GCOMP_OK);
  EXPECT_EQ(back_used, in.size());
  EXPECT_EQ(memcmp(back.data(), in.data(), in.size()), 0);
  return used;
}

} // namespace

TEST_F(ZstdEncoderTest, SequencesReachBackIntoEarlierBlocks) {
  const size_t kPhrase = 4096;
  // Two blocks' worth of filler between the copies, so the repeat is several
  // blocks back and cannot be found inside the block that needs it.
  const size_t kGap = 300 * 1024;

  std::vector<uint8_t> phrase = ZstdNoiseBytes(kPhrase, 11);
  std::vector<uint8_t> filler = ZstdNoiseBytes(kGap, 12);
  std::vector<uint8_t> other = ZstdNoiseBytes(kPhrase, 13);

  std::vector<uint8_t> repeated = phrase;
  repeated.insert(repeated.end(), filler.begin(), filler.end());
  repeated.insert(repeated.end(), phrase.begin(), phrase.end());

  std::vector<uint8_t> distinct = phrase;
  distinct.insert(distinct.end(), filler.begin(), filler.end());
  distinct.insert(distinct.end(), other.begin(), other.end());

  // A 1 MB window covers the whole input, so the repeat is inside what the
  // frame header promises the decoder.
  size_t with_repeat = ZstdEncodeWithWindow(registry_, 20, repeated);
  size_t without = ZstdEncodeWithWindow(registry_, 20, distinct);

  EXPECT_LT(with_repeat + kPhrase / 2, without)
      << with_repeat << " vs " << without;
}

TEST_F(ZstdEncoderTest, SequencesDoNotReachPastTheDeclaredWindow) {
  // The other half of the same requirement: a repeat further back than the
  // declared window must not be pointed at, however visible it is to the
  // encoder, because the decoder is not required to still have it.  The
  // round-trip inside the helper is what enforces this - libzstd and our own
  // decoder both reject a sequence that reaches too far - and the sizes say
  // the repeat was genuinely out of reach rather than merely unused.
  const size_t kPhrase = 4096;
  const size_t kGap = 300 * 1024;

  std::vector<uint8_t> phrase = ZstdNoiseBytes(kPhrase, 21);
  std::vector<uint8_t> filler = ZstdNoiseBytes(kGap, 22);

  std::vector<uint8_t> repeated = phrase;
  repeated.insert(repeated.end(), filler.begin(), filler.end());
  repeated.insert(repeated.end(), phrase.begin(), phrase.end());

  // 128 KB of window against a repeat 304 KB back.
  size_t narrow = ZstdEncodeWithWindow(registry_, 17, repeated);
  size_t wide = ZstdEncodeWithWindow(registry_, 20, repeated);
  EXPECT_GT(narrow, wide + kPhrase / 2) << narrow << " vs " << wide;
}

// A block that matches everything has no literals, and that is not a reason
// to give up on it.  RFC 8878 section 3.1.1.3 lets a Compressed_Block carry
// sequences with an empty Literals_Section, just as it lets one carry
// literals with no sequences.
//
// This became reachable the moment sequences could reach back into earlier
// blocks: on repetitive data the first block leaves nothing for the later
// ones to emit as a literal, and the encoder was storing every one of them
// raw.  A megabyte of words drawn at random from a thirteen-word vocabulary
// went from 189,076 bytes to 833,633 that way.
TEST_F(ZstdEncoderTest, ABlockThatMatchesEverythingIsStillCompressed) {
  // Long enough to need several blocks - a block holds at most 128 KB - and
  // repetitive enough that after the first one there is nothing new to say.
  static const char * words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "then ", "returns ", "home ", "with "};
  const size_t count = sizeof(words) / sizeof(words[0]);
  std::vector<uint8_t> data;
  data.reserve(600u * 1024u);
  uint32_t seed = 12345u;
  while (data.size() < 600u * 1024u) {
    seed = seed * 1103515245u + 12345u;
    const char * w = words[(seed >> 16) % count];
    for (const char * c = w; *c; c++) {
      data.push_back((uint8_t)*c);
    }
  }

  std::vector<uint8_t> out(data.size() * 2 + 4096);
  size_t used = out.size();
  ASSERT_EQ(gcomp_encode_buffer(registry_, "zstd", nullptr, data.data(),
                data.size(), out.data(), out.size(), &used),
      GCOMP_OK);

  std::vector<uint8_t> back(data.size() + 64);
  size_t back_used = back.size();
  ASSERT_EQ(gcomp_decode_buffer(registry_, "zstd", nullptr, out.data(), used,
                back.data(), back.size(), &back_used),
      GCOMP_OK);
  ASSERT_EQ(back_used, data.size());
  ASSERT_EQ(memcmp(back.data(), data.data(), data.size()), 0);

  // Walk the blocks: none of them may be a Raw_Block carrying data.  The
  // check is on the block type rather than on the size, because a raw block
  // is the specific failure - a compressed block that merely came out large
  // would be a different problem and should not be reported as this one.
  size_t p = 4;
  uint8_t fhd = out[p++];
  unsigned fcs_flag = fhd >> 6;
  bool single = ((fhd >> 5) & 1) != 0;
  unsigned did = fhd & 3;
  if (!single) {
    p += 1;
  }
  static const unsigned kDid[4] = {0, 1, 2, 4};
  p += kDid[did];
  p += (fcs_flag == 0) ? (single ? 1u : 0u)
                       : (fcs_flag == 1 ? 2u : (fcs_flag == 2 ? 4u : 8u));

  int raw_blocks = 0;
  int compressed_blocks = 0;
  for (;;) {
    ASSERT_LE(p + 3, used);
    uint32_t h = (uint32_t)out[p] | ((uint32_t)out[p + 1] << 8) |
        ((uint32_t)out[p + 2] << 16);
    p += 3;
    bool last = (h & 1) != 0;
    unsigned type = (h >> 1) & 3;
    size_t size = h >> 3;
    if (type == 0 && size > 0) {
      raw_blocks++;
    }
    if (type == 2) {
      compressed_blocks++;
    }
    p += (type == 1) ? 1u : size;
    if (last || p >= used) {
      break;
    }
  }
  EXPECT_GT(compressed_blocks, 1) << "expected several compressed blocks";
  EXPECT_EQ(raw_blocks, 0) << raw_blocks << " blocks were stored raw";
}

//
// Encoding into a buffer that barely fits
//

// The sequence bitstream writer drains whole bytes out of its 64-bit
// container with one 8-byte store, which touches eight bytes however few are
// actually due.  It may only do that with eight bytes free inside the output
// buffer; within eight bytes of the end it goes back to writing one byte at
// a time.  So the last few bytes of a tight output buffer are a different
// code path from all the rest of the stream, and this is what exercises it.
//
// The contract being checked is the encoder's, not the bit writer's: given
// too little room the encoder must say so.  It must never report success
// with a stream that is short, and must never write past what it was given
// (which is what the sanitizer build makes this test able to see).
class ZstdTightOutputTest : public ::testing::TestWithParam<int> {
protected:
  // Input with enough structure to produce real sequences, real FSE tables
  // and a bitstream of a few hundred bytes -- not an RLE block that never
  // reaches the writer.
  static std::vector<uint8_t> MakeInput() {
    std::vector<uint8_t> in;
    const char * words[] = {"alpha ", "beta ", "gamma ", "delta ",
        "epsilon ", "zeta ", "eta ", "theta "};
    uint32_t r = 12345u;
    while (in.size() < 60000) {
      r = r * 1103515245u + 12345u;
      const char * w = words[(r >> 16) % 8];
      in.insert(in.end(), w, w + strlen(w));
      if (((r >> 8) & 0x1f) == 0) {
        for (int k = 0; k < 40; k++) {
          in.push_back(static_cast<uint8_t>(r >> (k % 24)));
        }
      }
    }
    return in;
  }

  static gcomp_status_t EncodeInto(const std::vector<uint8_t> & in, int level,
      size_t cap, std::vector<uint8_t> & out, size_t * used) {
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_int64(opts, "zstd.level", level);
    out.assign(cap, 0);
    *used = 0;
    gcomp_status_t st = gcomp_encode_buffer(gcomp_registry_default(), "zstd",
        opts, in.data(), in.size(), cap ? out.data() : nullptr, cap, used);
    gcomp_options_destroy(opts);
    return st;
  }
};

// Every capacity from well short of the compressed size to just over it.
// Each one either fails or produces a stream that reads back exactly.
TEST_P(ZstdTightOutputTest, EveryCapacityAroundTheCompressedSizeIsHonest) {
  const int level = GetParam();
  std::vector<uint8_t> in = MakeInput();

  std::vector<uint8_t> roomy;
  size_t exact = 0;
  ASSERT_EQ(EncodeInto(in, level, in.size() * 2 + 4096, roomy, &exact),
      GCOMP_OK);
  ASSERT_GT(exact, 64u);

  // A window around the true size, one byte at a time, so the store's
  // eight-byte guard is crossed in both directions.
  const size_t lo = (exact > 64u) ? exact - 64u : 0u;
  size_t successes = 0;
  for (size_t cap = lo; cap <= exact + 8u; cap++) {
    std::vector<uint8_t> out;
    size_t used = 0;
    gcomp_status_t st = EncodeInto(in, level, cap, out, &used);
    if (st != GCOMP_OK) {
      continue;
    }
    successes++;
    ASSERT_LE(used, cap) << "reported " << used << " bytes into " << cap;

    std::vector<uint8_t> back(in.size() + 64);
    size_t back_len = 0;
    ASSERT_EQ(gcomp_decode_buffer(gcomp_registry_default(), "zstd", nullptr,
                  out.data(), used, back.data(), back.size(), &back_len),
        GCOMP_OK)
        << "capacity " << cap << " reported success but did not decode";
    ASSERT_EQ(back_len, in.size()) << "capacity " << cap;
    ASSERT_EQ(memcmp(back.data(), in.data(), in.size()), 0)
        << "capacity " << cap;
  }
  // The roomiest capacities in the window must have worked, or the test is
  // not testing what it claims to.
  EXPECT_GT(successes, 0u);
}

INSTANTIATE_TEST_SUITE_P(Levels, ZstdTightOutputTest,
    ::testing::Values(1, 3, 6, 9, 19));

// The encoder above never gives the sequence writer a tight buffer: blocks
// are built in a staging buffer that is always at least a whole block wide
// (zstd_encoder.c), and a block whose bitstream came close to filling it
// would lose to a raw block and be thrown away.  So the writer's own bounds
// handling -- the byte-at-a-time tail it falls back to within eight bytes of
// the end, and the overflow it reports when even that runs out -- is not
// reachable from the outside, and the test above does not reach it.
//
// It is still a writer that takes a size and must honour it.  These call it
// directly, with the buffer deliberately too small.
TEST(ZstdSequencesEncodeBounds, RefusesEveryBufferTooSmallToHoldTheStream) {
  // Enough sequences for a bitstream of a few hundred bytes, with varied
  // codes so the tables are real rather than RLE.
  std::vector<zstd_sequence_t> seqs;
  uint32_t r = 987654321u;
  for (int i = 0; i < 600; i++) {
    r = r * 1103515245u + 12345u;
    zstd_sequence_t s;
    s.lit_length = (r >> 3) % 40u;
    s.match_length = 3u + ((r >> 11) % 60u);
    s.match_offset = 1u + ((r >> 17) % 5000u);
    seqs.push_back(s);
  }

  std::vector<uint8_t> roomy(64 * 1024);
  size_t exact = 0;
  ASSERT_EQ(zstd_sequences_encode(seqs.data(), seqs.size(), roomy.data(),
                roomy.size(), &exact),
      GCOMP_OK);
  ASSERT_GT(exact, 64u);

  // Every capacity below the true size must be refused, not truncated.  A
  // heap buffer of exactly `cap` is what lets the sanitizer build see a
  // write past the end; the eight-byte store must give way to the tail loop
  // before that can happen.
  for (size_t cap = 1; cap < exact; cap++) {
    std::vector<uint8_t> tight(cap);
    size_t used = 12345u;
    gcomp_status_t st = zstd_sequences_encode(
        seqs.data(), seqs.size(), tight.data(), cap, &used);
    ASSERT_NE(st, GCOMP_OK) << "capacity " << cap << " of " << exact
                            << " reported success";
  }

  // And the exact size still works, so the refusals above are about room
  // and not about the sequences.
  std::vector<uint8_t> snug(exact);
  size_t used = 0;
  EXPECT_EQ(zstd_sequences_encode(
                seqs.data(), seqs.size(), snug.data(), exact, &used),
      GCOMP_OK);
  EXPECT_EQ(used, exact);
  EXPECT_EQ(memcmp(snug.data(), roomy.data(), exact), 0);
}
