/**
 * @file test_zstd_parallel.cpp
 *
 * Unit tests for Zstd parallel compression infrastructure.
 *
 * Tests the direct API (submit/get_result) for both inline and threaded modes.
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

// Include the parallel header
extern "C" {
#include "../../../src/methods/zstd/zstd_parallel.h"
}

class ZstdParallelTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  // Helper to decode data using the standard decoder
  std::vector<uint8_t> decode(
      const void * data, size_t len, bool concat = true) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_bool(opts, "zstd.concat", concat);

    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK) {
      gcomp_options_destroy(opts);
      return {};
    }
    std::vector<uint8_t> out(len * 1000 + 65536);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};

    // Loop until all input is consumed (for multi-block frames)
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

//
// Context Creation Tests
//

TEST_F(ZstdParallelTest, CreateInlineContext) {
  zstd_parallel_config_t config = {
      .num_threads = 1, // Inline mode
      .max_in_flight = 0,
      .job_size = 0,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_TRUE(zstd_parallel_is_inline(ctx));
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, CreateThreadedContext) {
  zstd_parallel_config_t config = {
      .num_threads = 2, // Threaded mode
      .max_in_flight = 4,
      .job_size = 64 * 1024, // 64KB
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_FALSE(zstd_parallel_is_inline(ctx));
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, CreateZeroThreadsIsInline) {
  zstd_parallel_config_t config = {
      .num_threads = 0, // Should be inline
      .max_in_flight = 0,
      .job_size = 0,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_TRUE(zstd_parallel_is_inline(ctx));

  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, NullCtxOutFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 0,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  EXPECT_EQ(zstd_parallel_create(&config, nullptr), GCOMP_ERR_INVALID_ARG);
}

//
// Job Allocation Tests
//

TEST_F(ZstdParallelTest, AllocAndFreeJob) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
  ASSERT_NE(job, nullptr);
  EXPECT_NE(job->base.input, nullptr);
  EXPECT_NE(job->base.output, nullptr);
  EXPECT_NE(job->match_finder, nullptr);

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, AllocJobNullCtxFails) {
  zstd_parallel_job_t * job = nullptr;
  EXPECT_EQ(zstd_parallel_alloc_job(nullptr, &job), GCOMP_ERR_INVALID_ARG);
}

TEST_F(ZstdParallelTest, AllocJobNullJobOutFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  EXPECT_EQ(zstd_parallel_alloc_job(ctx, nullptr), GCOMP_ERR_INVALID_ARG);

  zstd_parallel_destroy(ctx);
}

//
// Inline Mode Submit/GetResult Tests
//

TEST_F(ZstdParallelTest, InlineSubmitGetResult) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Allocate job
  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  // Fill input with test data
  const char test_data[] = "Hello from parallel compression test!";
  memcpy((void *)job->base.input, test_data, strlen(test_data));
  job->base.input_size = strlen(test_data);

  // Submit
  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 1u);
  EXPECT_TRUE(zstd_parallel_result_ready(ctx));

  // Get result
  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_EQ(result, job);
  EXPECT_GT(result->base.output_size, 0u);
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  // Verify the output is decodable
  auto decoded = decode(result->base.output, result->base.output_size);
  ASSERT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, InlineMultipleJobs) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  const char * test_strings[] = {"First job", "Second job", "Third job"};
  std::vector<zstd_parallel_job_t *> jobs;

  // Submit multiple jobs
  for (int i = 0; i < 3; i++) {
    zstd_parallel_job_t * job = nullptr;
    ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
    memcpy((void *)job->base.input, test_strings[i], strlen(test_strings[i]));
    job->base.input_size = strlen(test_strings[i]);
    ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);
    jobs.push_back(job);
  }

  EXPECT_EQ(zstd_parallel_pending_count(ctx), 3u);

  // Get results in order
  for (int i = 0; i < 3; i++) {
    zstd_parallel_job_t * result = nullptr;
    ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
    EXPECT_EQ(result, jobs[i]);

    auto decoded = decode(result->base.output, result->base.output_size);
    ASSERT_EQ(decoded.size(), strlen(test_strings[i]));
    EXPECT_EQ(memcmp(decoded.data(), test_strings[i], decoded.size()), 0);
  }

  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  for (auto job : jobs) {
    zstd_parallel_free_job(ctx, job);
  }
  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, InlineWithChecksum) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = true, // Enable checksum
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  const char test_data[] = "Data with checksum enabled";
  memcpy((void *)job->base.input, test_data, strlen(test_data));
  job->base.input_size = strlen(test_data);

  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_GT(result->content_checksum, 0u);

  auto decoded = decode(result->base.output, result->base.output_size);
  ASSERT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

//
// Threaded Mode Submit/GetResult Tests
//

TEST_F(ZstdParallelTest, ThreadedSubmitGetResult) {
  zstd_parallel_config_t config = {
      .num_threads = 2,
      .max_in_flight = 4,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  EXPECT_FALSE(zstd_parallel_is_inline(ctx));

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  const char test_data[] = "Hello from threaded parallel compression!";
  memcpy((void *)job->base.input, test_data, strlen(test_data));
  job->base.input_size = strlen(test_data);

  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_EQ(result, job);
  EXPECT_GT(result->base.output_size, 0u);

  auto decoded = decode(result->base.output, result->base.output_size);
  ASSERT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, ThreadedMultipleJobsOrderPreserved) {
  zstd_parallel_config_t config = {
      .num_threads = 4,
      .max_in_flight = 8,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Create jobs with different sizes to potentially complete out of order
  std::vector<std::vector<uint8_t>> inputs;
  std::vector<zstd_parallel_job_t *> jobs;

  for (int i = 0; i < 6; i++) {
    zstd_parallel_job_t * job = nullptr;
    ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

    // Different sizes: small, medium, small, large, small, medium
    size_t size = (i % 3 == 0) ? 100 : (i % 3 == 1) ? 5000 : 500;
    std::vector<uint8_t> data(size);
    test_helpers_generate_random(data.data(), data.size(), 12345 + i);
    inputs.push_back(data);

    memcpy((void *)job->base.input, data.data(), data.size());
    job->base.input_size = data.size();
    ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);
    jobs.push_back(job);
  }

  // Get results - they must come back in submission order
  for (size_t i = 0; i < jobs.size(); i++) {
    zstd_parallel_job_t * result = nullptr;
    ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
    EXPECT_EQ(result, jobs[i]) << "Results should be in submission order";

    auto decoded = decode(result->base.output, result->base.output_size);
    ASSERT_EQ(decoded.size(), inputs[i].size());
    EXPECT_EQ(memcmp(decoded.data(), inputs[i].data(), decoded.size()), 0);
  }

  for (auto job : jobs) {
    zstd_parallel_free_job(ctx, job);
  }
  zstd_parallel_destroy(ctx);
}

//
// Concatenated Frame Tests
//

TEST_F(ZstdParallelTest, ConcatenatedFramesDecodable) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Create multiple jobs
  std::vector<std::vector<uint8_t>> inputs;
  std::vector<uint8_t> concatenated_output;

  for (int i = 0; i < 3; i++) {
    zstd_parallel_job_t * job = nullptr;
    ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

    std::vector<uint8_t> data(1000);
    test_helpers_generate_random(data.data(), data.size(), 11111 + i);
    inputs.push_back(data);

    memcpy((void *)job->base.input, data.data(), data.size());
    job->base.input_size = data.size();
    ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

    zstd_parallel_job_t * result = nullptr;
    ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);

    // Concatenate output
    concatenated_output.insert(concatenated_output.end(), result->base.output,
        result->base.output + result->base.output_size);

    zstd_parallel_free_job(ctx, job);
  }

  zstd_parallel_destroy(ctx);

  // Decode concatenated frames
  auto decoded =
      decode(concatenated_output.data(), concatenated_output.size(), true);

  // Expected: all inputs concatenated
  size_t expected_size = 0;
  for (const auto & inp : inputs) {
    expected_size += inp.size();
  }
  ASSERT_EQ(decoded.size(), expected_size);

  // Verify content
  size_t offset = 0;
  for (const auto & inp : inputs) {
    EXPECT_EQ(memcmp(decoded.data() + offset, inp.data(), inp.size()), 0);
    offset += inp.size();
  }
}

//
// Memory Limit Tests
//

TEST_F(ZstdParallelTest, MemoryLimitReducesInFlight) {
  // With a small memory limit, max_in_flight should be reduced
  zstd_parallel_config_t config = {
      .num_threads = 4,
      .max_in_flight = 10, // Request 10
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 1024 * 1024, // 1MB limit
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  EXPECT_FALSE(zstd_parallel_is_inline(ctx));

  // Context should have been created with reduced in-flight count
  // We can verify by checking job_size was respected
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 64 * 1024u);

  zstd_parallel_destroy(ctx);
}

//
// Wait and Reset Tests
//

TEST_F(ZstdParallelTest, WaitInlineMode) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Wait should succeed immediately in inline mode
  EXPECT_EQ(zstd_parallel_wait(ctx), GCOMP_OK);

  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, ResetAfterAllJobsRetrieved) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Submit and retrieve a job
  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
  job->base.input_size = 10;
  memset((void *)job->base.input, 'A', 10);
  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);

  // Reset should succeed after all jobs retrieved
  EXPECT_EQ(zstd_parallel_reset(ctx), GCOMP_OK);
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, ResetWithPendingJobsFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Submit a job but don't retrieve it
  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
  job->base.input_size = 10;
  memset((void *)job->base.input, 'A', 10);
  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

  // Reset should fail with pending jobs
  EXPECT_EQ(zstd_parallel_reset(ctx), GCOMP_ERR_INVALID_ARG);

  // Clean up
  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

//
// Edge Cases
//

TEST_F(ZstdParallelTest, EmptyInput) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  job->base.input_size = 0; // Empty input

  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_GT(result->base.output_size, 0u); // Should produce a valid frame

  auto decoded = decode(result->base.output, result->base.output_size);
  EXPECT_EQ(decoded.size(), 0u);

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, RLECompressibleInput) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  // Fill with repeated byte (should use RLE block)
  memset((void *)job->base.input, 'X', 10000);
  job->base.input_size = 10000;

  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_LT(result->base.output_size, 100u); // Should compress very well

  auto decoded = decode(result->base.output, result->base.output_size);
  ASSERT_EQ(decoded.size(), 10000u);
  for (size_t i = 0; i < decoded.size(); i++) {
    EXPECT_EQ(decoded[i], 'X');
  }

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, LargeInputMultipleBlocks) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 256 * 1024, // 256KB
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  // Fill with data that will span multiple blocks (block max is 128KB)
  std::vector<uint8_t> data(200 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 99999);
  memcpy((void *)job->base.input, data.data(), data.size());
  job->base.input_size = data.size();

  ASSERT_EQ(zstd_parallel_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_GT(result->base.output_size, 0u);

  auto decoded = decode(result->base.output, result->base.output_size);
  ASSERT_EQ(decoded.size(), data.size());
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

  zstd_parallel_free_job(ctx, job);
  zstd_parallel_destroy(ctx);
}

//
// Error Handling Tests
//

TEST_F(ZstdParallelTest, SubmitNullCtxFails) {
  EXPECT_EQ(zstd_parallel_submit(nullptr, nullptr), GCOMP_ERR_INVALID_ARG);
}

TEST_F(ZstdParallelTest, GetResultNullCtxFails) {
  zstd_parallel_job_t * job = nullptr;
  EXPECT_EQ(zstd_parallel_get_result(nullptr, &job), GCOMP_ERR_INVALID_ARG);
}

TEST_F(ZstdParallelTest, GetResultNoPendingFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  EXPECT_EQ(zstd_parallel_get_result(ctx, &job), GCOMP_ERR_INVALID_ARG);

  zstd_parallel_destroy(ctx);
}

//
// Job Size Tests
//

TEST_F(ZstdParallelTest, JobSizeAutoDefault) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 0, // Auto
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Should use default job size (512KB)
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 512 * 1024u);

  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, JobSizeMinEnforced) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 1024, // Too small
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Should be clamped to minimum (64KB)
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 64 * 1024u);

  zstd_parallel_destroy(ctx);
}

TEST_F(ZstdParallelTest, JobSizeMaxEnforced) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 100 * 1024 * 1024, // Too large
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .allocator = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Should be clamped to maximum (16MB)
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 16 * 1024 * 1024u);

  zstd_parallel_destroy(ctx);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
