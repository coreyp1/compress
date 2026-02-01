/**
 * @file test_thread_pool.cpp
 *
 * Unit tests for the thread pool API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/thread_pool.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

//
// Test Fixtures
//

class ThreadPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    // No setup needed
  }

  void TearDown() override {
    // No cleanup needed
  }
};

//
// Simple Job Helpers
//

// Counter for tracking job executions
static std::atomic<int> g_job_counter{0};
static std::atomic<int> g_complete_counter{0};

// Simple job that increments counter
static gcomp_status_t simple_job(GCOMP_MAYBE_UNUSED(void * ctx)) {
  g_job_counter.fetch_add(1);
  return GCOMP_OK;
}

// Job that stores thread ID to verify parallel execution
struct ThreadIdJob {
  std::atomic<bool> executed{false};
  std::thread::id thread_id{};
};

static gcomp_status_t thread_id_job(void * ctx) {
  ThreadIdJob * job = static_cast<ThreadIdJob *>(ctx);
  job->thread_id = std::this_thread::get_id();
  job->executed.store(true);
  return GCOMP_OK;
}

// Job that takes some time
static gcomp_status_t slow_job(void * ctx) {
  int * delay_ms = static_cast<int *>(ctx);
  std::this_thread::sleep_for(std::chrono::milliseconds(*delay_ms));
  g_job_counter.fetch_add(1);
  return GCOMP_OK;
}

// Job that returns an error
static gcomp_status_t failing_job(GCOMP_MAYBE_UNUSED(void * ctx)) {
  return GCOMP_ERR_INTERNAL;
}

// Completion callback
static void job_complete_callback(GCOMP_MAYBE_UNUSED(void * ctx),
    GCOMP_MAYBE_UNUSED(gcomp_status_t status),
    GCOMP_MAYBE_UNUSED(void * user_data)) {
  g_complete_counter.fetch_add(1);
}

//
// Creation/Destruction Tests
//

TEST_F(ThreadPoolTest, CreateWithNullConfig) {
  gcomp_thread_pool_t * pool = nullptr;
  gcomp_status_t status = gcomp_thread_pool_create(nullptr, &pool);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_NE(pool, nullptr);
  EXPECT_TRUE(gcomp_thread_pool_is_inline(pool));
  EXPECT_EQ(gcomp_thread_pool_get_num_threads(pool), 0U);
  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, CreateWithNullPoolOut) {
  gcomp_status_t status = gcomp_thread_pool_create(nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(ThreadPoolTest, CreateInlineMode_ZeroThreads) {
  gcomp_thread_pool_config_t config = {.num_threads = 0, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_NE(pool, nullptr);
  EXPECT_TRUE(gcomp_thread_pool_is_inline(pool));
  EXPECT_EQ(gcomp_thread_pool_get_num_threads(pool), 0U);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, CreateInlineMode_OneThread) {
  gcomp_thread_pool_config_t config = {.num_threads = 1, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_NE(pool, nullptr);
  EXPECT_TRUE(gcomp_thread_pool_is_inline(pool));
  EXPECT_EQ(gcomp_thread_pool_get_num_threads(pool), 0U);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, CreateMultiThreaded_TwoThreads) {
  gcomp_thread_pool_config_t config = {.num_threads = 2, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_NE(pool, nullptr);
  EXPECT_FALSE(gcomp_thread_pool_is_inline(pool));
  EXPECT_EQ(gcomp_thread_pool_get_num_threads(pool), 2U);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, CreateMultiThreaded_FourThreads) {
  gcomp_thread_pool_config_t config = {.num_threads = 4, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_NE(pool, nullptr);
  EXPECT_FALSE(gcomp_thread_pool_is_inline(pool));
  EXPECT_EQ(gcomp_thread_pool_get_num_threads(pool), 4U);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, DestroyNull) {
  // Should not crash
  gcomp_thread_pool_destroy(nullptr);
}

//
// Inline Mode Tests
//

TEST_F(ThreadPoolTest, InlineModeJobExecution) {
  gcomp_thread_pool_config_t config = {.num_threads = 0, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset counter
  g_job_counter.store(0);

  // Submit job - should execute immediately in inline mode
  status =
      gcomp_thread_pool_submit(pool, simple_job, nullptr, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(g_job_counter.load(), 1);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, InlineModeJobExecutesInCallerThread) {
  gcomp_thread_pool_config_t config = {.num_threads = 1, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  ThreadIdJob job;
  std::thread::id caller_id = std::this_thread::get_id();

  status =
      gcomp_thread_pool_submit(pool, thread_id_job, &job, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_TRUE(job.executed.load());
  EXPECT_EQ(job.thread_id, caller_id);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, InlineModeWithCallback) {
  gcomp_thread_pool_config_t config = {.num_threads = 0, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  g_job_counter.store(0);
  g_complete_counter.store(0);

  status = gcomp_thread_pool_submit(
      pool, simple_job, nullptr, job_complete_callback, nullptr);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(g_job_counter.load(), 1);
  EXPECT_EQ(g_complete_counter.load(), 1);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, InlineModeFailingJob) {
  gcomp_thread_pool_config_t config = {.num_threads = 0, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  status =
      gcomp_thread_pool_submit(pool, failing_job, nullptr, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_OK); // Submit succeeds even if job fails

  // Wait should return the error
  status = gcomp_thread_pool_wait(pool);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_thread_pool_destroy(pool);
}

//
// Multi-threaded Mode Tests
//

TEST_F(ThreadPoolTest, MultiThreadedJobExecution) {
  gcomp_thread_pool_config_t config = {.num_threads = 2, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  g_job_counter.store(0);

  // Submit several jobs
  for (int i = 0; i < 10; i++) {
    status =
        gcomp_thread_pool_submit(pool, simple_job, nullptr, nullptr, nullptr);
    EXPECT_EQ(status, GCOMP_OK);
  }

  // Wait for all jobs to complete
  status = gcomp_thread_pool_wait(pool);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(g_job_counter.load(), 10);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, MultiThreadedJobsExecuteInWorkerThreads) {
  gcomp_thread_pool_config_t config = {.num_threads = 2, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  std::thread::id caller_id = std::this_thread::get_id();
  ThreadIdJob job;

  status =
      gcomp_thread_pool_submit(pool, thread_id_job, &job, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_OK);

  // Wait for completion
  status = gcomp_thread_pool_wait(pool);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_TRUE(job.executed.load());
  // In multi-threaded mode, job should execute in a different thread
  EXPECT_NE(job.thread_id, caller_id);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, MultiThreadedWithCallback) {
  gcomp_thread_pool_config_t config = {.num_threads = 2, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  g_job_counter.store(0);
  g_complete_counter.store(0);

  for (int i = 0; i < 5; i++) {
    status = gcomp_thread_pool_submit(
        pool, simple_job, nullptr, job_complete_callback, nullptr);
    EXPECT_EQ(status, GCOMP_OK);
  }

  status = gcomp_thread_pool_wait(pool);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(g_job_counter.load(), 5);
  EXPECT_EQ(g_complete_counter.load(), 5);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, MultiThreadedFailingJob) {
  gcomp_thread_pool_config_t config = {.num_threads = 2, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  g_job_counter.store(0);

  // Submit some good jobs and one failing job
  status =
      gcomp_thread_pool_submit(pool, simple_job, nullptr, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_OK);

  status =
      gcomp_thread_pool_submit(pool, failing_job, nullptr, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_OK);

  status =
      gcomp_thread_pool_submit(pool, simple_job, nullptr, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_OK);

  // Wait should return the first error
  status = gcomp_thread_pool_wait(pool);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  // Good jobs should still have executed
  EXPECT_EQ(g_job_counter.load(), 2);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, ParallelExecution) {
  gcomp_thread_pool_config_t config = {.num_threads = 4, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  g_job_counter.store(0);

  // Submit slow jobs - with parallel execution, this should be much faster
  // than sequential execution
  int delay = 50; // 50ms per job
  const int num_jobs = 8;

  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < num_jobs; i++) {
    status = gcomp_thread_pool_submit(pool, slow_job, &delay, nullptr, nullptr);
    EXPECT_EQ(status, GCOMP_OK);
  }

  status = gcomp_thread_pool_wait(pool);
  EXPECT_EQ(status, GCOMP_OK);

  auto end = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  EXPECT_EQ(g_job_counter.load(), num_jobs);

  // With 4 threads and 8 jobs of 50ms each, should take ~100ms (2 batches)
  // Sequential would be 400ms. Allow some margin for scheduling overhead.
  // If it takes less than 300ms, we're definitely running in parallel.
  EXPECT_LT(duration.count(), 300);

  gcomp_thread_pool_destroy(pool);
}

//
// Error Handling Tests
//

TEST_F(ThreadPoolTest, SubmitNullPool) {
  gcomp_status_t status =
      gcomp_thread_pool_submit(nullptr, simple_job, nullptr, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(ThreadPoolTest, SubmitNullFunc) {
  gcomp_thread_pool_config_t config = {.num_threads = 0, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_thread_pool_submit(pool, nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_thread_pool_destroy(pool);
}

TEST_F(ThreadPoolTest, WaitNullPool) {
  gcomp_status_t status = gcomp_thread_pool_wait(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(ThreadPoolTest, WaitNoJobs) {
  gcomp_thread_pool_config_t config = {.num_threads = 2, .allocator = nullptr};
  gcomp_thread_pool_t * pool = nullptr;

  gcomp_status_t status = gcomp_thread_pool_create(&config, &pool);
  ASSERT_EQ(status, GCOMP_OK);

  // Wait with no jobs should succeed immediately
  status = gcomp_thread_pool_wait(pool);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_thread_pool_destroy(pool);
}

//
// Query Function Tests
//

TEST_F(ThreadPoolTest, GetNumThreadsNull) {
  EXPECT_EQ(gcomp_thread_pool_get_num_threads(nullptr), 0U);
}

TEST_F(ThreadPoolTest, IsInlineNull) {
  EXPECT_TRUE(gcomp_thread_pool_is_inline(nullptr));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
