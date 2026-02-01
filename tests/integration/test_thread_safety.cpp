/**
 * @file test_thread_safety.cpp
 *
 * Thread safety stress tests for the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Default registry is safe for concurrent reads after initialization
 * - Frozen options can be shared safely across threads
 * - Multiple encoder/decoder instances in parallel don't interfere
 * - Thread isolation for encoder/decoder state
 *
 * Thread safety guarantees (from architecture.md):
 * | Component          | Thread Safety |
 * |--------------------|---------------|
 * | Default registry   | Safe for concurrent reads after initialization |
 * | Custom registries  | Not thread-safe |
 * | Encoders/decoders  | Not thread-safe; use one instance per thread |
 * | Options (frozen)   | Safe to share across threads |
 * | Options (mutable)  | Not thread-safe |
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// Get thread count from environment or use default
static int getThreadCount() {
  const char * env = std::getenv("GCOMP_THREAD_COUNT");
  if (env) {
    int val = std::atoi(env);
    if (val > 0) {
      return val;
    }
  }
  // Default: use hardware concurrency, minimum 4
  unsigned int hw = std::thread::hardware_concurrency();
  return std::max(hw, 4u);
}

// Get iteration count from environment or use default
static int getIterations() {
  const char * env = std::getenv("GCOMP_THREAD_ITERATIONS");
  if (env) {
    int val = std::atoi(env);
    if (val > 0) {
      return val;
    }
  }
  return 100; // Default for quick CI runs
}

class ThreadSafetyTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    num_threads_ = getThreadCount();
    iterations_ = getIterations();
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Generate random data with a specific seed
  std::vector<uint8_t> generateRandomData(size_t size, unsigned int seed) {
    std::vector<uint8_t> data(size);
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < size; i++) {
      data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
  }

  // Helper: Generate compressible data (repeating patterns)
  std::vector<uint8_t> generateCompressibleData(size_t size, unsigned int seed) {
    std::vector<uint8_t> data(size);
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> dis(0, 63); // Limited range for better compression
    for (size_t i = 0; i < size; i++) {
      data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
  }

  gcomp_registry_t * registry_ = nullptr;
  int num_threads_ = 4;
  int iterations_ = 100;
};

//
// Test 1: Concurrent Registry Lookup
//
// Multiple threads looking up methods simultaneously should not fail or
// return incorrect results.
//

TEST_F(ThreadSafetyTest, ConcurrentRegistryLookup) {
  std::atomic<int> errors{0};
  std::atomic<int> total_lookups{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads_; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < iterations_; i++) {
        // Look up deflate
        const gcomp_method_t * method = gcomp_registry_find(registry_, "deflate");
        if (!method) {
          errors++;
        }

        // Look up gzip
        method = gcomp_registry_find(registry_, "gzip");
        if (!method) {
          errors++;
        }

        // Look up lz4
        method = gcomp_registry_find(registry_, "lz4");
        if (!method) {
          errors++;
        }

        // Look up non-existent method (should return NULL consistently)
        method = gcomp_registry_find(registry_, "nonexistent");
        if (method != nullptr) {
          errors++;
        }

        total_lookups += 4;
      }
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0) << "Registry lookups failed in concurrent access";
  EXPECT_EQ(total_lookups.load(), num_threads_ * iterations_ * 4);
}

//
// Test 2: Parallel Encoding with Shared Frozen Options
//
// Multiple threads using separate encoders with the same frozen options
// object should work correctly.
//

TEST_F(ThreadSafetyTest, ParallelEncodingWithSharedOptions) {
  // Create shared frozen options
  gcomp_options_t * shared_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&shared_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(shared_opts, "deflate.level", 6), GCOMP_OK);
  ASSERT_EQ(gcomp_options_freeze(shared_opts), GCOMP_OK);

  std::atomic<int> errors{0};
  std::atomic<int> successful_compressions{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads_; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < iterations_; i++) {
        // Each thread creates its own encoder using the shared options
        gcomp_encoder_t * encoder = nullptr;
        gcomp_status_t status =
            gcomp_encoder_create(registry_, "deflate", shared_opts, &encoder);
        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        // Generate unique data for this thread/iteration
        auto data = generateCompressibleData(1024, t * 10000 + i);
        std::vector<uint8_t> compressed(data.size() + 256);

        gcomp_buffer_t in_buf = {data.data(), data.size(), 0};
        gcomp_buffer_t out_buf = {compressed.data(), compressed.size(), 0};

        status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
        if (status != GCOMP_OK) {
          errors++;
          gcomp_encoder_destroy(encoder);
          continue;
        }

        gcomp_buffer_t finish_buf = {
            compressed.data() + out_buf.used, compressed.size() - out_buf.used, 0};
        status = gcomp_encoder_finish(encoder, &finish_buf);
        if (status != GCOMP_OK) {
          errors++;
          gcomp_encoder_destroy(encoder);
          continue;
        }

        successful_compressions++;
        gcomp_encoder_destroy(encoder);
      }
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  gcomp_options_destroy(shared_opts);

  EXPECT_EQ(errors.load(), 0) << "Parallel encoding with shared options failed";
  EXPECT_EQ(successful_compressions.load(), num_threads_ * iterations_);
}

//
// Test 3: Parallel Decoding with Shared Frozen Options
//
// Multiple threads using separate decoders with the same frozen options
// object should work correctly.
//

TEST_F(ThreadSafetyTest, ParallelDecodingWithSharedOptions) {
  // Prepare compressed data that all threads will decode
  auto original_data = generateCompressibleData(2048, 12345);

  size_t comp_capacity = original_data.size() + 256;
  std::vector<uint8_t> compressed(comp_capacity);
  size_t comp_size = 0;

  ASSERT_EQ(gcomp_encode_buffer(registry_, "deflate", nullptr, original_data.data(),
                original_data.size(), compressed.data(), comp_capacity, &comp_size),
      GCOMP_OK);
  compressed.resize(comp_size);

  // Create shared frozen options for decoding
  gcomp_options_t * shared_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&shared_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(shared_opts, "limits.max_expansion_ratio", 0),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_freeze(shared_opts), GCOMP_OK);

  std::atomic<int> errors{0};
  std::atomic<int> successful_decompressions{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads_; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < iterations_; i++) {
        // Each thread creates its own decoder using the shared options
        gcomp_decoder_t * decoder = nullptr;
        gcomp_status_t status =
            gcomp_decoder_create(registry_, "deflate", shared_opts, &decoder);
        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        // Decompress the shared compressed data
        std::vector<uint8_t> decompressed(original_data.size() + 256);

        gcomp_buffer_t in_buf = {
            const_cast<uint8_t *>(compressed.data()), compressed.size(), 0};
        gcomp_buffer_t out_buf = {decompressed.data(), decompressed.size(), 0};

        status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
        if (status != GCOMP_OK) {
          errors++;
          gcomp_decoder_destroy(decoder);
          continue;
        }

        gcomp_buffer_t finish_buf = {
            decompressed.data() + out_buf.used, decompressed.size() - out_buf.used, 0};
        status = gcomp_decoder_finish(decoder, &finish_buf);
        if (status != GCOMP_OK) {
          errors++;
          gcomp_decoder_destroy(decoder);
          continue;
        }

        // Verify the result
        size_t total_size = out_buf.used + finish_buf.used;
        if (total_size != original_data.size() ||
            memcmp(decompressed.data(), original_data.data(), total_size) != 0) {
          errors++;
        } else {
          successful_decompressions++;
        }

        gcomp_decoder_destroy(decoder);
      }
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  gcomp_options_destroy(shared_opts);

  EXPECT_EQ(errors.load(), 0) << "Parallel decoding with shared options failed";
  EXPECT_EQ(successful_decompressions.load(), num_threads_ * iterations_);
}

//
// Test 4: Thread Isolation - Full Roundtrip Per Thread
//
// Verify that encoder/decoder state is not corrupted when multiple instances
// run in parallel by doing complete roundtrips in each thread.
//

TEST_F(ThreadSafetyTest, ThreadIsolationRoundtrip) {
  std::atomic<int> errors{0};
  std::atomic<int> successful_roundtrips{0};
  std::mutex error_mutex;
  std::vector<std::string> error_messages;

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads_; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < iterations_; i++) {
        // Generate unique data for this thread/iteration
        size_t data_size = 1024 + (t * 100) + (i % 512);
        auto original = generateCompressibleData(data_size, t * 10000 + i);

        // Compress
        size_t comp_capacity = original.size() + 256;
        std::vector<uint8_t> compressed(comp_capacity);
        size_t comp_size = 0;

        gcomp_status_t status =
            gcomp_encode_buffer(registry_, "deflate", nullptr, original.data(),
                original.size(), compressed.data(), comp_capacity, &comp_size);
        if (status != GCOMP_OK) {
          std::lock_guard<std::mutex> lock(error_mutex);
          error_messages.push_back("Compress failed: thread=" + std::to_string(t) +
                                   " iter=" + std::to_string(i));
          errors++;
          continue;
        }

        // Decompress
        gcomp_options_t * dec_opts = nullptr;
        gcomp_options_create(&dec_opts);
        gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0);

        size_t decomp_capacity = original.size() + 256;
        std::vector<uint8_t> decompressed(decomp_capacity);
        size_t decomp_size = 0;

        status = gcomp_decode_buffer(registry_, "deflate", dec_opts,
            compressed.data(), comp_size, decompressed.data(), decomp_capacity,
            &decomp_size);
        gcomp_options_destroy(dec_opts);

        if (status != GCOMP_OK) {
          std::lock_guard<std::mutex> lock(error_mutex);
          error_messages.push_back("Decompress failed: thread=" + std::to_string(t) +
                                   " iter=" + std::to_string(i));
          errors++;
          continue;
        }

        // Verify
        if (decomp_size != original.size()) {
          std::lock_guard<std::mutex> lock(error_mutex);
          error_messages.push_back("Size mismatch: thread=" + std::to_string(t) +
                                   " iter=" + std::to_string(i) +
                                   " expected=" + std::to_string(original.size()) +
                                   " got=" + std::to_string(decomp_size));
          errors++;
          continue;
        }

        if (memcmp(decompressed.data(), original.data(), decomp_size) != 0) {
          std::lock_guard<std::mutex> lock(error_mutex);
          error_messages.push_back("Data mismatch: thread=" + std::to_string(t) +
                                   " iter=" + std::to_string(i));
          errors++;
          continue;
        }

        successful_roundtrips++;
      }
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  // Print any error messages for debugging
  if (!error_messages.empty()) {
    for (const auto & msg : error_messages) {
      ADD_FAILURE() << msg;
    }
  }

  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(successful_roundtrips.load(), num_threads_ * iterations_);
}

//
// Test 5: Multiple Methods in Parallel
//
// Different threads use different compression methods simultaneously.
//

TEST_F(ThreadSafetyTest, MultipleMethodsInParallel) {
  const char * methods[] = {"deflate", "gzip", "lz4"};
  const int num_methods = 3;

  std::atomic<int> errors{0};
  std::atomic<int> successful_roundtrips{0};

  std::vector<std::thread> threads;

  // Create num_threads_ threads, each assigned to a different method
  for (int t = 0; t < num_threads_; t++) {
    const char * method = methods[t % num_methods];

    threads.emplace_back([&, t, method]() {
      for (int i = 0; i < iterations_; i++) {
        // Generate unique data for this thread/iteration
        auto original = generateCompressibleData(2048, t * 10000 + i);

        // Compress
        size_t comp_capacity = original.size() * 2 + 256;
        std::vector<uint8_t> compressed(comp_capacity);
        size_t comp_size = 0;

        gcomp_status_t status =
            gcomp_encode_buffer(registry_, method, nullptr, original.data(),
                original.size(), compressed.data(), comp_capacity, &comp_size);
        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        // Decompress
        gcomp_options_t * dec_opts = nullptr;
        gcomp_options_create(&dec_opts);
        gcomp_options_set_uint64(dec_opts, "limits.max_expansion_ratio", 0);

        size_t decomp_capacity = original.size() + 256;
        std::vector<uint8_t> decompressed(decomp_capacity);
        size_t decomp_size = 0;

        status = gcomp_decode_buffer(registry_, method, dec_opts, compressed.data(),
            comp_size, decompressed.data(), decomp_capacity, &decomp_size);
        gcomp_options_destroy(dec_opts);

        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        // Verify
        if (decomp_size != original.size() ||
            memcmp(decompressed.data(), original.data(), decomp_size) != 0) {
          errors++;
          continue;
        }

        successful_roundtrips++;
      }
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0) << "Multiple methods in parallel failed";
  EXPECT_EQ(successful_roundtrips.load(), num_threads_ * iterations_);
}

//
// Test 6: Encoder/Decoder Create/Destroy Race
//
// Rapidly creating and destroying encoders/decoders in parallel to stress
// test memory allocation and deallocation.
//

TEST_F(ThreadSafetyTest, RapidCreateDestroyParallel) {
  std::atomic<int> errors{0};
  std::atomic<int> successful_ops{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads_; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < iterations_; i++) {
        // Alternate between encoder and decoder based on iteration
        if (i % 2 == 0) {
          gcomp_encoder_t * encoder = nullptr;
          gcomp_status_t status =
              gcomp_encoder_create(registry_, "deflate", nullptr, &encoder);
          if (status != GCOMP_OK) {
            errors++;
            continue;
          }
          gcomp_encoder_destroy(encoder);
        } else {
          gcomp_decoder_t * decoder = nullptr;
          gcomp_status_t status =
              gcomp_decoder_create(registry_, "deflate", nullptr, &decoder);
          if (status != GCOMP_OK) {
            errors++;
            continue;
          }
          gcomp_decoder_destroy(decoder);
        }
        successful_ops++;
      }
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(successful_ops.load(), num_threads_ * iterations_);
}

//
// Test 7: Options Create/Clone/Destroy Race
//
// Rapidly creating, cloning, and destroying options in parallel.
//

TEST_F(ThreadSafetyTest, RapidOptionsOperationsParallel) {
  std::atomic<int> errors{0};
  std::atomic<int> successful_ops{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads_; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < iterations_; i++) {
        // Create options
        gcomp_options_t * opts = nullptr;
        gcomp_status_t status = gcomp_options_create(&opts);
        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        // Set some values
        status = gcomp_options_set_int64(opts, "deflate.level", t % 10);
        if (status != GCOMP_OK) {
          errors++;
          gcomp_options_destroy(opts);
          continue;
        }

        // Clone
        gcomp_options_t * clone = nullptr;
        status = gcomp_options_clone(opts, &clone);
        if (status != GCOMP_OK) {
          errors++;
          gcomp_options_destroy(opts);
          continue;
        }

        // Freeze the clone
        status = gcomp_options_freeze(clone);
        if (status != GCOMP_OK) {
          errors++;
          gcomp_options_destroy(clone);
          gcomp_options_destroy(opts);
          continue;
        }

        // Destroy both
        gcomp_options_destroy(clone);
        gcomp_options_destroy(opts);

        successful_ops++;
      }
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(successful_ops.load(), num_threads_ * iterations_);
}

//
// Test 8: Long-Running Parallel Compression Stress
//
// Extended test with larger data to stress thread safety over longer periods.
//

TEST_F(ThreadSafetyTest, LongRunningParallelCompression) {
  // Use larger data for more thorough testing
  const size_t data_size = 64 * 1024; // 64KB per compression

  std::atomic<int> errors{0};
  std::atomic<uint64_t> total_bytes_processed{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads_; t++) {
    threads.emplace_back([&, t]() {
      // Each thread gets its own encoder to reuse
      gcomp_encoder_t * encoder = nullptr;
      gcomp_status_t status =
          gcomp_encoder_create(registry_, "deflate", nullptr, &encoder);
      if (status != GCOMP_OK) {
        errors++;
        return;
      }

      for (int i = 0; i < iterations_ / 10; i++) {
        // Generate data
        auto data = generateCompressibleData(data_size, t * 10000 + i);
        std::vector<uint8_t> compressed(data_size + 1024);

        gcomp_buffer_t in_buf = {data.data(), data.size(), 0};
        gcomp_buffer_t out_buf = {compressed.data(), compressed.size(), 0};

        // Reset and compress
        status = gcomp_encoder_reset(encoder);
        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        gcomp_buffer_t finish_buf = {
            compressed.data() + out_buf.used, compressed.size() - out_buf.used, 0};
        status = gcomp_encoder_finish(encoder, &finish_buf);
        if (status != GCOMP_OK) {
          errors++;
          continue;
        }

        total_bytes_processed += data_size;
      }

      gcomp_encoder_destroy(encoder);
    });
  }

  for (auto & t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0);
  // Each thread should process (iterations_/10) * data_size bytes
  uint64_t expected = (uint64_t)num_threads_ * (iterations_ / 10) * data_size;
  EXPECT_EQ(total_bytes_processed.load(), expected);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
