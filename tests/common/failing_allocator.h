/**
 * @file failing_allocator.h
 *
 * An allocator that fails on demand, for testing allocation-failure paths.
 *
 * ## Why this exists
 *
 * Every allocation in the library goes through the allocator it was handed
 * (CONVENTIONS.md section 5, "Allocation"), so a caller can watch every one
 * of them and make any chosen one fail.  That reaches code no ordinary test
 * can: the `GCOMP_ERR_MEMORY` return beside each allocation, and the growth
 * paths that only run when a buffer has to be enlarged.  A reallocation path
 * no test reaches is untested rather than working - a hash table that
 * corrupted itself when it grew once survived a fully green suite in this
 * library.
 *
 * ## The contract being tested
 *
 * When an allocation fails, an operation may do exactly one of two things:
 *
 * - finish anyway (::GCOMP_OK), having not needed the memory; or
 * - fail with ::GCOMP_ERR_MEMORY.
 *
 * Anything else is a defect.  ::GCOMP_ERR_INTERNAL for an allocation failure
 * is wrong by CONVENTIONS.md section 5, which assigns "the allocator returned
 * NULL" its own code.  ::GCOMP_OK with truncated output is worse: silent data
 * loss.  And whichever of the two happens, every block must be freed by the
 * time the encoder, decoder, registry or options object is destroyed.
 *
 * ## Use
 *
 * @code
 * FailingAllocator fa;
 * size_t n = 0;
 * // Count pass: nothing fails, so this is how many allocations a whole
 * // successful run makes.
 * run_the_operation(fa.allocator());
 * n = fa.calls();
 *
 * for (size_t k = 1; k <= n; k++) {
 *   fa.reset();
 *   fa.fail_at(k);
 *   gcomp_status_t s = run_the_operation(fa.allocator());
 *   ASSERT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_MEMORY);
 *   EXPECT_EQ(fa.live_blocks(), 0u);   // after destroying everything
 * }
 * @endcode
 *
 * ## Thread safety
 *
 * The counter and the live-block table are mutex-guarded, because the
 * parallel encoders and decoders allocate on worker threads.  The library
 * itself uses cutil's threading; this is test code, so it uses the C++
 * standard library's mutex rather than adding a cutil dependency to a header
 * that gtest includes.
 *
 * Note that with more than one thread running, *which* allocation is the
 * `k`th is not deterministic.  Failure injection against a threaded
 * configuration therefore tests that failing any one allocation is handled,
 * not that failing a specific one is - which is what is wanted here.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_COMPRESS_TESTS_FAILING_ALLOCATOR_H
#define GHOTI_IO_COMPRESS_TESTS_FAILING_ALLOCATOR_H

#include <ghoti.io/compress/allocator.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

/**
 * @brief An allocator that hands out memory until told to stop.
 *
 * Wraps the C library.  Counts every allocating call (malloc, calloc and
 * realloc; free is not counted) and can be told to fail one of them, or every
 * one from a chosen point on.  Tracks what is currently live so a test can
 * assert that a failed operation left nothing behind.
 */
class FailingAllocator {
public:
  FailingAllocator() {
    allocator_.ctx = this;
    allocator_.malloc_fn = &FailingAllocator::malloc_thunk;
    allocator_.calloc_fn = &FailingAllocator::calloc_thunk;
    allocator_.realloc_fn = &FailingAllocator::realloc_thunk;
    allocator_.free_fn = &FailingAllocator::free_thunk;
  }

  FailingAllocator(const FailingAllocator &) = delete;
  FailingAllocator & operator=(const FailingAllocator &) = delete;

  ~FailingAllocator() {
    // Anything still live is the test's leak to report, not ours to hide;
    // free it so the process exits clean under ASan either way.
    for (auto & entry : live_) {
      std::free(entry.first);
    }
  }

  /// The allocator to hand to the library.  Valid for this object's lifetime.
  gcomp_allocator_t * allocator() {
    return &allocator_;
  }

