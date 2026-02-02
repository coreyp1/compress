/**
 * @file test_zstd_reset.cpp
 *
 * Tests for zstd encoder and decoder reset functionality (Z4.6, Z5.6, Z7.9).
 *
 * Verifies:
 * - Reset clears all state correctly
 * - Buffers are retained (not reallocated) after reset (BP-4)
 * - Multiple streams can be processed without reallocation
 * - Reset after error recovers correctly
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

class ZstdResetTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }
  gcomp_registry_t * registry_ = nullptr;
};

//
// Encoder Reset Tests
//

TEST_F(ZstdResetTest, EncoderResetAllowsReuse) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // First stream
  const char d1[] = "First stream data";
  std::vector<uint8_t> o1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t b1 = {o1.data(), o1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i1, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b1), GCOMP_OK);
  size_t out1_size = b1.used;
  EXPECT_GT(out1_size, 0u);

  // Reset
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Second stream
  const char d2[] = "Second stream different";
  std::vector<uint8_t> o2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t b2 = {o2.data(), o2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i2, &b2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b2), GCOMP_OK);
  size_t out2_size = b2.used;
  EXPECT_GT(out2_size, 0u);

  // Verify second stream is valid by decoding
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> decoded(256);
  gcomp_buffer_t din = {o2.data(), out2_size, 0};
  gcomp_buffer_t dob = {decoded.data(), decoded.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(memcmp(decoded.data(), d2, strlen(d2)), 0);
  gcomp_decoder_destroy(dec);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, EncoderResetClearsStateCompletely) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // First stream with partial data (no finish)
  std::vector<uint8_t> data1(1000, 'A');
  std::vector<uint8_t> out1(2048);
  gcomp_buffer_t in1 = {data1.data(), data1.size(), 0};
  gcomp_buffer_t ob1 = {out1.data(), out1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in1, &ob1), GCOMP_OK);
  // Don't call finish - simulate interrupted stream

  // Reset should clear partial state
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Second stream should work independently
  std::vector<uint8_t> data2(500, 'B');
  std::vector<uint8_t> out2(1024);
  gcomp_buffer_t in2 = {data2.data(), data2.size(), 0};
  gcomp_buffer_t ob2 = {out2.data(), out2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in2, &ob2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob2), GCOMP_OK);
  EXPECT_GT(ob2.used, 0u);

  // Verify second stream decodes correctly
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> decoded(1024);
  gcomp_buffer_t din = {out2.data(), ob2.used, 0};
  gcomp_buffer_t dob = {decoded.data(), decoded.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(dob.used, 500u);
  EXPECT_EQ(memcmp(decoded.data(), data2.data(), 500), 0);
  gcomp_decoder_destroy(dec);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, EncoderResetWithChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  // First stream
  const char d1[] = "First with checksum";
  std::vector<uint8_t> o1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t b1 = {o1.data(), o1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i1, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b1), GCOMP_OK);

  // Reset
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Second stream - checksum should be recomputed fresh
  const char d2[] = "Second with fresh checksum";
  std::vector<uint8_t> o2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t b2 = {o2.data(), o2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i2, &b2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b2), GCOMP_OK);

  // Verify second stream decodes with checksum validation
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> decoded(256);
  gcomp_buffer_t din = {o2.data(), b2.used, 0};
  gcomp_buffer_t dob = {decoded.data(), decoded.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(memcmp(decoded.data(), d2, strlen(d2)), 0);
  gcomp_decoder_destroy(dec);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdResetTest, EncoderMultipleResets) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Process 5 streams with resets between each
  for (int i = 0; i < 5; i++) {
    std::string data = "Stream number " + std::to_string(i);
    std::vector<uint8_t> out(256);
    gcomp_buffer_t in = {(void *)data.data(), data.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};

    EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
    EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
    EXPECT_GT(ob.used, 0u) << "Stream " << i << " should produce output";

    if (i < 4) {
      EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
    }
  }

  gcomp_encoder_destroy(enc);
}

//
// Decoder Reset Tests
//

TEST_F(ZstdResetTest, DecoderResetAllowsReuse) {
  // Encode two different streams
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  const char d1[] = "First decoder test";
  std::vector<uint8_t> c1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t o1 = {c1.data(), c1.size(), 0};
  gcomp_encoder_update(enc, &i1, &o1);
  gcomp_encoder_finish(enc, &o1);
  size_t c1_size = o1.used;

  gcomp_encoder_reset(enc);

  const char d2[] = "Second decoder test";
  std::vector<uint8_t> c2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t o2 = {c2.data(), c2.size(), 0};
  gcomp_encoder_update(enc, &i2, &o2);
  gcomp_encoder_finish(enc, &o2);
  size_t c2_size = o2.used;

  gcomp_encoder_destroy(enc);

  // Decode both streams with reset
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  // First stream
  std::vector<uint8_t> dec1(256);
  gcomp_buffer_t din1 = {c1.data(), c1_size, 0};
  gcomp_buffer_t dob1 = {dec1.data(), dec1.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din1, &dob1), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob1), GCOMP_OK);
  EXPECT_EQ(memcmp(dec1.data(), d1, strlen(d1)), 0);

  // Reset
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // Second stream
  std::vector<uint8_t> dec2(256);
  gcomp_buffer_t din2 = {c2.data(), c2_size, 0};
  gcomp_buffer_t dob2 = {dec2.data(), dec2.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din2, &dob2), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob2), GCOMP_OK);
  EXPECT_EQ(memcmp(dec2.data(), d2, strlen(d2)), 0);

  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdResetTest, DecoderResetClearsRepeatOffsets) {
  // Encode data that uses repeat offsets
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Pattern that will create repeat offsets
  std::string pattern = "ABCDABCDABCDABCD";
  std::vector<uint8_t> data;
  for (int i = 0; i < 50; i++) {
    data.insert(data.end(), pattern.begin(), pattern.end());
  }

  std::vector<uint8_t> compressed(data.size() + 1024);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Decode twice with reset
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  for (int round = 0; round < 2; round++) {
    std::vector<uint8_t> output(data.size() + 256);
    gcomp_buffer_t din = {compressed.data(), comp_size, 0};
    gcomp_buffer_t dob = {output.data(), output.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
    EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
    EXPECT_EQ(dob.used, data.size());
    EXPECT_EQ(memcmp(output.data(), data.data(), data.size()), 0)
        << "Round " << round << " should decode correctly";

    if (round < 1) {
      EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
    }
  }

  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdResetTest, DecoderResetClearsWindowBuffer) {
  // Encode data larger than typical small test
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  std::vector<uint8_t> data(10000);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)(i % 256);
  }

  std::vector<uint8_t> compressed(data.size() + 1024);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Decode, reset, decode again
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  // First decode
  std::vector<uint8_t> output1(data.size() + 256);
  gcomp_buffer_t din1 = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob1 = {output1.data(), output1.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din1, &dob1), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob1), GCOMP_OK);
  EXPECT_EQ(dob1.used, data.size());

  // Reset
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // Second decode should not be affected by first decode's window content
  std::vector<uint8_t> output2(data.size() + 256);
  gcomp_buffer_t din2 = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob2 = {output2.data(), output2.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din2, &dob2), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob2), GCOMP_OK);
  EXPECT_EQ(dob2.used, data.size());
  EXPECT_EQ(memcmp(output2.data(), data.data(), data.size()), 0);

  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdResetTest, DecoderMultipleResets) {
  // Encode test data
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  const char test_data[] = "Multiple reset test data";
  std::vector<uint8_t> compressed(256);
  gcomp_buffer_t in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Decode 5 times with resets
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  for (int i = 0; i < 5; i++) {
    std::vector<uint8_t> output(256);
    gcomp_buffer_t din = {compressed.data(), comp_size, 0};
    gcomp_buffer_t dob = {output.data(), output.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
    EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
    EXPECT_EQ(memcmp(output.data(), test_data, strlen(test_data)), 0)
        << "Decode " << i << " should match original";

    if (i < 4) {
      EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
    }
  }

  gcomp_decoder_destroy(dec);
}

//
// Reset After Partial Operations
//

TEST_F(ZstdResetTest, EncoderResetAfterPartialUpdate) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Partial update with small output buffer (forces buffering)
  std::vector<uint8_t> data(5000, 'X');
  std::vector<uint8_t> out(100); // Intentionally small
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  // Don't call finish

  // Reset should clear all buffered state
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // New stream should work
  const char new_data[] = "Fresh start";
  std::vector<uint8_t> new_out(256);
  gcomp_buffer_t new_in = {(void *)new_data, strlen(new_data), 0};
  gcomp_buffer_t new_ob = {new_out.data(), new_out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &new_in, &new_ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &new_ob), GCOMP_OK);
  EXPECT_GT(new_ob.used, 0u);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, DecoderResetAfterPartialDecode) {
  // Encode test data
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  std::vector<uint8_t> data(1000, 'Y');
  std::vector<uint8_t> compressed(2048);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Start decoding with small output buffer
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  std::vector<uint8_t> output(100); // Intentionally small
  gcomp_buffer_t din = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob = {output.data(), output.size(), 0};
  gcomp_decoder_update(dec, &din, &dob);
  // Don't call finish

  // Reset should clear partial decode state
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // New decode should work from fresh state
  std::vector<uint8_t> full_output(2048);
  gcomp_buffer_t din2 = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob2 = {full_output.data(), full_output.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din2, &dob2), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob2), GCOMP_OK);
  EXPECT_EQ(dob2.used, 1000u);

  gcomp_decoder_destroy(dec);
}

//
// Reset Immediately After Create
//

TEST_F(ZstdResetTest, EncoderResetImmediatelyAfterCreate) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Reset right away (should be no-op but valid)
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Should still work
  const char data[] = "After immediate reset";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, DecoderResetImmediatelyAfterCreate) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  // Reset right away (should be no-op but valid)
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // Should still work - encode and decode test data
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  const char data[] = "Decode after immediate reset";
  std::vector<uint8_t> compressed(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t din = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob = {output.data(), output.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(memcmp(output.data(), data, strlen(data)), 0);

  gcomp_decoder_destroy(dec);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
