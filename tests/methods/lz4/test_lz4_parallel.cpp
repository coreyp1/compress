/**
 * @file test_lz4_parallel.cpp
 *
 * Unit tests for LZ4 parallel block compression in the Ghoti.io Compress
 * library.
 *
 * These tests verify:
 * - Parallel context creation
 * - Encoder integration with independent blocks (parallel-friendly)
 *
 * Note: Some direct API tests are disabled until the parallel submit/get_result
 * API is fully implemented. The encoder integration tests verify the encoder
 * works correctly with parallel-friendly settings.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
// Include internal header for direct testing (relative to tests/methods/lz4/)
#include "../../../src/methods/lz4/lz4_parallel.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture for direct lz4_parallel API testing
//

class Lz4ParallelTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Nothing to set up
  }

  void TearDown() override {
    // Nothing to tear down
  }
};

//
// Context Creation Tests
//

TEST_F(Lz4ParallelTest, CreateInlineContext) {
  lz4_parallel_config_t config = {};
  config.num_threads = 0; // Inline mode
  config.block_max_size = 65536;
  config.block_checksum = false;
  config.max_memory_bytes = 0; // No limit

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(ctx, nullptr);

  EXPECT_TRUE(lz4_parallel_is_inline(ctx));
  EXPECT_EQ(lz4_parallel_pending_count(ctx), 0U);

  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, CreateWithOneThread) {
  lz4_parallel_config_t config = {};
  config.num_threads = 1; // Still inline
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(ctx, nullptr);

  EXPECT_TRUE(lz4_parallel_is_inline(ctx));

  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, CreateWithTwoThreads) {
  lz4_parallel_config_t config = {};
  config.num_threads = 2;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(ctx, nullptr);

  EXPECT_FALSE(lz4_parallel_is_inline(ctx));
  EXPECT_EQ(lz4_parallel_pending_count(ctx), 0U);

  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, CreateWithFourThreads) {
  lz4_parallel_config_t config = {};
  config.num_threads = 4;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(ctx, nullptr);

  EXPECT_FALSE(lz4_parallel_is_inline(ctx));

  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, CreateWithNullCtxOutFails) {
  lz4_parallel_config_t config = {};
  config.num_threads = 2;
  config.block_max_size = 65536;

  gcomp_status_t status = lz4_parallel_create(&config, nullptr);
  EXPECT_NE(status, GCOMP_OK);
}

//
// Direct API Tests (submit/get_result)
//

TEST_F(Lz4ParallelTest, InlineModeSubmitGetResult) {
  lz4_parallel_config_t config = {};
  config.num_threads = 0; // Inline mode
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_TRUE(lz4_parallel_is_inline(ctx));

  // Allocate a job
  lz4_parallel_job_t * job = nullptr;
  status = lz4_parallel_alloc_job(ctx, &job);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(job, nullptr);

  // Fill input with test data
  const char * test_data = "Hello, LZ4 parallel compression!";
  size_t data_len = strlen(test_data);
  memcpy(const_cast<uint8_t *>(job->base.input), test_data, data_len);
  job->base.input_size = data_len;

  // Submit - should complete inline
  status = lz4_parallel_submit(ctx, job);
  ASSERT_EQ(status, GCOMP_OK);

  // Check pending count
  EXPECT_EQ(lz4_parallel_pending_count(ctx), 1U);
  EXPECT_TRUE(lz4_parallel_result_ready(ctx));

  // Get result
  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(result, job);
  EXPECT_EQ(result->base.status, GCOMP_JOB_COMPLETE);
  EXPECT_EQ(result->base.result, GCOMP_OK);
  EXPECT_GT(result->base.output_size, 0U);

  // Pending count should be 0 now
  EXPECT_EQ(lz4_parallel_pending_count(ctx), 0U);
  EXPECT_FALSE(lz4_parallel_result_ready(ctx));

  // Cleanup
  lz4_parallel_free_job(ctx, job);
  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, InlineModeMultipleJobs) {
  lz4_parallel_config_t config = {};
  config.num_threads = 1; // Still inline
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_TRUE(lz4_parallel_is_inline(ctx));

  // Submit multiple jobs
  const int num_jobs = 5;
  lz4_parallel_job_t * jobs[num_jobs];

  for (int i = 0; i < num_jobs; i++) {
    status = lz4_parallel_alloc_job(ctx, &jobs[i]);
    ASSERT_EQ(status, GCOMP_OK);

    // Fill with unique data
    char data[64];
    snprintf(data, sizeof(data), "Block %d test data", i);
    size_t len = strlen(data);
    memcpy(const_cast<uint8_t *>(jobs[i]->base.input), data, len);
    jobs[i]->base.input_size = len;

    status = lz4_parallel_submit(ctx, jobs[i]);
    ASSERT_EQ(status, GCOMP_OK);
  }

  // All jobs should be pending
  EXPECT_EQ(lz4_parallel_pending_count(ctx), (uint32_t)num_jobs);

  // Retrieve in order
  for (int i = 0; i < num_jobs; i++) {
    lz4_parallel_job_t * result = nullptr;
    status = lz4_parallel_get_result(ctx, &result);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_EQ(result, jobs[i]); // Should be in submission order
    EXPECT_EQ(result->base.status, GCOMP_JOB_COMPLETE);
  }

  EXPECT_EQ(lz4_parallel_pending_count(ctx), 0U);

  // Cleanup
  for (int i = 0; i < num_jobs; i++) {
    lz4_parallel_free_job(ctx, jobs[i]);
  }
  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, InlineModeWithBlockChecksum) {
  lz4_parallel_config_t config = {};
  config.num_threads = 0;
  config.block_max_size = 65536;
  config.block_checksum = true; // Enable block checksums

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);

  lz4_parallel_job_t * job = nullptr;
  status = lz4_parallel_alloc_job(ctx, &job);
  ASSERT_EQ(status, GCOMP_OK);

  // Fill input
  const char * test_data = "Data for checksum test";
  size_t data_len = strlen(test_data);
  memcpy(const_cast<uint8_t *>(job->base.input), test_data, data_len);
  job->base.input_size = data_len;

  status = lz4_parallel_submit(ctx, job);
  ASSERT_EQ(status, GCOMP_OK);

  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  ASSERT_EQ(status, GCOMP_OK);

  // Checksum should be non-zero
  EXPECT_NE(result->block_checksum, 0U);

  lz4_parallel_free_job(ctx, job);
  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, GetResultWithNoPendingJobsFails) {
  lz4_parallel_config_t config = {};
  config.num_threads = 0;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);

  // Try to get result with no jobs pending
  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  EXPECT_NE(status, GCOMP_OK);
  EXPECT_EQ(result, nullptr);

  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, ResetAfterAllJobsRetrieved) {
  lz4_parallel_config_t config = {};
  config.num_threads = 0;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);

  // Submit and retrieve a job
  lz4_parallel_job_t * job = nullptr;
  status = lz4_parallel_alloc_job(ctx, &job);
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Reset test";
  memcpy(const_cast<uint8_t *>(job->base.input), data, strlen(data));
  job->base.input_size = strlen(data);

  status = lz4_parallel_submit(ctx, job);
  ASSERT_EQ(status, GCOMP_OK);

  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset should succeed
  status = lz4_parallel_reset(ctx);
  EXPECT_EQ(status, GCOMP_OK);

  lz4_parallel_free_job(ctx, job);
  lz4_parallel_destroy(ctx);
}

//
// Multi-threaded Mode Tests
//

TEST_F(Lz4ParallelTest, ThreadedModeSubmitGetResult) {
  lz4_parallel_config_t config = {};
  config.num_threads = 2;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_FALSE(lz4_parallel_is_inline(ctx));

  // Allocate a job
  lz4_parallel_job_t * job = nullptr;
  status = lz4_parallel_alloc_job(ctx, &job);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(job, nullptr);

  // Fill input with test data
  const char * test_data = "Hello, threaded LZ4 compression!";
  size_t data_len = strlen(test_data);
  memcpy(const_cast<uint8_t *>(job->base.input), test_data, data_len);
  job->base.input_size = data_len;

  // Submit
  status = lz4_parallel_submit(ctx, job);
  ASSERT_EQ(status, GCOMP_OK);

  // Get result (may block until job completes)
  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(result, job);
  EXPECT_EQ(result->base.status, GCOMP_JOB_COMPLETE);
  EXPECT_EQ(result->base.result, GCOMP_OK);
  EXPECT_GT(result->base.output_size, 0U);

  // Cleanup
  lz4_parallel_free_job(ctx, job);
  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, ThreadedModeMultipleJobs) {
  lz4_parallel_config_t config = {};
  config.num_threads = 4;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_FALSE(lz4_parallel_is_inline(ctx));

  // Submit multiple jobs
  const int num_jobs = 8;
  lz4_parallel_job_t * jobs[num_jobs];

  for (int i = 0; i < num_jobs; i++) {
    status = lz4_parallel_alloc_job(ctx, &jobs[i]);
    ASSERT_EQ(status, GCOMP_OK);

    // Fill with unique data
    char data[128];
    snprintf(
        data, sizeof(data), "Block %d test data for parallel compression", i);
    size_t len = strlen(data);
    memcpy(const_cast<uint8_t *>(jobs[i]->base.input), data, len);
    jobs[i]->base.input_size = len;

    status = lz4_parallel_submit(ctx, jobs[i]);
    ASSERT_EQ(status, GCOMP_OK);
  }

  // Retrieve in order - should get them in sequence order regardless of
  // completion order
  for (int i = 0; i < num_jobs; i++) {
    lz4_parallel_job_t * result = nullptr;
    status = lz4_parallel_get_result(ctx, &result);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_EQ(result, jobs[i]); // Should be in submission order
    EXPECT_EQ(result->base.status, GCOMP_JOB_COMPLETE);
    EXPECT_EQ(result->base.result, GCOMP_OK);
  }

  // Cleanup
  for (int i = 0; i < num_jobs; i++) {
    lz4_parallel_free_job(ctx, jobs[i]);
  }
  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, ThreadedModeWithBlockChecksum) {
  lz4_parallel_config_t config = {};
  config.num_threads = 2;
  config.block_max_size = 65536;
  config.block_checksum = true;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);

  lz4_parallel_job_t * job = nullptr;
  status = lz4_parallel_alloc_job(ctx, &job);
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Checksum test in threaded mode";
  memcpy(const_cast<uint8_t *>(job->base.input), test_data, strlen(test_data));
  job->base.input_size = strlen(test_data);

  status = lz4_parallel_submit(ctx, job);
  ASSERT_EQ(status, GCOMP_OK);

  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  ASSERT_EQ(status, GCOMP_OK);

  // Checksum should be non-zero
  EXPECT_NE(result->block_checksum, 0U);

  lz4_parallel_free_job(ctx, job);
  lz4_parallel_destroy(ctx);
}

//
// Error Propagation Tests
//

TEST_F(Lz4ParallelTest, InlineModeErrorPropagation) {
  lz4_parallel_config_t config = {};
  config.num_threads = 0; // Inline mode
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_TRUE(lz4_parallel_is_inline(ctx));

  // Allocate a job
  lz4_parallel_job_t * job = nullptr;
  status = lz4_parallel_alloc_job(ctx, &job);
  ASSERT_EQ(status, GCOMP_OK);

  // Fill with random/incompressible data that will expand when compressed
  for (size_t i = 0; i < 1000; i++) {
    const_cast<uint8_t *>(job->base.input)[i] = (uint8_t)(i * 17 + 31);
  }
  job->base.input_size = 1000;

  // Sabotage the output buffer capacity to be too small to store uncompressed
  // This will trigger GCOMP_ERR_LIMIT when compression expands and fallback
  // to uncompressed storage fails
  job->base.output_capacity = 100; // Too small for 1000 byte input

  // Submit - should complete inline but with error
  status = lz4_parallel_submit(ctx, job);
  ASSERT_EQ(status, GCOMP_OK); // Submit itself succeeds

  // Get result - should propagate the job's error
  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT); // Error should be propagated
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->base.status, GCOMP_JOB_ERROR);
  EXPECT_EQ(result->base.result, GCOMP_ERR_LIMIT);

  lz4_parallel_free_job(ctx, job);
  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, ThreadedModeErrorPropagation) {
  lz4_parallel_config_t config = {};
  config.num_threads = 2;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_FALSE(lz4_parallel_is_inline(ctx));

  // Allocate a job
  lz4_parallel_job_t * job = nullptr;
  status = lz4_parallel_alloc_job(ctx, &job);
  ASSERT_EQ(status, GCOMP_OK);

  // Fill with random/incompressible data
  for (size_t i = 0; i < 1000; i++) {
    const_cast<uint8_t *>(job->base.input)[i] = (uint8_t)(i * 17 + 31);
  }
  job->base.input_size = 1000;

  // Sabotage the output buffer capacity
  job->base.output_capacity = 100; // Too small

  // Submit
  status = lz4_parallel_submit(ctx, job);
  ASSERT_EQ(status, GCOMP_OK);

  // Get result - should propagate the job's error
  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->base.status, GCOMP_JOB_ERROR);
  EXPECT_EQ(result->base.result, GCOMP_ERR_LIMIT);

  lz4_parallel_free_job(ctx, job);
  lz4_parallel_destroy(ctx);
}

TEST_F(Lz4ParallelTest, MixedSuccessAndFailureJobs) {
  lz4_parallel_config_t config = {};
  config.num_threads = 2;
  config.block_max_size = 65536;

  lz4_parallel_ctx_t * ctx = nullptr;
  gcomp_status_t status = lz4_parallel_create(&config, &ctx);
  ASSERT_EQ(status, GCOMP_OK);

  // Submit 3 jobs: success, failure, success
  lz4_parallel_job_t * jobs[3];

  // Job 0: will succeed
  status = lz4_parallel_alloc_job(ctx, &jobs[0]);
  ASSERT_EQ(status, GCOMP_OK);
  const char * good_data = "This is compressible data that will succeed";
  memcpy(
      const_cast<uint8_t *>(jobs[0]->base.input), good_data, strlen(good_data));
  jobs[0]->base.input_size = strlen(good_data);
  status = lz4_parallel_submit(ctx, jobs[0]);
  ASSERT_EQ(status, GCOMP_OK);

  // Job 1: will fail (sabotaged output capacity)
  status = lz4_parallel_alloc_job(ctx, &jobs[1]);
  ASSERT_EQ(status, GCOMP_OK);
  for (size_t i = 0; i < 1000; i++) {
    const_cast<uint8_t *>(jobs[1]->base.input)[i] = (uint8_t)(i * 17 + 31);
  }
  jobs[1]->base.input_size = 1000;
  jobs[1]->base.output_capacity = 100; // Sabotage
  status = lz4_parallel_submit(ctx, jobs[1]);
  ASSERT_EQ(status, GCOMP_OK);

  // Job 2: will succeed
  status = lz4_parallel_alloc_job(ctx, &jobs[2]);
  ASSERT_EQ(status, GCOMP_OK);
  memcpy(
      const_cast<uint8_t *>(jobs[2]->base.input), good_data, strlen(good_data));
  jobs[2]->base.input_size = strlen(good_data);
  status = lz4_parallel_submit(ctx, jobs[2]);
  ASSERT_EQ(status, GCOMP_OK);

  // Retrieve in order - job 0 should succeed
  lz4_parallel_job_t * result = nullptr;
  status = lz4_parallel_get_result(ctx, &result);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(result, jobs[0]);
  EXPECT_EQ(result->base.result, GCOMP_OK);

  // Job 1 should fail
  status = lz4_parallel_get_result(ctx, &result);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
  EXPECT_EQ(result, jobs[1]);
  EXPECT_EQ(result->base.result, GCOMP_ERR_LIMIT);

  // Job 2 should succeed despite job 1 failing
  status = lz4_parallel_get_result(ctx, &result);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(result, jobs[2]);
  EXPECT_EQ(result->base.result, GCOMP_OK);

  // Cleanup
  for (int i = 0; i < 3; i++) {
    lz4_parallel_free_job(ctx, jobs[i]);
  }
  lz4_parallel_destroy(ctx);
}

//
// Integration Tests with Encoder
//

class Lz4ParallelEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data
  std::vector<uint8_t> compress(const void * data, size_t len,
      gcomp_options_t * opts, gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "lz4", opts, &encoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 100 + 256);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_encoder_destroy(encoder);
      return {};
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_encoder_destroy(encoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Decompress data
  std::vector<uint8_t> decompress(
      const void * data, size_t len, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len * 100 + 65536);

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
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_decoder_destroy(decoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(Lz4ParallelEncoderTest, SingleThreadedRoundtrip) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Use small block size to create multiple blocks
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
  ASSERT_EQ(status, GCOMP_OK);

  // Create data spanning multiple blocks
  std::vector<uint8_t> data(200000);
  test_helpers_generate_random(data.data(), data.size(), 12345);

  auto compressed = compress(data.data(), data.size(), opts);
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4ParallelEncoderTest, ParallelFriendlyIndependentBlocks) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Configure for parallel-friendly compression
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  // Create data spanning multiple blocks
  std::vector<uint8_t> data(500000);
  test_helpers_generate_random(data.data(), data.size(), 54321);

  auto compressed = compress(data.data(), data.size(), opts);
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4ParallelEncoderTest, ManyBlocksWithAllFeatures) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // All parallel-friendly options
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  // Create data spanning many blocks (1MB / 64KB = ~15 blocks)
  std::vector<uint8_t> data(1024 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 99999);

  auto compressed = compress(data.data(), data.size(), opts);
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);

  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
