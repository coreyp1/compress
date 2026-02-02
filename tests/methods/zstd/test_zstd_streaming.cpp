/**
 * @file test_zstd_streaming.cpp
 *
 * Streaming tests for the zstd compression method.
 * Tests state machine correctness across various chunk boundaries.
 *
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

class ZstdStreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  // Encode with chunked input (feed input in pieces of chunk_size)
  std::vector<uint8_t> encodeChunked(const uint8_t * data, size_t len,
      size_t chunk_size, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    if (gcomp_encoder_create(registry_, "zstd", opts, &enc) != GCOMP_OK)
      return {};

    std::vector<uint8_t> out(len + 1024);
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    size_t offset = 0;

    // Feed input in chunks
    while (offset < len) {
      size_t this_chunk = std::min(chunk_size, len - offset);
      gcomp_buffer_t in = {const_cast<uint8_t *>(data + offset), this_chunk, 0};
      gcomp_status_t status = gcomp_encoder_update(enc, &in, &ob);
      if (status != GCOMP_OK) {
        gcomp_encoder_destroy(enc);
        return {};
      }
      // Verify input was consumed
      offset += in.used;
    }

    if (gcomp_encoder_finish(enc, &ob) != GCOMP_OK) {
      gcomp_encoder_destroy(enc);
      return {};
    }
    gcomp_encoder_destroy(enc);
    out.resize(ob.used);
    return out;
  }

  // Encode with small output buffer (produce output in pieces)
  std::vector<uint8_t> encodeSmallOutput(const uint8_t * data, size_t len,
      size_t out_chunk_size, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    if (gcomp_encoder_create(registry_, "zstd", opts, &enc) != GCOMP_OK)
      return {};

    std::vector<uint8_t> result;
    std::vector<uint8_t> out_buf(out_chunk_size);
    gcomp_buffer_t in = {const_cast<uint8_t *>(data), len, 0};

    // Keep calling update until all input consumed
    while (in.used < in.size) {
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      gcomp_status_t status = gcomp_encoder_update(enc, &in, &ob);
      if (status != GCOMP_OK) {
        gcomp_encoder_destroy(enc);
        return {};
      }
      result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);
    }

    // Finish with small output buffer - keep calling until no more output
    bool done = false;
    while (!done) {
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      gcomp_status_t status = gcomp_encoder_finish(enc, &ob);
      if (status != GCOMP_OK) {
        gcomp_encoder_destroy(enc);
        return {};
      }
      result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);
      // If no output produced, we're done
      if (ob.used == 0) {
        done = true;
      }
    }

    gcomp_encoder_destroy(enc);
    return result;
  }

  // Decode with chunked input (feed input in pieces of chunk_size)
  std::vector<uint8_t> decodeChunked(const uint8_t * data, size_t len,
      size_t chunk_size, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK)
      return {};

    std::vector<uint8_t> result;
    std::vector<uint8_t> out_buf(65536);
    size_t offset = 0;

    // Feed input in chunks
    while (offset < len) {
      size_t this_chunk = std::min(chunk_size, len - offset);
      gcomp_buffer_t in = {const_cast<uint8_t *>(data + offset), this_chunk, 0};
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      gcomp_status_t status = gcomp_decoder_update(dec, &in, &ob);
      if (status != GCOMP_OK) {
        gcomp_decoder_destroy(dec);
        return {};
      }
      result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);
      offset += in.used;
    }

    // Finish
    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    if (gcomp_decoder_finish(dec, &ob) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return {};
    }
    result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);

    gcomp_decoder_destroy(dec);
    return result;
  }

  // Decode with small output buffer
  std::vector<uint8_t> decodeSmallOutput(const uint8_t * data, size_t len,
      size_t out_chunk_size, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK)
      return {};

    std::vector<uint8_t> result;
    std::vector<uint8_t> out_buf(out_chunk_size);
    gcomp_buffer_t in = {const_cast<uint8_t *>(data), len, 0};

    // Keep calling update until all input consumed
    while (in.used < in.size) {
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      gcomp_status_t status = gcomp_decoder_update(dec, &in, &ob);
      if (status != GCOMP_OK) {
        gcomp_decoder_destroy(dec);
        return {};
      }
      result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);
    }

    // Finish with small output buffer - keep calling until no more output
    bool done = false;
    while (!done) {
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      gcomp_status_t status = gcomp_decoder_finish(dec, &ob);
      if (status != GCOMP_OK) {
        gcomp_decoder_destroy(dec);
        return {};
      }
      result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);
      // If no output produced, we're done
      if (ob.used == 0) {
        done = true;
      }
    }

    gcomp_decoder_destroy(dec);
    return result;
  }

  // Simple one-shot encode for reference
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

  // Simple one-shot decode for reference
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

// =============================================================================
// Encoder streaming tests
// =============================================================================

TEST_F(ZstdStreamingTest, Encoder1ByteInputChunks) {
  const char data[] = "Hello, streaming zstd test with chunked input!";
  auto compressed =
      encodeChunked(reinterpret_cast<const uint8_t *>(data), strlen(data), 1);
  ASSERT_GT(compressed.size(), 0u) << "Encoding failed";

  // Verify it can be decoded
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdStreamingTest, Encoder8ByteInputChunks) {
  std::vector<uint8_t> data(1024);
  test_helpers_generate_sequential(data.data(), data.size());

  auto compressed = encodeChunked(data.data(), data.size(), 8);
  ASSERT_GT(compressed.size(), 0u) << "Encoding failed";

  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdStreamingTest, EncoderSmallOutputBuffer) {
  const char data[] = "Testing encoder with small output buffer!";
  auto compressed =
      encodeSmallOutput(reinterpret_cast<const uint8_t *>(data), strlen(data),
          16); // Very small output buffer
  ASSERT_GT(compressed.size(), 0u) << "Encoding failed";

  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdStreamingTest, Encoder1ByteOutputBuffer) {
  const char data[] = "Test";
  auto compressed =
      encodeSmallOutput(reinterpret_cast<const uint8_t *>(data), strlen(data),
          1); // 1-byte output buffer
  ASSERT_GT(compressed.size(), 0u) << "Encoding failed";

  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdStreamingTest, EncoderLargeDataChunkedInput) {
  std::vector<uint8_t> data(10 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 12345);

  // Test with various chunk sizes
  for (size_t chunk_size : {1, 7, 13, 64, 256, 1024}) {
    auto compressed = encodeChunked(data.data(), data.size(), chunk_size);
    ASSERT_GT(compressed.size(), 0u)
        << "Encoding failed with chunk_size=" << chunk_size;

    auto decompressed = decode(compressed.data(), compressed.size());
    ASSERT_EQ(decompressed.size(), data.size())
        << "Size mismatch with chunk_size=" << chunk_size;
    EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0)
        << "Data mismatch with chunk_size=" << chunk_size;
  }
}

// =============================================================================
// Decoder streaming tests
// =============================================================================

TEST_F(ZstdStreamingTest, Decoder1ByteInputChunks) {
  const char data[] = "Hello, streaming zstd test with chunked decoding!";

  // First compress normally
  auto compressed =
      encode(reinterpret_cast<const uint8_t *>(data), strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  // Decode with 1-byte input chunks
  auto decompressed = decodeChunked(compressed.data(), compressed.size(), 1);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdStreamingTest, Decoder8ByteInputChunks) {
  std::vector<uint8_t> data(1024);
  test_helpers_generate_sequential(data.data(), data.size());

  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);

  auto decompressed = decodeChunked(compressed.data(), compressed.size(), 8);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdStreamingTest, DecoderSmallOutputBuffer) {
  const char data[] = "Testing decoder with small output buffer!";

  auto compressed =
      encode(reinterpret_cast<const uint8_t *>(data), strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  // Decode with small output buffer
  auto decompressed =
      decodeSmallOutput(compressed.data(), compressed.size(), 16);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdStreamingTest, Decoder1ByteOutputBuffer) {
  const char data[] = "Test";

  auto compressed =
      encode(reinterpret_cast<const uint8_t *>(data), strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  // Decode with 1-byte output buffer
  auto decompressed =
      decodeSmallOutput(compressed.data(), compressed.size(), 1);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdStreamingTest, DecoderLargeDataChunkedInput) {
  std::vector<uint8_t> data(10 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 54321);

  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);

  // Test with various chunk sizes
  for (size_t chunk_size : {1, 7, 13, 64, 256, 1024}) {
    auto decompressed =
        decodeChunked(compressed.data(), compressed.size(), chunk_size);
    ASSERT_EQ(decompressed.size(), data.size())
        << "Size mismatch with chunk_size=" << chunk_size;
    EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0)
        << "Data mismatch with chunk_size=" << chunk_size;
  }
}

// =============================================================================
// Combined encoder/decoder streaming tests
// =============================================================================

TEST_F(ZstdStreamingTest, ChunkedEncodeThenChunkedDecode) {
  std::vector<uint8_t> data(2048);
  test_helpers_generate_random(data.data(), data.size(), 98765);

  // Encode with chunked input
  auto compressed = encodeChunked(data.data(), data.size(), 37);
  ASSERT_GT(compressed.size(), 0u);

  // Decode with chunked input
  auto decompressed = decodeChunked(compressed.data(), compressed.size(), 43);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdStreamingTest, SmallOutputEncodeThenSmallOutputDecode) {
  std::vector<uint8_t> data(512);
  test_helpers_generate_sequential(data.data(), data.size());

  // Encode with small output buffer
  auto compressed = encodeSmallOutput(data.data(), data.size(), 32);
  ASSERT_GT(compressed.size(), 0u);

  // Decode with small output buffer
  auto decompressed =
      decodeSmallOutput(compressed.data(), compressed.size(), 32);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

// =============================================================================
// RLE block streaming tests (single repeated byte)
// =============================================================================

TEST_F(ZstdStreamingTest, RLEBlockChunkedDecode) {
  // Create highly compressible data (RLE block)
  std::vector<uint8_t> data(10000, 'X');

  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_LT(compressed.size(), 50u) << "Should compress to RLE block";

  // Decode with various chunk sizes
  for (size_t chunk_size : {1, 3, 7, 16}) {
    auto decompressed =
        decodeChunked(compressed.data(), compressed.size(), chunk_size);
    ASSERT_EQ(decompressed.size(), data.size())
        << "RLE decode failed with chunk_size=" << chunk_size;
    EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0)
        << "RLE data mismatch with chunk_size=" << chunk_size;
  }
}

TEST_F(ZstdStreamingTest, RLEBlockSmallOutputDecode) {
  std::vector<uint8_t> data(5000, 'Y');

  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);

  // Decode with small output buffer (forces multiple iterations)
  auto decompressed =
      decodeSmallOutput(compressed.data(), compressed.size(), 64);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

// =============================================================================
// Compressed block streaming tests
// =============================================================================

TEST_F(ZstdStreamingTest, CompressedBlockChunkedDecode) {
  // Create data that will produce a compressed block
  std::vector<uint8_t> data(4096);
  test_helpers_generate_sequential(data.data(), data.size());

  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);

  // Decode with various chunk sizes
  for (size_t chunk_size : {1, 5, 17, 64, 256}) {
    auto decompressed =
        decodeChunked(compressed.data(), compressed.size(), chunk_size);
    ASSERT_EQ(decompressed.size(), data.size())
        << "Compressed block decode failed with chunk_size=" << chunk_size;
    EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0)
        << "Data mismatch with chunk_size=" << chunk_size;
  }
}

// =============================================================================
// State persistence tests
// =============================================================================

TEST_F(ZstdStreamingTest, DecoderStatePersistenceAcrossHeaderChunks) {
  const char data[] = "Testing header parsing across multiple update calls";

  auto compressed =
      encode(reinterpret_cast<const uint8_t *>(data), strlen(data));
  ASSERT_GT(compressed.size(), 4u); // At least magic number

  // Feed just the magic number first (4 bytes), then rest
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  std::vector<uint8_t> result;
  std::vector<uint8_t> out_buf(1024);

  // First: just magic (4 bytes)
  gcomp_buffer_t in1 = {compressed.data(), 4, 0};
  gcomp_buffer_t ob1 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in1, &ob1), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob1.used);

  // Second: rest of data
  gcomp_buffer_t in2 = {compressed.data() + 4, compressed.size() - 4, 0};
  gcomp_buffer_t ob2 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in2, &ob2), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob2.used);

  // Finish
  gcomp_buffer_t ob3 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_finish(dec, &ob3), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob3.used);

  gcomp_decoder_destroy(dec);

  ASSERT_EQ(result.size(), strlen(data));
  EXPECT_EQ(memcmp(result.data(), data, strlen(data)), 0);
}

TEST_F(ZstdStreamingTest, DecoderStatePersistenceAcrossBlockHeaderChunks) {
  std::vector<uint8_t> data(1024);
  test_helpers_generate_sequential(data.data(), data.size());

  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 10u);

  // Feed byte by byte to ensure block header parsing works across calls
  auto decompressed = decodeChunked(compressed.data(), compressed.size(), 1);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

// =============================================================================
// With checksum option streaming tests
// =============================================================================

TEST_F(ZstdStreamingTest, WithChecksumChunkedDecode) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  const char data[] = "Data with checksum, decoded in chunks!";
  auto compressed =
      encode(reinterpret_cast<const uint8_t *>(data), strlen(data), opts);
  ASSERT_GT(compressed.size(), 0u);

  // Decode with 1-byte chunks to test checksum parsing across calls
  auto decompressed = decodeChunked(compressed.data(), compressed.size(), 1);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);

  gcomp_options_destroy(opts);
}

// =============================================================================
// Edge cases
// =============================================================================

TEST_F(ZstdStreamingTest, EmptyInputStreaming) {
  // Encode empty input
  auto compressed = encode(nullptr, 0);
  ASSERT_GT(compressed.size(), 0u) << "Empty input should still produce frame";

  // Decode with chunked input
  auto decompressed = decodeChunked(compressed.data(), compressed.size(), 1);
  EXPECT_EQ(decompressed.size(), 0u);
}

TEST_F(ZstdStreamingTest, SingleByteDataStreaming) {
  uint8_t data = 0x42;

  auto compressed = encodeChunked(&data, 1, 1);
  ASSERT_GT(compressed.size(), 0u);

  auto decompressed = decodeChunked(compressed.data(), compressed.size(), 1);
  ASSERT_EQ(decompressed.size(), 1u);
  EXPECT_EQ(decompressed[0], 0x42);
}

TEST_F(ZstdStreamingTest, ZeroSizeUpdateCalls) {
  const char data[] = "Test data";

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  std::vector<uint8_t> out(256);
  gcomp_buffer_t ob = {out.data(), out.size(), 0};

  // Call update with zero-size input (should be no-op)
  gcomp_buffer_t in_empty = {nullptr, 0, 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in_empty, &ob), GCOMP_OK);

  // Now feed real data
  gcomp_buffer_t in = {const_cast<char *>(data), strlen(data), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);

  gcomp_encoder_destroy(enc);

  // Verify output is valid
  auto decompressed = decode(out.data(), ob.used);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
