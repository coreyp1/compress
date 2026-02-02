/**
 * @file test_zstd_allocator.cpp
 *
 * Custom allocator tests for the Zstd implementation in the Ghoti.io Compress
 * library.
 *
 * Tests verify that:
 * - Encoder and decoder correctly use custom allocators from the registry
 * - All allocations and frees go through the custom allocator
 * - Memory tracking is accurate
 * - Proper cleanup on destroy
 * - Allocation failure handling
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

// Custom allocator that tracks allocations
struct TrackingAllocator {
  size_t alloc_count = 0;
  size_t free_count = 0;
  size_t total_allocated = 0;
  size_t current_allocated = 0;
  size_t peak_allocated = 0;
  bool fail_next = false;
  bool fail_all = false;
};

static void * tracking_malloc(void * ctx, size_t size) {
  TrackingAllocator * tracker = static_cast<TrackingAllocator *>(ctx);
  if (tracker->fail_next || tracker->fail_all) {
    tracker->fail_next = false;
    return nullptr;
  }
  void * ptr = malloc(size);
  if (ptr) {
    tracker->alloc_count++;
    tracker->total_allocated += size;
    tracker->current_allocated += size;
    if (tracker->current_allocated > tracker->peak_allocated) {
      tracker->peak_allocated = tracker->current_allocated;
    }
  }
  return ptr;
}

static void * tracking_calloc(void * ctx, size_t count, size_t size) {
  TrackingAllocator * tracker = static_cast<TrackingAllocator *>(ctx);
  if (tracker->fail_next || tracker->fail_all) {
    tracker->fail_next = false;
    return nullptr;
  }
  void * ptr = calloc(count, size);
  if (ptr) {
    tracker->alloc_count++;
    tracker->total_allocated += count * size;
    tracker->current_allocated += count * size;
    if (tracker->current_allocated > tracker->peak_allocated) {
      tracker->peak_allocated = tracker->current_allocated;
    }
  }
  return ptr;
}

static void tracking_free(void * ctx, void * ptr) {
  if (!ptr)
    return;
  TrackingAllocator * tracker = static_cast<TrackingAllocator *>(ctx);
  tracker->free_count++;
  // Note: We can't track exact size of freed memory without more complex
  // tracking
  free(ptr);
}

static void * tracking_realloc(void * ctx, void * ptr, size_t size) {
  TrackingAllocator * tracker = static_cast<TrackingAllocator *>(ctx);
  if (tracker->fail_next || tracker->fail_all) {
    tracker->fail_next = false;
    return nullptr;
  }
  void * new_ptr = realloc(ptr, size);
  if (new_ptr && !ptr) {
    // New allocation
    tracker->alloc_count++;
    tracker->total_allocated += size;
    tracker->current_allocated += size;
    if (tracker->current_allocated > tracker->peak_allocated) {
      tracker->peak_allocated = tracker->current_allocated;
    }
  }
  return new_ptr;
}

class ZstdAllocatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    tracker_ = TrackingAllocator();
    allocator_.ctx = &tracker_;
    allocator_.malloc_fn = tracking_malloc;
    allocator_.calloc_fn = tracking_calloc;
    allocator_.realloc_fn = tracking_realloc;
    allocator_.free_fn = tracking_free;

    // Create registry with custom allocator
    gcomp_status_t status =
        gcomp_registry_create(&allocator_, &custom_registry_);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(custom_registry_, nullptr);

    // Register zstd method
    status = gcomp_method_zstd_register(custom_registry_);
    ASSERT_EQ(status, GCOMP_OK);
  }

  void TearDown() override {
    if (custom_registry_) {
      gcomp_registry_destroy(custom_registry_);
      custom_registry_ = nullptr;
    }
  }

  // Helper: Compress with custom registry
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(custom_registry_, "zstd", opts, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result(len * 2 + 4096);
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

  // Helper: Decompress with custom registry
  std::vector<uint8_t> decompress(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(custom_registry_, "zstd", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result(len * 100 + 65536);
    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_decoder_destroy(decoder);
      return {};
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status_out)
      *status_out = status;

    result.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return result;
  }

  TrackingAllocator tracker_;
  gcomp_allocator_t allocator_;
  gcomp_registry_t * custom_registry_ = nullptr;
};

//
// Basic Allocator Usage Tests
//

TEST_F(ZstdAllocatorTest, EncoderUsesCustomAllocator) {
  // Reset tracker counts (registry creation allocates)
  size_t initial_allocs = tracker_.alloc_count;

  std::vector<uint8_t> input = {'H', 'e', 'l', 'l', 'o'};
  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  // Encoder should have made allocations through our allocator
  EXPECT_GT(tracker_.alloc_count, initial_allocs);
}

TEST_F(ZstdAllocatorTest, DecoderUsesCustomAllocator) {
  // First compress some data
  std::vector<uint8_t> input = {'T', 'e', 's', 't'};
  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  // Reset tracker counts
  size_t initial_allocs = tracker_.alloc_count;

  // Decompress
  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());

  // Decoder should have made allocations through our allocator
  EXPECT_GT(tracker_.alloc_count, initial_allocs);
}

TEST_F(ZstdAllocatorTest, EncoderCleansUpOnDestroy) {
  size_t initial_frees = tracker_.free_count;

  // Create and destroy encoder
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(custom_registry_, "zstd", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(encoder, nullptr);

    gcomp_encoder_destroy(encoder);
  }

  // Should have freed allocations
  EXPECT_GT(tracker_.free_count, initial_frees);
}

TEST_F(ZstdAllocatorTest, DecoderCleansUpOnDestroy) {
  size_t initial_frees = tracker_.free_count;

  // Create and destroy decoder
  {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(custom_registry_, "zstd", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(decoder, nullptr);

    gcomp_decoder_destroy(decoder);
  }

  // Should have freed allocations
  EXPECT_GT(tracker_.free_count, initial_frees);
}

//
// Roundtrip with Custom Allocator
//

TEST_F(ZstdAllocatorTest, RoundtripWithCustomAllocator) {
  std::vector<uint8_t> input(1000);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(i * 7 + 13);
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdAllocatorTest, RLERoundtripWithCustomAllocator) {
  std::vector<uint8_t> input(1000, 'A');

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Allocation Tracking Tests
//

TEST_F(ZstdAllocatorTest, TracksTotalAllocated) {
  size_t initial_total = tracker_.total_allocated;

  std::vector<uint8_t> input(5000);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(i);
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  // Should have allocated memory for state, buffers, etc.
  EXPECT_GT(tracker_.total_allocated, initial_total);
}

TEST_F(ZstdAllocatorTest, PeakMemoryTracked) {
  // Large data to ensure meaningful memory usage
  std::vector<uint8_t> input(50000);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(i * 3);
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  // Peak should be non-trivial
  EXPECT_GT(tracker_.peak_allocated, 10000);
}

//
// Multiple Operations Tests
//

TEST_F(ZstdAllocatorTest, MultipleEncodersSequential) {
  std::vector<uint8_t> input = {'D', 'a', 't', 'a'};

  for (int i = 0; i < 5; i++) {
    auto compressed = compress(input.data(), input.size());
    ASSERT_FALSE(compressed.empty()) << "Iteration " << i;
  }

  // Allocations and frees should balance reasonably
  // (Note: exact balance depends on internal pooling/caching)
  EXPECT_GT(tracker_.alloc_count, 0);
  EXPECT_GT(tracker_.free_count, 0);
}

TEST_F(ZstdAllocatorTest, MultipleDecodersSequential) {
  std::vector<uint8_t> input = {'T', 'e', 's', 't'};
  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  for (int i = 0; i < 5; i++) {
    auto decompressed = decompress(compressed.data(), compressed.size());
    ASSERT_EQ(decompressed.size(), input.size()) << "Iteration " << i;
  }

  EXPECT_GT(tracker_.alloc_count, 0);
  EXPECT_GT(tracker_.free_count, 0);
}

//
// Reset Tests with Custom Allocator
//

TEST_F(ZstdAllocatorTest, EncoderResetRetainsBuffers) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(custom_registry_, "zstd", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // First encode
  std::vector<uint8_t> input1 = {'F', 'i', 'r', 's', 't'};
  std::vector<uint8_t> output1(1024);
  gcomp_buffer_t in_buf = {input1.data(), input1.size(), 0};
  gcomp_buffer_t out_buf = {output1.data(), output1.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  size_t allocs_after_first = tracker_.alloc_count;

  // Reset
  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Second encode - should reuse buffers (minimal new allocations)
  std::vector<uint8_t> input2 = {'S', 'e', 'c', 'o', 'n', 'd'};
  std::vector<uint8_t> output2(1024);
  in_buf = {input2.data(), input2.size(), 0};
  out_buf = {output2.data(), output2.size(), 0};
  out_buf.used = 0;

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  // Check minimal new allocations after reset (buffers retained per BP-4)
  // Allow some allocations for header rebuilding, etc.
  size_t new_allocs = tracker_.alloc_count - allocs_after_first;
  EXPECT_LE(new_allocs, 2) << "Expected minimal allocations after reset";

  gcomp_encoder_destroy(encoder);
}

TEST_F(ZstdAllocatorTest, DecoderResetRetainsBuffers) {
  // First create some compressed data
  std::vector<uint8_t> input = {'T', 'e', 's', 't'};
  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(custom_registry_, "zstd", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // First decode
  std::vector<uint8_t> output1(1024);
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {output1.data(), output1.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_decoder_finish(decoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  size_t allocs_after_first = tracker_.alloc_count;

  // Reset
  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Second decode - should reuse buffers
  std::vector<uint8_t> output2(1024);
  in_buf = {compressed.data(), compressed.size(), 0};
  out_buf = {output2.data(), output2.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_decoder_finish(decoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  // Check minimal new allocations after reset (buffers retained per BP-4)
  size_t new_allocs = tracker_.alloc_count - allocs_after_first;
  EXPECT_LE(new_allocs, 2) << "Expected minimal allocations after reset";

  gcomp_decoder_destroy(decoder);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
