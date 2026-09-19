/**
 * @file test_lz4_parallel.cpp
 *
 * Unit tests for LZ4 parallel block compression in the Ghoti.io Compress
 * library.
 *
 * These tests verify:
 * - Parallel context creation and the direct submit/get_result API
 * - That `threads.count` reaches the encoder at all, which for a long time it
 *   did not: lz4_parallel.c was written, tested and documented, but no option
 *   named it and LZ4's schema rejects unknown keys, so every configuration
 *   that would have used it was refused with GCOMP_ERR_INVALID_ARG.  See
 *   ThreadsCountIsAnOptionLz4Accepts.
 * - That parallel and single-threaded encoding produce **the same bytes**,
 *   which is the property the whole design rests on -- see
 *   TheSameBytesWhicheverThreadCountProducedThem and lz4_parallel.h.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
// Include internal header for direct testing (relative to tests/methods/lz4/)
#include "../../../src/methods/lz4/lz4_parallel.h"
#include "../../../src/methods/lz4/lz4_internal.h"
#include <algorithm>
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


//
// Parallel encoding through the public API
//
// The point of these is what the direct-API tests above cannot see: whether
// an ordinary caller setting threads.count gets parallel encoding, and
// whether what comes out is the same frame either way.
//

class Lz4ThreadedEncodingTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  /// Options every test here starts from; `threads` of 0 leaves it unset.
  gcomp_options_t * make_options(uint64_t threads, uint64_t block_size,
      bool block_checksum, bool content_checksum, bool independent = true) {
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", block_size),
        GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum",
                  block_checksum ? 1 : 0),
        GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum",
                  content_checksum ? 1 : 0),
        GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(
                  opts, "lz4.independent_blocks", independent ? 1 : 0),
        GCOMP_OK);
    if (threads > 0) {
      EXPECT_EQ(gcomp_options_set_uint64(opts, "threads.count", threads),
          GCOMP_OK);
    }
    return opts;
  }

  /// Encode in one pass with room to spare.
  std::vector<uint8_t> encode(const std::vector<uint8_t> & data,
      gcomp_options_t * opts, uint32_t * workers_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
    if (!encoder) {
      return {};
    }
    if (workers_out) {
      *workers_out = gcomp_lz4_encoder_worker_count(encoder);
    }
    std::vector<uint8_t> out(data.size() + data.size() / 2 + 65536);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(data.data()), data.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
    EXPECT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);
    out.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    return out;
  }

  /**
   * Encode with the input and output buffers deliberately starved, so every
   * resumption path is exercised: a job filled across many update() calls, a
   * block handed out a few bytes at a time, finish() called repeatedly.
   */
  std::vector<uint8_t> encode_drip(const std::vector<uint8_t> & data,
      gcomp_options_t * opts, size_t in_chunk, size_t out_chunk) {
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
    if (!encoder) {
      return {};
    }
    std::vector<uint8_t> out;
    std::vector<uint8_t> chunk(out_chunk);

    size_t consumed = 0;
    while (consumed < data.size()) {
      size_t take = std::min(in_chunk, data.size() - consumed);
      gcomp_buffer_t in_buf = {
          const_cast<uint8_t *>(data.data() + consumed), take, 0};
      // Keep offering the same input until it is taken: a full output buffer
      // makes update() return with input left over, which is not an error.
      while (in_buf.used < in_buf.size) {
        gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
        size_t before = in_buf.used;
        EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
        out.insert(out.end(), chunk.begin(), chunk.begin() + out_buf.used);
        if (in_buf.used == before && out_buf.used == 0) {
          ADD_FAILURE() << "update() made no progress";
          break;
        }
      }
      consumed += take;
    }

    for (;;) {
      gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
      gcomp_status_t status = gcomp_encoder_finish(encoder, &out_buf);
      out.insert(out.end(), chunk.begin(), chunk.begin() + out_buf.used);
      if (status == GCOMP_OK) {
        break;
      }
      if (status != GCOMP_ERR_LIMIT) {
        ADD_FAILURE() << "finish() returned " << status;
        break;
      }
      if (out_buf.used == 0) {
        ADD_FAILURE() << "finish() made no progress";
        break;
      }
    }
    gcomp_encoder_destroy(encoder);
    return out;
  }

  std::vector<uint8_t> decode(const std::vector<uint8_t> & frame) {
    gcomp_decoder_t * decoder = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);
    if (!decoder) {
      return {};
    }
    std::vector<uint8_t> out(frame.size() * 32 + 65536);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(frame.data()), frame.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
    EXPECT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK);
    out.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return out;
  }

  /// Compressible enough that blocks are really compressed, not stored.
  static std::vector<uint8_t> make_data(size_t len, unsigned seed) {
    std::vector<uint8_t> data(len);
    unsigned state = seed;
    static const char * words[] = {"the ", "quick ", "brown ", "fox ",
        "jumps ", "over ", "lazy ", "dog ", "and then ", "again "};
    size_t pos = 0;
    while (pos < len) {
      state = state * 1103515245u + 12345u;
      const char * w = words[(state >> 16) % 10];
      size_t n = strlen(w);
      if (pos + n > len) {
        n = len - pos;
      }
      memcpy(data.data() + pos, w, n);
      pos += n;
    }
    return data;
  }

  gcomp_registry_t * registry_ = nullptr;
};