  /**
   * @brief Fail the @p n th allocating call, counting from 1.
   *
   * Zero means never fail, which is what a counting pass wants.
   */
  void fail_at(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_at_ = n;
  }

  /**
   * @brief Fail every allocating call from the @p n th on, counting from 1.
   *
   * Models sustained memory pressure, where a caller that retries or falls
   * back to a smaller allocation does not get to succeed on the second try.
   * Zero disables it.
   */
  void fail_from(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_from_ = n;
  }

  /// Allocating calls made so far, failed ones included.
  size_t calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }

  /// Allocations that were refused because a test asked for them to be.
  size_t injected_failures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return injected_;
  }

  /// Blocks allocated and not yet freed.
  size_t live_blocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_.size();
  }

  /// Bytes allocated and not yet freed.
  size_t live_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto & entry : live_) {
      total += entry.second;
    }
    return total;
  }

  /**
   * @brief Clear the counter and the injection settings.
   *
   * Does not free anything: live blocks are the previous run's business, and
   * a test that resets while blocks are live wants to know that.
   */
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    calls_ = 0;
    injected_ = 0;
    fail_at_ = 0;
    fail_from_ = 0;
  }

private:
  /**
   * @brief Decide whether this call fails, and count it.
   *
   * @return true when the caller should return NULL.
   */
  bool should_fail() {
    calls_++;
    if (fail_at_ != 0 && calls_ == fail_at_) {
      injected_++;
      return true;
    }
    if (fail_from_ != 0 && calls_ >= fail_from_) {
      injected_++;
      return true;
    }
    return false;
  }

  void * do_malloc(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (should_fail()) {
      return nullptr;
    }
    // A zero-size request must return a usable non-NULL pointer, so that NULL
    // always means failure (allocator.h).
    void * p = std::malloc(size ? size : 1);
    if (p) {
      live_[p] = size;
    }
    return p;
  }

  void * do_calloc(size_t nitems, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Overflow is an allocation failure, not a truncated block (allocator.h).
    // Checked before should_fail() so it is not counted as an injected one.
    if (nitems != 0 && size > (size_t)-1 / nitems) {
      calls_++;
      return nullptr;
    }
    if (should_fail()) {
      return nullptr;
    }
    size_t total = nitems * size;
    void * p = std::calloc(total ? total : 1, 1);
    if (p) {
      live_[p] = total;
    }
    return p;
  }

  void * do_realloc(void * ptr, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (should_fail()) {
      // A failed realloc leaves the original block valid and live.
      return nullptr;
    }
    void * p = std::realloc(ptr, size ? size : 1);
    if (p) {
      if (ptr && p != ptr) {
        live_.erase(ptr);
      }
      live_[p] = size;
    }
    return p;
  }

  void do_free(void * ptr) {
    if (!ptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    live_.erase(ptr);
    std::free(ptr);
  }

  static void * malloc_thunk(void * ctx, size_t size) {
    return static_cast<FailingAllocator *>(ctx)->do_malloc(size);
  }

  static void * calloc_thunk(void * ctx, size_t nitems, size_t size) {
    return static_cast<FailingAllocator *>(ctx)->do_calloc(nitems, size);
  }

  static void * realloc_thunk(void * ctx, void * ptr, size_t size) {
    return static_cast<FailingAllocator *>(ctx)->do_realloc(ptr, size);
  }

  static void free_thunk(void * ctx, void * ptr) {
    static_cast<FailingAllocator *>(ctx)->do_free(ptr);
  }

  gcomp_allocator_t allocator_{};
  mutable std::mutex mutex_;
  std::unordered_map<void *, size_t> live_;
  size_t calls_ = 0;
  size_t injected_ = 0;
  size_t fail_at_ = 0;
  size_t fail_from_ = 0;
};

#endif // GHOTI_IO_COMPRESS_TESTS_FAILING_ALLOCATOR_H
