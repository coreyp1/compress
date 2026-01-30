/**
 * @file test_stress.cpp
 *
 * Stress and stability tests for the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Memory stability over many iterations (no leaks, no growth)
 * - Correct behavior under stress (rapid create/destroy)
 * - Handling of various input sizes
 * - No degradation over time
 *
 * Note: Some stress tests are parameterized for quick CI runs but can be
 * configured for extended runs via environment variables:
 *   GCOMP_STRESS_ITERATIONS - Number of iterations (default: 100)
 *   GCOMP_STRESS_LARGE_SIZE - Large file size in bytes (default: 1MB)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <random>
#include <vector>

// Get iteration count from environment or use default
static int getStressIterations() {
  const char * env = std::getenv("GCOMP_STRESS_ITERATIONS");
  if (env) {
    int val = std::atoi(env);
    if (val > 0) {
      return val;
    }
  }
  return 100; // Default for quick CI runs
}

// Get large file size from environment or use default
static size_t getLargeSize() {
  const char * env = std::getenv("GCOMP_STRESS_LARGE_SIZE");
  if (env) {
    size_t val = std::strtoull(env, nullptr, 10);
    if (val > 0) {
      return val;
    }
  }
  return 1024 * 1024; // Default 1 MB for quick CI runs
}

class StressTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    iterations_ = getStressIterations();
    large_size_ = getLargeSize();
  }

  // Helper to generate random data
  std::vector<uint8_t> generateRandomData(size_t size, unsigned int seed = 0) {
    std::vector<uint8_t> data(size);
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < size; i++) {
      data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
  }

  // Helper to generate compressible data (repeating patterns)
  std::vector<uint8_t> generateCompressibleData(size_t size) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++) {
      data[i] = static_cast<uint8_t>(i % 64);
    }
    return data;
  }

  // Helper to compress and decompress, verify round-trip
  bool roundTrip(const std::vector<uint8_t> & input, int level = 6) {
    gcomp_options_t * enc_opts = nullptr;
    if (gcomp_options_create(&enc_opts) != GCOMP_OK) {
      return false;
    }
    if (gcomp_options_set_int64(enc_opts, "deflate.level", level) != GCOMP_OK) {
      gcomp_options_destroy(enc_opts);
      return false;
    }

    // Compress
    size_t comp_capacity = input.size() + 1024;
    std::vector<uint8_t> compressed(comp_capacity);
    size_t comp_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "deflate", enc_opts, input.data(),
            input.size(), compressed.data(), comp_capacity, &comp_size);
    gcomp_options_destroy(enc_opts);
    if (status != GCOMP_OK) {
      return false;
    }

    // Decompress with no expansion ratio limit (stress tests use known-good data)
    gcomp_options_t * dec_opts = nullptr;
    if (gcomp_options_create(&dec_opts) != GCOMP_OK) {
      return false;
    }
    // Disable expansion ratio limit for stress tests - data is known-good
    if (gcomp_options_set_uint64(
            dec_opts, "limits.max_expansion_ratio", 0) != GCOMP_OK) {
      gcomp_options_destroy(dec_opts);
      return false;
    }

    size_t decomp_capacity = input.size() + 1024;
    std::vector<uint8_t> decompressed(decomp_capacity);
    size_t decomp_size = 0;

    status = gcomp_decode_buffer(registry_, "deflate", dec_opts, compressed.data(),
        comp_size, decompressed.data(), decomp_capacity, &decomp_size);
    gcomp_options_destroy(dec_opts);

    if (status != GCOMP_OK) {
      return false;
    }

    // Verify
    if (decomp_size != input.size()) {
      return false;
    }
    return memcmp(decompressed.data(), input.data(), input.size()) == 0;
  }

  gcomp_registry_t * registry_ = nullptr;
  int iterations_ = 100;
  size_t large_size_ = 1024 * 1024;
};

//
// Rapid create/destroy cycle tests
//

TEST_F(StressTest, RapidEncoderCreateDestroy) {
  // Create and destroy encoders rapidly
  for (int i = 0; i < iterations_; i++) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "deflate", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at iteration " << i;
    ASSERT_NE(encoder, nullptr);
    gcomp_encoder_destroy(encoder);
  }
}

TEST_F(StressTest, RapidDecoderCreateDestroy) {
  // Create and destroy decoders rapidly
  for (int i = 0; i < iterations_; i++) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "deflate", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at iteration " << i;
    ASSERT_NE(decoder, nullptr);
    gcomp_decoder_destroy(decoder);
  }
}

TEST_F(StressTest, RapidOptionsCreateDestroy) {
  // Create and destroy options rapidly
  for (int i = 0; i < iterations_; i++) {
    gcomp_options_t * opts = nullptr;
    gcomp_status_t status = gcomp_options_create(&opts);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at iteration " << i;
    ASSERT_NE(opts, nullptr);

    // Set some options to exercise the hash table
    EXPECT_EQ(gcomp_options_set_int64(opts, "deflate.level", i % 10), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1024),
        GCOMP_OK);

    gcomp_options_destroy(opts);
  }
}

TEST_F(StressTest, RapidEncoderResetCycles) {
  // Create one encoder, reset it many times
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  std::vector<uint8_t> input = generateCompressibleData(1024);
  std::vector<uint8_t> output(2048);

  for (int i = 0; i < iterations_; i++) {
    // Reset
    status = gcomp_encoder_reset(encoder);
    ASSERT_EQ(status, GCOMP_OK) << "Reset failed at iteration " << i;

    // Encode some data
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(input.data()), input.size(), 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Update failed at iteration " << i;

    out_buf = {output.data() + out_buf.used, output.size() - out_buf.used, 0};
    status = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Finish failed at iteration " << i;
  }

  gcomp_encoder_destroy(encoder);
}

TEST_F(StressTest, RapidDecoderResetCycles) {
  // Prepare compressed data
  std::vector<uint8_t> input = generateCompressibleData(1024);
  size_t comp_capacity = input.size() + 100;
  std::vector<uint8_t> compressed(comp_capacity);
  size_t comp_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(registry_, "deflate", nullptr,
      input.data(), input.size(), compressed.data(), comp_capacity, &comp_size);
  ASSERT_EQ(status, GCOMP_OK);
  compressed.resize(comp_size);

  // Create one decoder, reset it many times
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "deflate", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  std::vector<uint8_t> output(2048);

  for (int i = 0; i < iterations_; i++) {
    // Reset
    status = gcomp_decoder_reset(decoder);
    ASSERT_EQ(status, GCOMP_OK) << "Reset failed at iteration " << i;

    // Decode
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(compressed.data()), compressed.size(), 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Update failed at iteration " << i;

    gcomp_buffer_t finish_buf = {
        output.data() + out_buf.used, output.size() - out_buf.used, 0};
    status = gcomp_decoder_finish(decoder, &finish_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Finish failed at iteration " << i;
  }

  gcomp_decoder_destroy(decoder);
}

//
// Many small compressions
//

TEST_F(StressTest, ManySmallCompressions) {
  // Compress many small files
  int small_iterations = std::min(iterations_ * 10, 1000);

  for (int i = 0; i < small_iterations; i++) {
    // Random size between 1 and 1024 bytes
    size_t size = (i % 1024) + 1;
    std::vector<uint8_t> input = generateRandomData(size, i);

    ASSERT_TRUE(roundTrip(input)) << "Failed at iteration " << i
                                  << " with size " << size;
  }
}

TEST_F(StressTest, ManyEmptyCompressions) {
  // Compress many empty inputs
  std::vector<uint8_t> empty;

  // Create decode options with disabled expansion ratio limit
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0),
      GCOMP_OK);

  for (int i = 0; i < iterations_; i++) {
    size_t comp_capacity = 100;
    std::vector<uint8_t> compressed(comp_capacity);
    size_t comp_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "deflate", nullptr, empty.data(), 0,
            compressed.data(), comp_capacity, &comp_size);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at iteration " << i;
    ASSERT_GT(comp_size, 0); // Deflate always produces some output (empty block)

    // Decompress
    std::vector<uint8_t> decompressed(100);
    size_t decomp_size = 0;

    status = gcomp_decode_buffer(registry_, "deflate", dec_opts, compressed.data(),
        comp_size, decompressed.data(), 100, &decomp_size);
    ASSERT_EQ(status, GCOMP_OK) << "Decompress failed at iteration " << i;
    ASSERT_EQ(decomp_size, 0);
  }

  gcomp_options_destroy(dec_opts);
}

//
// Large file compression
//

TEST_F(StressTest, LargeCompressibleData) {
  // Large highly compressible data (all zeros)
  std::vector<uint8_t> input(large_size_, 0);

  ASSERT_TRUE(roundTrip(input, 9)); // Max compression
}

TEST_F(StressTest, LargeRandomData) {
  // Large random (incompressible) data
  std::vector<uint8_t> input = generateRandomData(large_size_, 12345);

  // Random data doesn't compress well - use custom roundTrip that allocates
  // more space for compressed output
  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(enc_opts, "deflate.level", 1), GCOMP_OK);

  // For random data, we need extra space since it may not compress
  size_t comp_capacity = (input.size() * 12 / 10) + 1024;
  std::vector<uint8_t> compressed(comp_capacity);
  size_t comp_size = 0;

  gcomp_status_t status =
      gcomp_encode_buffer(registry_, "deflate", enc_opts, input.data(),
          input.size(), compressed.data(), comp_capacity, &comp_size);
  gcomp_options_destroy(enc_opts);
  ASSERT_EQ(status, GCOMP_OK) << "Compression failed";

  // Decompress with no expansion ratio limit
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0),
      GCOMP_OK);

  size_t decomp_capacity = input.size() + 1024;
  std::vector<uint8_t> decompressed(decomp_capacity);
  size_t decomp_size = 0;

  status = gcomp_decode_buffer(registry_, "deflate", dec_opts, compressed.data(),
      comp_size, decompressed.data(), decomp_capacity, &decomp_size);
  gcomp_options_destroy(dec_opts);
  ASSERT_EQ(status, GCOMP_OK) << "Decompression failed";

  ASSERT_EQ(decomp_size, input.size());
  ASSERT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(StressTest, LargePatternData) {
  // Large repeating pattern data
  std::vector<uint8_t> input = generateCompressibleData(large_size_);

  ASSERT_TRUE(roundTrip(input, 6)); // Default compression
}

//
// Random input sizes
//

TEST_F(StressTest, RandomInputSizes) {
  std::mt19937 gen(42);
  std::uniform_int_distribution<size_t> size_dist(0, 64 * 1024); // 0 to 64KB
  std::uniform_int_distribution<int> level_dist(0, 9);

  // Create decode options with disabled expansion ratio limit
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0),
      GCOMP_OK);

  for (int i = 0; i < iterations_; i++) {
    size_t size = size_dist(gen);
    int level = level_dist(gen);

    std::vector<uint8_t> input = generateRandomData(size, i);

    // Create options with random level
    gcomp_options_t * enc_opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(enc_opts, "deflate.level", level), GCOMP_OK);

    // Compress - allocate extra space since random data may not compress well
    // and deflate has overhead (header, block markers, etc.)
    // Use size * 1.2 + 1024 to handle worst-case expansion
    size_t comp_capacity = (size * 12 / 10) + 1024;
    std::vector<uint8_t> compressed(std::max(comp_capacity, size_t(1024)));
    size_t comp_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "deflate", enc_opts, input.data(), size,
            compressed.data(), compressed.size(), &comp_size);
    ASSERT_EQ(status, GCOMP_OK)
        << "Compress failed at iteration " << i << " with size " << size
        << " and level " << level;

    // Decompress
    size_t decomp_capacity = size + 1024;
    std::vector<uint8_t> decompressed(std::max(decomp_capacity, size_t(1024)));
    size_t decomp_size = 0;

    status = gcomp_decode_buffer(registry_, "deflate", dec_opts, compressed.data(),
        comp_size, decompressed.data(), decompressed.size(), &decomp_size);
    ASSERT_EQ(status, GCOMP_OK)
        << "Decompress failed at iteration " << i << " with size " << size;

    // Verify
    ASSERT_EQ(decomp_size, size)
        << "Size mismatch at iteration " << i << ": expected " << size
        << ", got " << decomp_size;
    ASSERT_EQ(memcmp(decompressed.data(), input.data(), size), 0)
        << "Data mismatch at iteration " << i;

    gcomp_options_destroy(enc_opts);
  }

  gcomp_options_destroy(dec_opts);
}

//
// Streaming stress tests
//

TEST_F(StressTest, StreamingWithTinyChunks) {
  // Compress with very small output buffers (stress incremental output)
  std::vector<uint8_t> input = generateCompressibleData(10 * 1024);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> compressed;
  compressed.reserve(input.size() + 1024);

  // Feed input in 100-byte chunks, collect output in 32-byte chunks
  size_t input_offset = 0;
  while (input_offset < input.size()) {
    size_t chunk_size = std::min(size_t(100), input.size() - input_offset);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(input.data() + input_offset), chunk_size, 0};

    while (in_buf.used < in_buf.size) {
      uint8_t out_chunk[32];
      gcomp_buffer_t out_buf = {out_chunk, sizeof(out_chunk), 0};

      status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
      ASSERT_EQ(status, GCOMP_OK);

      compressed.insert(compressed.end(), out_chunk, out_chunk + out_buf.used);
    }

    input_offset += chunk_size;
  }

  // Finish
  bool finished = false;
  while (!finished) {
    uint8_t out_chunk[32];
    gcomp_buffer_t out_buf = {out_chunk, sizeof(out_chunk), 0};

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status == GCOMP_OK && out_buf.used == 0) {
      finished = true;
    } else {
      ASSERT_TRUE(status == GCOMP_OK || status == GCOMP_ERR_LIMIT);
      compressed.insert(compressed.end(), out_chunk, out_chunk + out_buf.used);
    }
  }

  gcomp_encoder_destroy(encoder);

  // Verify by decompressing (with no expansion ratio limit)
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0),
      GCOMP_OK);

  size_t decomp_capacity = input.size() + 1024;
  std::vector<uint8_t> decompressed(decomp_capacity);
  size_t decomp_size = 0;

  status = gcomp_decode_buffer(registry_, "deflate", dec_opts, compressed.data(),
      compressed.size(), decompressed.data(), decomp_capacity, &decomp_size);
  gcomp_options_destroy(dec_opts);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp_size, input.size());
  ASSERT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// All compression levels stress test
//

TEST_F(StressTest, AllLevelsRoundTrip) {
  std::vector<uint8_t> input = generateCompressibleData(8 * 1024);

  for (int level = 0; level <= 9; level++) {
    for (int iteration = 0; iteration < 10; iteration++) {
      ASSERT_TRUE(roundTrip(input, level))
          << "Failed at level " << level << ", iteration " << iteration;
    }
  }
}

//
// Concurrent-like access (single-threaded but simulating multiple streams)
//

TEST_F(StressTest, MultipleSimultaneousEncoders) {
  // Create multiple encoders, use them interleaved
  const int num_encoders = 5;
  std::vector<gcomp_encoder_t *> encoders(num_encoders, nullptr);
  std::vector<std::vector<uint8_t>> inputs(num_encoders);
  std::vector<std::vector<uint8_t>> outputs(num_encoders);

  // Create encoders and prepare data
  for (int i = 0; i < num_encoders; i++) {
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "deflate", nullptr, &encoders[i]);
    ASSERT_EQ(status, GCOMP_OK);

    inputs[i] = generateCompressibleData(1024 * (i + 1));
    outputs[i].resize(inputs[i].size() + 1024);
  }

  // Use encoders in round-robin fashion
  std::vector<size_t> input_offsets(num_encoders, 0);
  std::vector<size_t> output_offsets(num_encoders, 0);

  for (int round = 0; round < iterations_; round++) {
    int idx = round % num_encoders;

    if (input_offsets[idx] < inputs[idx].size()) {
      size_t chunk_size =
          std::min(size_t(256), inputs[idx].size() - input_offsets[idx]);

      gcomp_buffer_t in_buf = {
          const_cast<uint8_t *>(inputs[idx].data() + input_offsets[idx]),
          chunk_size, 0};
      gcomp_buffer_t out_buf = {outputs[idx].data() + output_offsets[idx],
          outputs[idx].size() - output_offsets[idx], 0};

      gcomp_status_t status =
          gcomp_encoder_update(encoders[idx], &in_buf, &out_buf);
      ASSERT_EQ(status, GCOMP_OK)
          << "Failed encoder " << idx << " at round " << round;

      input_offsets[idx] += in_buf.used;
      output_offsets[idx] += out_buf.used;
    }
  }

  // Finish all encoders
  for (int i = 0; i < num_encoders; i++) {
    gcomp_buffer_t out_buf = {outputs[i].data() + output_offsets[i],
        outputs[i].size() - output_offsets[i], 0};
    gcomp_status_t status = gcomp_encoder_finish(encoders[i], &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Finish failed for encoder " << i;
    output_offsets[i] += out_buf.used;
  }

  // Verify by decompressing (with no expansion ratio limit)
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0),
      GCOMP_OK);

  for (int i = 0; i < num_encoders; i++) {
    std::vector<uint8_t> decompressed(inputs[i].size() + 1024);
    size_t decomp_size = 0;

    gcomp_status_t status =
        gcomp_decode_buffer(registry_, "deflate", dec_opts, outputs[i].data(),
            output_offsets[i], decompressed.data(), decompressed.size(),
            &decomp_size);
    ASSERT_EQ(status, GCOMP_OK) << "Decompress failed for encoder " << i;
    ASSERT_EQ(decomp_size, inputs[i].size()) << "Size mismatch for encoder " << i;

    gcomp_encoder_destroy(encoders[i]);
  }

  gcomp_options_destroy(dec_opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