/**
 * The regression test for the defect this whole path had: `threads.count` was
 * not in LZ4's option schema, and LZ4's schema is GCOMP_UNKNOWN_KEY_ERROR, so
 * asking for threads did not select a slower encoder -- it failed the call.
 */
TEST_F(Lz4ThreadedEncodingTest, ThreadsCountIsAnOptionLz4Accepts) {
  gcomp_options_t * opts = make_options(4, 65536, false, false);
  gcomp_encoder_t * encoder = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  std::vector<uint8_t> data = make_data(300000, 7);
  std::vector<uint8_t> out(data.size() + 65536);
  gcomp_buffer_t in_buf = {data.data(), data.size(), 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);
  EXPECT_GT(out_buf.used, 0u);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

/**
 * The invariant the design rests on, stated as a test: what the workers
 * produce is what one thread would have produced, byte for byte, for every
 * combination of block size and checksum.  Both lz4_parallel.h and
 * lz4_parallel.c name this test; if it is renamed, rename it there too.
 *
 * It is worth being clear about what would make this test worthless: if the
 * encoder quietly fell back to inline compression, every comparison here
 * would pass while proving nothing.  That is why the worker count is checked
 * as well -- the bytes must match *and* the threads must be real.
 */
TEST_F(Lz4ThreadedEncodingTest, TheSameBytesWhicheverThreadCountProducedThem) {
  const std::vector<uint8_t> data = make_data(700000, 11);
  const uint64_t block_sizes[] = {65536, 262144, 1048576, 4194304};

  for (uint64_t block : block_sizes) {
    for (int bcsum = 0; bcsum < 2; bcsum++) {
      for (int ccsum = 0; ccsum < 2; ccsum++) {
        gcomp_options_t * one =
            make_options(1, block, bcsum != 0, ccsum != 0);
        uint32_t serial_workers = 0;
        std::vector<uint8_t> reference = encode(data, one, &serial_workers);
        gcomp_options_destroy(one);
        ASSERT_FALSE(reference.empty());
        EXPECT_EQ(serial_workers, 1u);

        for (uint64_t threads : {2u, 4u, 8u}) {
          gcomp_options_t * many =
              make_options(threads, block, bcsum != 0, ccsum != 0);
          uint32_t workers = 0;
          std::vector<uint8_t> got = encode(data, many, &workers);
          gcomp_options_destroy(many);

          EXPECT_EQ(workers, threads)
              << "fell back to inline, so the comparison below proves nothing";
          EXPECT_EQ(got, reference)
              << "block=" << block << " bcsum=" << bcsum
              << " ccsum=" << ccsum << " threads=" << threads;
        }
      }
    }
  }
}

/**
 * The same, with a dictionary.  This is the part most likely to drift: a
 * worker has to start from the dictionary bytes *and* from the hash table
 * indexing them leaves behind, and getting either wrong still produces a
 * valid frame -- just a different one.
 */
TEST_F(Lz4ThreadedEncodingTest, TheSameBytesWithADictionaryToo) {
  const std::vector<uint8_t> data = make_data(400000, 23);
  const std::vector<uint8_t> dict = make_data(40000, 23);

  auto with_dict = [&](uint64_t threads) {
    gcomp_options_t * opts = make_options(threads, 65536, true, true);
    EXPECT_EQ(gcomp_options_set_bytes(
                  opts, "lz4.dictionary", dict.data(), dict.size()),
        GCOMP_OK);
    uint32_t workers = 0;
    std::vector<uint8_t> out = encode(data, opts, &workers);
    EXPECT_EQ(workers, threads > 1 ? threads : 1u);
    gcomp_options_destroy(opts);
    return out;
  };

  std::vector<uint8_t> reference = with_dict(1);
  ASSERT_FALSE(reference.empty());
  EXPECT_EQ(with_dict(2), reference);
  EXPECT_EQ(with_dict(4), reference);

  // And the dictionary is doing something, or the comparison above would hold
  // for a parallel path that ignored it entirely.
  gcomp_options_t * plain = make_options(1, 65536, true, true);
  std::vector<uint8_t> without = encode(data, plain, nullptr);
  gcomp_options_destroy(plain);
  EXPECT_LT(reference.size(), without.size());
}

/// Whatever the thread count, a decoder gets the original bytes back.
TEST_F(Lz4ThreadedEncodingTest, EveryThreadCountRoundTrips) {
  const std::vector<uint8_t> data = make_data(900000, 31);
  for (uint64_t threads : {1u, 2u, 4u, 8u}) {
    gcomp_options_t * opts = make_options(threads, 65536, true, true);
    std::vector<uint8_t> frame = encode(data, opts, nullptr);
    gcomp_options_destroy(opts);
    ASSERT_FALSE(frame.empty()) << "threads=" << threads;
    EXPECT_EQ(decode(frame), data) << "threads=" << threads;
  }
}

/**
 * Linked blocks are the format saying block N may reference block N-1, which
 * is exactly the dependency that rules out compressing them at the same time.
 * Asking for threads anyway is not an error and does not change the output;
 * it simply leaves nothing to parallelise, and the encoder says so.
 */
TEST_F(Lz4ThreadedEncodingTest, LinkedBlocksLeaveNothingToParallelise) {
  const std::vector<uint8_t> data = make_data(300000, 41);

  gcomp_options_t * linked =
      make_options(4, 65536, false, true, /*independent=*/false);
  uint32_t workers = 0;
  std::vector<uint8_t> threaded = encode(data, linked, &workers);
  gcomp_options_destroy(linked);
  EXPECT_EQ(workers, 1u) << "linked blocks cannot be compressed in parallel";

  gcomp_options_t * serial =
      make_options(1, 65536, false, true, /*independent=*/false);
  std::vector<uint8_t> reference = encode(data, serial, nullptr);
  gcomp_options_destroy(serial);

  EXPECT_EQ(threaded, reference);
  EXPECT_EQ(decode(threaded), data);
}

/// threads.count of 1 is the default and must not start a pool.
TEST_F(Lz4ThreadedEncodingTest, OneThreadIsNotThreaded) {
  gcomp_options_t * opts = make_options(1, 65536, false, false);
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
  EXPECT_EQ(gcomp_lz4_encoder_worker_count(encoder), 1u);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  gcomp_encoder_t * plain = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "lz4", nullptr, &plain), GCOMP_OK);
  EXPECT_EQ(gcomp_lz4_encoder_worker_count(plain), 1u);
  gcomp_encoder_destroy(plain);
}

