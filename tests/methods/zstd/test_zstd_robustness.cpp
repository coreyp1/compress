/**
 * @file test_zstd_robustness.cpp
 *
 * Robustness tests for Zstd encoder/decoder in the Ghoti.io Compress library.
 *
 * These tests verify safe handling of unexpected API call sequences:
 * - finish() before any update()
 * - update() after finish() returned success
 * - Multiple finish() calls
 * - destroy() without calling finish()
 * - update() with zero-size input buffer
 * - update() with zero-size output buffer
 * - reset() mid-stream
 * - NULL pointer arguments
 *
 * All tests verify no crash/UB via ASan+UBSan and valgrind.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class ZstdRobustnessTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data (for creating test input)
  std::vector<uint8_t> compress(const void * data, size_t len) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "zstd", nullptr, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 100 + 256);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Encoder Robustness Tests
//

TEST_F(ZstdRobustnessTest, EncoderFinishBeforeUpdate) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  // Call finish() without any update() - should produce valid empty frame
  std::vector<uint8_t> output(256);
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  gcomp_status_t status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_GT(out_buf.used, 0u); // Should have at least frame header + end mark

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, EncoderUpdateAfterFinish) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  // Normal encode and finish
  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 12345);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);

  // Try update() after finish() - should fail or be ignored
  std::vector<uint8_t> more_input(50);
  test_helpers_generate_random(more_input.data(), more_input.size(), 54321);

  gcomp_buffer_t in_buf2 = {more_input.data(), more_input.size(), 0};
  gcomp_buffer_t out_buf2 = {output.data(), output.size(), 0};

  // Implementation may return error or silently ignore
  gcomp_status_t status = gcomp_encoder_update(encoder, &in_buf2, &out_buf2);
  // Don't crash is the main requirement
  (void)status;

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, EncoderMultipleFinishCalls) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 11111);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);

  // Multiple finish() calls - should not crash
  for (int i = 0; i < 5; i++) {
    gcomp_buffer_t out_buf2 = {output.data(), output.size(), 0};
    gcomp_status_t status = gcomp_encoder_finish(encoder, &out_buf2);
    // May return error or produce no output, but should not crash
    (void)status;
  }

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, EncoderDestroyWithoutFinish) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 22222);

  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);

  // Destroy without finish - should not leak memory
  gcomp_encoder_destroy(encoder);
  // Valgrind will verify no leaks
}

TEST_F(ZstdRobustnessTest, EncoderUpdateZeroSizeInput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {nullptr, 0, 0}; // Zero-size input
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  // Zero-size input should be valid (no-op)
  gcomp_status_t status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Finish should still work
  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, EncoderUpdateZeroSizeOutput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 33333);

  gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {nullptr, 0, 0}; // Zero-size output

  // Should not crash - may buffer internally or return OK/AGAIN
  gcomp_status_t status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Now provide adequate output buffer
  std::vector<uint8_t> output(256);
  out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, EncoderResetMidStream) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  // Start encoding
  std::vector<uint8_t> input(500);
  test_helpers_generate_random(input.data(), input.size(), 44444);

  std::vector<uint8_t> output(1000);
  gcomp_buffer_t in_buf = {input.data(), input.size() / 2, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);

  // Reset mid-stream
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);

  // Should be able to encode fresh stream
  std::vector<uint8_t> new_input(300);
  test_helpers_generate_random(new_input.data(), new_input.size(), 55555);

  gcomp_buffer_t in_buf2 = {new_input.data(), new_input.size(), 0};
  gcomp_buffer_t out_buf2 = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf2, &out_buf2), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf2), GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

//
// Decoder Robustness Tests
//

TEST_F(ZstdRobustnessTest, DecoderFinishBeforeUpdate) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  // Call finish() without any update() - should fail (no frame header)
  std::vector<uint8_t> output(256);
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  gcomp_status_t status = gcomp_decoder_finish(decoder, &out_buf);
  // Expect error because no valid frame was provided
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

TEST_F(ZstdRobustnessTest, DecoderUpdateAfterFinish) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  // Create valid compressed data
  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 66666);
  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  // Normal decode and finish
  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK);

  // Try update() after finish() - should fail or be ignored
  gcomp_buffer_t in_buf2 = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf2 = {output.data(), output.size(), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf2, &out_buf2);
  // Don't crash is the main requirement
  (void)status;

  gcomp_decoder_destroy(decoder);
}

TEST_F(ZstdRobustnessTest, DecoderMultipleFinishCalls) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 77777);
  auto compressed = compress(input.data(), input.size());

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK);

  // Multiple finish() calls - should not crash
  for (int i = 0; i < 5; i++) {
    gcomp_buffer_t out_buf2 = {output.data(), output.size(), 0};
    gcomp_status_t status = gcomp_decoder_finish(decoder, &out_buf2);
    (void)status;
  }

  gcomp_decoder_destroy(decoder);
}

TEST_F(ZstdRobustnessTest, DecoderDestroyWithoutFinish) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 88888);
  auto compressed = compress(input.data(), input.size());

  std::vector<uint8_t> output(2000);
  // Only decode part of the data
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size() / 2, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Destroy without finish - should not leak memory
  gcomp_decoder_destroy(decoder);
}

TEST_F(ZstdRobustnessTest, DecoderUpdateZeroSizeInput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {nullptr, 0, 0}; // Zero-size input
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  // Zero-size input should be valid (no-op)
  gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(out_buf.used, 0u);

  gcomp_decoder_destroy(decoder);
}

TEST_F(ZstdRobustnessTest, DecoderUpdateZeroSizeOutput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 99999);
  auto compressed = compress(input.data(), input.size());

  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {nullptr, 0, 0}; // Zero-size output

  // Should not crash - may buffer internally or return OK with partial consume
  gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  // Input may not be fully consumed
  EXPECT_EQ(status, GCOMP_OK);

  // Now provide adequate output buffer to drain
  std::vector<uint8_t> output(256);
  size_t total_output = 0;

  while (in_buf.used < in_buf.size) {
    gcomp_buffer_t out_buf2 = {output.data(), output.size(), 0};
    status = gcomp_decoder_update(decoder, &in_buf, &out_buf2);
    ASSERT_EQ(status, GCOMP_OK);
    total_output += out_buf2.used;
  }

  gcomp_buffer_t out_buf_final = {output.data(), output.size(), 0};
  status = gcomp_decoder_finish(decoder, &out_buf_final);
  EXPECT_EQ(status, GCOMP_OK);
  total_output += out_buf_final.used;

  EXPECT_EQ(total_output, input.size());

  gcomp_decoder_destroy(decoder);
}

TEST_F(ZstdRobustnessTest, DecoderResetMidStream) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 11111);
  auto compressed = compress(input.data(), input.size());

  // Start decoding
  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size() / 2, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);

  // Reset mid-stream
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  // Should be able to decode fresh stream
  std::vector<uint8_t> new_input(500);
  test_helpers_generate_random(new_input.data(), new_input.size(), 22222);
  auto new_compressed = compress(new_input.data(), new_input.size());

  gcomp_buffer_t in_buf2 = {new_compressed.data(), new_compressed.size(), 0};
  gcomp_buffer_t out_buf2 = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &in_buf2, &out_buf2), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &out_buf2), GCOMP_OK);

  EXPECT_EQ(out_buf2.used, new_input.size());
  EXPECT_EQ(memcmp(output.data(), new_input.data(), new_input.size()), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Immediate Destroy Tests
//

TEST_F(ZstdRobustnessTest, EncoderDestroyImmediately) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  // Destroy immediately without any operations
  gcomp_encoder_destroy(encoder);
  // Valgrind will verify no leaks
}

TEST_F(ZstdRobustnessTest, DecoderDestroyImmediately) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  // Destroy immediately without any operations
  gcomp_decoder_destroy(decoder);
  // Valgrind will verify no leaks
}

//
// Repeated Reset Tests
//

TEST_F(ZstdRobustnessTest, EncoderRepeatedReset) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  // Reset multiple times without any operations
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  }

  // Should still work
  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 33333);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, DecoderRepeatedReset) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  // Reset multiple times without any operations
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);
  }

  // Should still work
  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 44444);
  auto compressed = compress(input.data(), input.size());

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK);

  EXPECT_EQ(out_buf.used, input.size());

  gcomp_decoder_destroy(decoder);
}

//
// Large Number of Small Operations
//

TEST_F(ZstdRobustnessTest, EncoderManySmallUpdates) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> output(10000);
  size_t output_pos = 0;

  // Many small updates
  for (int i = 0; i < 1000; i++) {
    uint8_t byte = static_cast<uint8_t>(i & 0xFF);
    gcomp_buffer_t in_buf = {&byte, 1, 0};
    gcomp_buffer_t out_buf = {
        output.data() + output_pos, output.size() - output_pos, 0};

    gcomp_status_t status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    output_pos += out_buf.used;
  }

  gcomp_buffer_t out_buf = {
      output.data() + output_pos, output.size() - output_pos, 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, DecoderManySmallOutputBuffers) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> input(500);
  test_helpers_generate_random(input.data(), input.size(), 55555);
  auto compressed = compress(input.data(), input.size());

  std::vector<uint8_t> result;
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};

  // Decode with 1-byte output buffer
  while (in_buf.used < in_buf.size) {
    uint8_t byte;
    gcomp_buffer_t out_buf = {&byte, 1, 0};
    gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    if (out_buf.used > 0) {
      result.push_back(byte);
    }
  }

  // Finish with 1-byte output buffer
  bool done = false;
  while (!done) {
    uint8_t byte;
    gcomp_buffer_t out_buf = {&byte, 1, 0};
    gcomp_status_t status = gcomp_decoder_finish(decoder, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    if (out_buf.used > 0) {
      result.push_back(byte);
    }
    else {
      done = true;
    }
  }

  ASSERT_EQ(result.size(), input.size());
  EXPECT_EQ(memcmp(result.data(), input.data(), input.size()), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Stress Tests
//

TEST_F(ZstdRobustnessTest, AlternatingUpdateAndReset) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> input(100);
  std::vector<uint8_t> output(256);

  for (int i = 0; i < 50; i++) {
    test_helpers_generate_random(
        input.data(), input.size(), static_cast<uint32_t>(i * 1000));

    gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  }

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, DecoderAlternatingUpdateAndReset) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> output(256);

  for (int i = 0; i < 50; i++) {
    std::vector<uint8_t> input(100);
    test_helpers_generate_random(
        input.data(), input.size(), static_cast<uint32_t>(i * 1000));
    auto compressed = compress(input.data(), input.size());

    gcomp_buffer_t in_buf = {compressed.data(), compressed.size() / 2, 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    ASSERT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
    ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);
  }

  gcomp_decoder_destroy(decoder);
}

//
// Buffer pointer validation (SAFE-1) tests
//

TEST_F(ZstdRobustnessTest, EncoderNullInputDataWithNonzeroSize) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {nullptr, 100, 0}; // NULL data with nonzero size
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  // Should return error, not crash
  gcomp_status_t status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, EncoderNullOutputDataWithNonzeroSize) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 12345);

  gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {nullptr, 256, 0}; // NULL data with nonzero size

  // Should return error, not crash
  gcomp_status_t status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdRobustnessTest, DecoderNullInputDataWithNonzeroSize) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {nullptr, 100, 0}; // NULL data with nonzero size
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  // Should return error, not crash
  gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

TEST_F(ZstdRobustnessTest, DecoderNullOutputDataWithNonzeroSize) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> input(100);
  test_helpers_generate_random(input.data(), input.size(), 12345);
  auto compressed = compress(input.data(), input.size());

  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {nullptr, 256, 0}; // NULL data with nonzero size

  // Should return error, not crash
  gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
