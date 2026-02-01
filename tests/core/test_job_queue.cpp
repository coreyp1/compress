/**
 * @file test_job_queue.cpp
 *
 * Unit tests for the job queue API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/job_queue.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

//
// Test Fixtures
//

class JobQueueTest : public ::testing::Test {
protected:
  void SetUp() override {
    // No setup needed
  }

  void TearDown() override {
    // No cleanup needed
  }
};

//
// Creation/Destruction Tests
//

TEST_F(JobQueueTest, CreateWithNullConfig) {
  gcomp_job_queue_t * queue = nullptr;
  gcomp_status_t status = gcomp_job_queue_create(nullptr, &queue);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_NE(queue, nullptr);
  EXPECT_EQ(gcomp_job_queue_capacity(queue), 0U); // Unlimited
  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, CreateWithNullQueueOut) {
  gcomp_status_t status = gcomp_job_queue_create(nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(JobQueueTest, CreateWithCapacity) {
  gcomp_job_queue_config_t config = {.capacity = 4, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_NE(queue, nullptr);
  EXPECT_EQ(gcomp_job_queue_capacity(queue), 4U);

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, DestroyNull) {
  // Should not crash
  gcomp_job_queue_destroy(nullptr);
}

//
// Submit/Complete/Get Tests
//

TEST_F(JobQueueTest, SubmitSingleJob) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  // Create a job
  gcomp_block_job_t job = {};
  uint8_t input_data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
  job.input = input_data;
  job.input_size = sizeof(input_data);

  // Submit
  status = gcomp_job_queue_submit(queue, &job);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(job.sequence_num, 0U);
  EXPECT_EQ(job.status, GCOMP_JOB_PENDING);
  EXPECT_EQ(gcomp_job_queue_pending_count(queue), 1U);

  // Mark complete
  gcomp_job_queue_complete(queue, &job, GCOMP_OK);
  EXPECT_EQ(job.status, GCOMP_JOB_COMPLETE);
  EXPECT_EQ(job.result, GCOMP_OK);

  // Get result
  gcomp_block_job_t * result = nullptr;
  status = gcomp_job_queue_get_next_result(queue, &result);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(result, &job);
  EXPECT_EQ(gcomp_job_queue_pending_count(queue), 0U);

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, SubmitMultipleJobs_InOrderCompletion) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  // Create and submit multiple jobs
  gcomp_block_job_t jobs[5] = {};
  for (int i = 0; i < 5; i++) {
    status = gcomp_job_queue_submit(queue, &jobs[i]);
    EXPECT_EQ(status, GCOMP_OK);
    EXPECT_EQ(jobs[i].sequence_num, (uint64_t)i);
  }
  EXPECT_EQ(gcomp_job_queue_pending_count(queue), 5U);

  // Complete in order
  for (int i = 0; i < 5; i++) {
    gcomp_job_queue_complete(queue, &jobs[i], GCOMP_OK);
  }

  // Get results - should be in order
  for (int i = 0; i < 5; i++) {
    gcomp_block_job_t * result = nullptr;
    status = gcomp_job_queue_get_next_result(queue, &result);
    EXPECT_EQ(status, GCOMP_OK);
    EXPECT_EQ(result, &jobs[i]);
  }
  EXPECT_EQ(gcomp_job_queue_pending_count(queue), 0U);

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, SubmitMultipleJobs_OutOfOrderCompletion) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  // Create and submit multiple jobs
  gcomp_block_job_t jobs[5] = {};
  for (int i = 0; i < 5; i++) {
    status = gcomp_job_queue_submit(queue, &jobs[i]);
    EXPECT_EQ(status, GCOMP_OK);
  }

  // Complete out of order: 2, 4, 0, 3, 1
  int completion_order[] = {2, 4, 0, 3, 1};
  for (int i : completion_order) {
    gcomp_job_queue_complete(queue, &jobs[i], GCOMP_OK);
  }

  // Get results - should still be in submission order (0, 1, 2, 3, 4)
  for (int i = 0; i < 5; i++) {
    gcomp_block_job_t * result = nullptr;
    status = gcomp_job_queue_get_next_result(queue, &result);
    EXPECT_EQ(status, GCOMP_OK);
    EXPECT_EQ(result, &jobs[i])
        << "Expected job " << i << " but got job " << result->sequence_num;
  }

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, ResultReady) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_block_job_t jobs[3] = {};
  for (int i = 0; i < 3; i++) {
    status = gcomp_job_queue_submit(queue, &jobs[i]);
    EXPECT_EQ(status, GCOMP_OK);
  }

  // No results ready yet
  EXPECT_FALSE(gcomp_job_queue_result_ready(queue));

  // Complete job 2 first (out of order)
  gcomp_job_queue_complete(queue, &jobs[2], GCOMP_OK);
  // Still no result ready - job 0 is next expected
  EXPECT_FALSE(gcomp_job_queue_result_ready(queue));

  // Complete job 0
  gcomp_job_queue_complete(queue, &jobs[0], GCOMP_OK);
  // Now job 0 is ready
  EXPECT_TRUE(gcomp_job_queue_result_ready(queue));

  // Get job 0
  gcomp_block_job_t * result = nullptr;
  status = gcomp_job_queue_get_next_result(queue, &result);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(result, &jobs[0]);

  // Job 1 not complete yet
  EXPECT_FALSE(gcomp_job_queue_result_ready(queue));

  // Complete job 1
  gcomp_job_queue_complete(queue, &jobs[1], GCOMP_OK);
  EXPECT_TRUE(gcomp_job_queue_result_ready(queue));

  // Get remaining jobs
  status = gcomp_job_queue_get_next_result(queue, &result);
  EXPECT_EQ(result, &jobs[1]);

  // Job 2 was already completed, should be ready
  EXPECT_TRUE(gcomp_job_queue_result_ready(queue));
  status = gcomp_job_queue_get_next_result(queue, &result);
  EXPECT_EQ(result, &jobs[2]);

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, JobWithError) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_block_job_t job = {};
  status = gcomp_job_queue_submit(queue, &job);
  EXPECT_EQ(status, GCOMP_OK);

  // Complete with error
  gcomp_job_queue_complete(queue, &job, GCOMP_ERR_CORRUPT);
  EXPECT_EQ(job.status, GCOMP_JOB_ERROR);
  EXPECT_EQ(job.result, GCOMP_ERR_CORRUPT);

  // Get result
  gcomp_block_job_t * result = nullptr;
  status = gcomp_job_queue_get_next_result(queue, &result);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(result->result, GCOMP_ERR_CORRUPT);

  gcomp_job_queue_destroy(queue);
}

//
// Reset Tests
//

TEST_F(JobQueueTest, Reset) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  // Submit, complete, and get a job
  gcomp_block_job_t job = {};
  status = gcomp_job_queue_submit(queue, &job);
  EXPECT_EQ(job.sequence_num, 0U);
  gcomp_job_queue_complete(queue, &job, GCOMP_OK);
  gcomp_block_job_t * result = nullptr;
  status = gcomp_job_queue_get_next_result(queue, &result);
  EXPECT_EQ(status, GCOMP_OK);

  // Reset
  status = gcomp_job_queue_reset(queue);
  EXPECT_EQ(status, GCOMP_OK);

  // Submit another job - sequence should reset
  gcomp_block_job_t job2 = {};
  status = gcomp_job_queue_submit(queue, &job2);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(job2.sequence_num, 0U); // Reset to 0

  // Cleanup
  gcomp_job_queue_complete(queue, &job2, GCOMP_OK);
  status = gcomp_job_queue_get_next_result(queue, &result);

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, ResetWithPendingJobs) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  // Submit a job but don't complete/get it
  gcomp_block_job_t job = {};
  status = gcomp_job_queue_submit(queue, &job);
  EXPECT_EQ(status, GCOMP_OK);

  // Reset should fail with pending jobs
  status = gcomp_job_queue_reset(queue);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // Complete and get the job
  gcomp_job_queue_complete(queue, &job, GCOMP_OK);
  gcomp_block_job_t * result = nullptr;
  status = gcomp_job_queue_get_next_result(queue, &result);

  // Now reset should succeed
  status = gcomp_job_queue_reset(queue);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_job_queue_destroy(queue);
}

//
// Error Handling Tests
//

TEST_F(JobQueueTest, SubmitNull) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_job_queue_submit(queue, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  status = gcomp_job_queue_submit(nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, GetNextResultNull) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_job_queue_get_next_result(queue, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  status = gcomp_job_queue_get_next_result(nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_job_queue_destroy(queue);
}

TEST_F(JobQueueTest, PendingCountNull) {
  EXPECT_EQ(gcomp_job_queue_pending_count(nullptr), 0U);
}

TEST_F(JobQueueTest, CapacityNull) {
  EXPECT_EQ(gcomp_job_queue_capacity(nullptr), 0U);
}

TEST_F(JobQueueTest, ResultReadyNull) {
  EXPECT_FALSE(gcomp_job_queue_result_ready(nullptr));
}

TEST_F(JobQueueTest, ResetNull) {
  gcomp_status_t status = gcomp_job_queue_reset(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

//
// Multi-threaded Tests
//

TEST_F(JobQueueTest, ConcurrentSubmitAndComplete) {
  gcomp_job_queue_config_t config = {.capacity = 0, .allocator = nullptr};
  gcomp_job_queue_t * queue = nullptr;

  gcomp_status_t status = gcomp_job_queue_create(&config, &queue);
  ASSERT_EQ(status, GCOMP_OK);

  const int num_jobs = 100;
  std::vector<gcomp_block_job_t> jobs(num_jobs);

  // Submit all jobs from main thread
  for (int i = 0; i < num_jobs; i++) {
    status = gcomp_job_queue_submit(queue, &jobs[i]);
    EXPECT_EQ(status, GCOMP_OK);
  }

  // Complete from multiple threads
  std::atomic<int> completed{0};
  auto completer = [&](int start, int step) {
    for (int i = start; i < num_jobs; i += step) {
      gcomp_job_queue_complete(queue, &jobs[i], GCOMP_OK);
      completed.fetch_add(1);
    }
  };

  std::thread t1(completer, 0, 3);
  std::thread t2(completer, 1, 3);
  std::thread t3(completer, 2, 3);

  t1.join();
  t2.join();
  t3.join();

  EXPECT_EQ(completed.load(), num_jobs);

  // Get all results in order
  for (int i = 0; i < num_jobs; i++) {
    gcomp_block_job_t * result = nullptr;
    status = gcomp_job_queue_get_next_result(queue, &result);
    EXPECT_EQ(status, GCOMP_OK);
    EXPECT_EQ(result, &jobs[i]);
  }

  gcomp_job_queue_destroy(queue);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