/**
 * A caller who hands over one byte at a time and takes seven back is doing
 * nothing wrong, and must get the same frame as one who passes the lot in a
 * single call.  In parallel mode this is where a block gets submitted while
 * an earlier one is still half-delivered, which is the case the encoder has
 * to hold a result back for.
 */
TEST_F(Lz4ThreadedEncodingTest, StarvedBuffersProduceTheSameFrame) {
  const std::vector<uint8_t> data = make_data(250000, 53);

  gcomp_options_t * one = make_options(1, 65536, true, true);
  std::vector<uint8_t> reference = encode(data, one, nullptr);
  gcomp_options_destroy(one);
  ASSERT_FALSE(reference.empty());

  for (uint64_t threads : {1u, 4u}) {
    gcomp_options_t * opts = make_options(threads, 65536, true, true);
    EXPECT_EQ(encode_drip(data, opts, 1, 7), reference)
        << "threads=" << threads << " one byte in, seven out";
    gcomp_options_destroy(opts);

    gcomp_options_t * opts2 = make_options(threads, 65536, true, true);
    EXPECT_EQ(encode_drip(data, opts2, 40000, 100), reference)
        << "threads=" << threads << " large input, tiny output";
    gcomp_options_destroy(opts2);
  }
}

/**
 * An output buffer smaller than one block forces every result to be held back
 * and handed out in pieces, which is the path that deadlocked the zstd
 * encoder before it collected instead of waiting.  With more blocks than
 * in-flight slots, an encoder that waits for room only it can make stops
 * here; the test would hang rather than fail, which is its own kind of
 * failure report.
 */
TEST_F(Lz4ThreadedEncodingTest, MoreBlocksThanSlotsDoesNotStall) {
  const std::vector<uint8_t> data = make_data(2 * 1024 * 1024, 67);

  gcomp_options_t * one = make_options(1, 65536, false, true);
  std::vector<uint8_t> reference = encode(data, one, nullptr);
  gcomp_options_destroy(one);

  // 32 blocks against 2 threads * 2 in flight.
  gcomp_options_t * opts = make_options(2, 65536, false, true);
  EXPECT_EQ(encode_drip(data, opts, 4096, 1024), reference);
  gcomp_options_destroy(opts);
}

/// A reset starts a new frame; the workers are still there for it.
TEST_F(Lz4ThreadedEncodingTest, ResetStartsANewFrameWithTheWorkersIntact) {
  const std::vector<uint8_t> data = make_data(300000, 71);

  gcomp_options_t * opts = make_options(4, 65536, true, true);
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
  EXPECT_EQ(gcomp_lz4_encoder_worker_count(encoder), 4u);

  std::vector<uint8_t> first(data.size() + 65536);
  gcomp_buffer_t in1 = {data.data(), data.size(), 0};
  gcomp_buffer_t out1 = {first.data(), first.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(encoder, &in1, &out1), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out1), GCOMP_OK);
  first.resize(out1.used);

  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  EXPECT_EQ(gcomp_lz4_encoder_worker_count(encoder), 4u);

  std::vector<uint8_t> second(data.size() + 65536);
  gcomp_buffer_t in2 = {data.data(), data.size(), 0};
  gcomp_buffer_t out2 = {second.data(), second.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(encoder, &in2, &out2), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out2), GCOMP_OK);
  second.resize(out2.used);

  EXPECT_EQ(second, first) << "a reset frame must be the first frame again";
  EXPECT_EQ(decode(second), data);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

/**
 * Abandoning a frame part-way through leaves blocks with the workers.  Those
 * jobs are the encoder's to free, and nothing else will: under ASan this test
 * is the one that notices if they are not.
 */
TEST_F(Lz4ThreadedEncodingTest, AbandoningAFrameFreesWhatTheWorkersHold) {
  const std::vector<uint8_t> data = make_data(2 * 1024 * 1024, 73);

  for (int trial = 0; trial < 4; trial++) {
    gcomp_options_t * opts = make_options(4, 65536, true, true);
    gcomp_encoder_t * encoder = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);

    // A small output buffer leaves results uncollected and jobs in flight.
    std::vector<uint8_t> out(4096);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(data.data()), data.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);

    // No finish: walk away mid-frame, the way a failing caller would.
    gcomp_encoder_destroy(encoder);
    gcomp_options_destroy(opts);
  }
}

/**
 * Blocks a worker had to store rather than compress are still step-downs, and
 * the encoder's tally has to hold them -- otherwise parallel mode would be a
 * blind spot in exactly the accounting src/core/stepdown.h exists to keep.
 * Random bytes do not compress, so every block is stored.
 */
TEST_F(Lz4ThreadedEncodingTest, AStoredBlockIsCountedWhoeverStoredIt) {
  std::vector<uint8_t> noise(400000);
  test_helpers_generate_random(noise.data(), noise.size(), 4242);

  auto tally_for = [&](uint64_t threads) {
    gcomp_options_t * opts = make_options(threads, 65536, false, false);
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
    std::vector<uint8_t> out(noise.size() + 65536);
    gcomp_buffer_t in_buf = {noise.data(), noise.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
    EXPECT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);
    const gcomp_stepdown_tally_t * tally =
        gcomp_lz4_encoder_stepdowns(encoder);
    gcomp_stepdown_tally_t copy = *tally;
    gcomp_encoder_destroy(encoder);
    gcomp_options_destroy(opts);
    return copy;
  };

  gcomp_stepdown_tally_t serial = tally_for(1);
  gcomp_stepdown_tally_t parallel = tally_for(4);

  EXPECT_GT(serial.counts[GCOMP_STEPDOWN_STORED_IS_SMALLER], 0u)
      << "random data should have produced stored blocks";
  EXPECT_EQ(parallel.counts[GCOMP_STEPDOWN_STORED_IS_SMALLER],
      serial.counts[GCOMP_STEPDOWN_STORED_IS_SMALLER])
      << "a worker's stored block must be counted like any other";
  EXPECT_EQ(gcomp_stepdown_forced_total(&parallel), 0u);
}

/**
 * The memory limit has to cover the workers' buffers, not just the encoder's.
 * A limit too small for even one in-flight block is refused rather than
 * quietly exceeded.
 */
TEST_F(Lz4ThreadedEncodingTest, TheMemoryLimitCoversTheWorkersToo) {
  gcomp_options_t * opts = make_options(4, 4194304, false, false);
  // Room for the encoder's own buffers but nowhere near four 4 MB blocks in
  // flight, each with a window, an output buffer and a 256 KB table.
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_memory_bytes",
                9u * 1024 * 1024),
      GCOMP_OK);
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_MEMORY);
  if (status == GCOMP_OK) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);

  // With a generous limit the same configuration is fine, so the refusal
  // above was about the limit and not about the configuration.
  gcomp_options_t * roomy = make_options(4, 4194304, false, false);
  ASSERT_EQ(gcomp_options_set_uint64(roomy, "limits.max_memory_bytes",
                512u * 1024 * 1024),
      GCOMP_OK);
  gcomp_encoder_t * ok_encoder = nullptr;
  EXPECT_EQ(
      gcomp_encoder_create(registry_, "lz4", roomy, &ok_encoder), GCOMP_OK);
  if (ok_encoder) {
    EXPECT_EQ(gcomp_lz4_encoder_worker_count(ok_encoder), 4u);
    gcomp_encoder_destroy(ok_encoder);
  }
  gcomp_options_destroy(roomy);
}

/// Input that stops exactly on a block boundary must not emit an empty block:
/// a zero block size is the frame's end mark.
TEST_F(Lz4ThreadedEncodingTest, AnExactMultipleOfTheBlockSizeEndsCleanly) {
  const std::vector<uint8_t> data = make_data(65536 * 6, 79);
  for (uint64_t threads : {1u, 4u}) {
    gcomp_options_t * opts = make_options(threads, 65536, true, true);
    std::vector<uint8_t> frame = encode(data, opts, nullptr);
    gcomp_options_destroy(opts);
    ASSERT_FALSE(frame.empty());
    EXPECT_EQ(decode(frame), data) << "threads=" << threads;
  }
}

/// An empty frame is still a frame, workers or not.
TEST_F(Lz4ThreadedEncodingTest, NoInputAtAllStillMakesAFrame) {
  const std::vector<uint8_t> empty;
  gcomp_options_t * one = make_options(1, 65536, true, true);
  std::vector<uint8_t> reference = encode(empty, one, nullptr);
  gcomp_options_destroy(one);

  gcomp_options_t * many = make_options(4, 65536, true, true);
  std::vector<uint8_t> threaded = encode(empty, many, nullptr);
  gcomp_options_destroy(many);

  EXPECT_EQ(threaded, reference);
  EXPECT_TRUE(decode(threaded).empty());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
