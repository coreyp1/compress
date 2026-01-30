# Threading Support for Parallel Compression

This document describes how compression methods in the Ghoti.io Compress library can implement parallel compression using the threading primitives provided by the sibling `cutil` library.

## Overview

The compress library framework does **not** include its own threading infrastructure. Instead, methods that need parallel compression should use the cross-platform threading APIs provided by `cutil`:

- **`cutil/thread.h`** - Thread creation, joining, and utilities
- **`cutil/mutex.h`** - Mutual exclusion locks
- **`cutil/semaphore.h`** - Counting semaphores for coordination

This design keeps the compress library focused on compression algorithms while leveraging the battle-tested threading primitives in `cutil`.

## Threading Primitives from cutil

### Thread Management (`cutil/thread.h`)

```c
#include <cutil/thread.h>

// Thread handle type
GCU_Thread thread;

// Thread function signature
GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION my_worker(GCU_THREAD_FUNC_ARG_T arg) {
    // Worker code here
    return 0;
}

// Create and start a thread
int gcu_thread_create(GCU_Thread *thread, GCU_THREAD_FUNC func, void *arg);

// Wait for a thread to complete
int gcu_thread_join(GCU_Thread thread);

// Detach a thread (let it run independently)
int gcu_thread_detach(GCU_Thread thread);

// Get the number of logical processors (useful for default thread count)
unsigned int gcu_thread_get_num_processors();

// Utility functions
void gcu_thread_sleep(unsigned long milliseconds);
void gcu_thread_yield();
```

### Mutexes (`cutil/mutex.h`)

```c
#include <cutil/mutex.h>

GCU_MUTEX_T my_mutex;

// Create a mutex (returns 0 on success)
if (GCU_MUTEX_CREATE(my_mutex) != 0) {
    // Handle error
}

// Lock the mutex (blocking)
GCU_MUTEX_LOCK(my_mutex);

// Critical section...

// Unlock the mutex
GCU_MUTEX_UNLOCK(my_mutex);

// Try to lock without blocking (returns 0 if acquired)
if (GCU_MUTEX_TRYLOCK(my_mutex) == 0) {
    // Got the lock
}

// Destroy the mutex when done
GCU_MUTEX_DESTROY(my_mutex);
```

### Semaphores (`cutil/semaphore.h`)

```c
#include <cutil/semaphore.h>

GCU_Semaphore sem;

// Create a semaphore with initial value
int gcu_semaphore_create(GCU_Semaphore *semaphore, int value);

// Wait (decrement, blocking if zero)
int gcu_semaphore_wait(GCU_Semaphore *semaphore);

// Signal (increment)
int gcu_semaphore_signal(GCU_Semaphore *semaphore);

// Try to wait without blocking
int gcu_semaphore_trywait(GCU_Semaphore *semaphore);

// Wait with timeout (milliseconds)
int gcu_semaphore_timedwait(GCU_Semaphore *semaphore, int timeout);

// Get current value
int gcu_semaphore_getvalue(GCU_Semaphore *semaphore, int *value);

// Destroy the semaphore
int gcu_semaphore_destroy(GCU_Semaphore *semaphore);
```

## Option Convention: `threads.count`

Methods that support parallel compression should check for a `threads.count` option:

```c
int64_t thread_count = 1;  // Default: single-threaded
gcomp_options_get_int64(options, "threads.count", &thread_count);

if (thread_count < 1) {
    thread_count = 1;  // Minimum 1 thread
}
```

### Recommended Behavior

| `threads.count` | Behavior |
|-----------------|----------|
| 1 (default)     | Single-threaded operation (no threading overhead) |
| N > 1           | Use N worker threads for parallel compression |
| 0               | Auto-detect: use `gcu_thread_get_num_processors()` |

Methods should document whether they support the `threads.count` option in their schema.

## Implementation Pattern

Here's a typical pattern for methods implementing parallel compression:

```c
typedef struct {
    // Shared state
    GCU_MUTEX_T mutex;
    gcomp_status_t error;      // First error encountered
    
    // Job coordination
    GCU_Semaphore jobs_available;
    GCU_Semaphore jobs_complete;
    
    // Job queue (protected by mutex)
    // ...
} parallel_state_t;

static GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION
worker_thread(GCU_THREAD_FUNC_ARG_T arg) {
    parallel_state_t *state = (parallel_state_t *)arg;
    
    while (1) {
        // Wait for a job
        gcu_semaphore_wait(&state->jobs_available);
        
        // Check for shutdown signal
        GCU_MUTEX_LOCK(state->mutex);
        // ... get job from queue or check shutdown flag ...
        GCU_MUTEX_UNLOCK(state->mutex);
        
        // Process job
        gcomp_status_t result = process_job(/* ... */);
        
        // Report errors
        if (result != GCOMP_OK) {
            GCU_MUTEX_LOCK(state->mutex);
            if (state->error == GCOMP_OK) {
                state->error = result;  // Record first error
            }
            GCU_MUTEX_UNLOCK(state->mutex);
        }
        
        // Signal completion
        gcu_semaphore_signal(&state->jobs_complete);
    }
    
    return 0;
}
```

## Memory Bounds with Parallelism

Methods must respect `limits.max_memory_bytes` even when using multiple threads. Strategies:

### 1. Limit In-Flight Jobs

Use a semaphore to bound the number of concurrent jobs:

```c
// At initialization
int max_jobs = max_memory_bytes / per_job_memory;
gcu_semaphore_create(&job_slots, max_jobs);

// Before starting a job
gcu_semaphore_wait(&job_slots);  // Blocks if at limit

// After job completes
gcu_semaphore_signal(&job_slots);
```

### 2. Shared Memory Tracking

If using the framework's `gcomp_memory_tracker_t` across threads, protect it with a mutex:

```c
GCU_MUTEX_LOCK(memory_mutex);
gcomp_memory_track_alloc(tracker, size);
gcomp_status_t status = gcomp_memory_check_limit(tracker, limit);
GCU_MUTEX_UNLOCK(memory_mutex);

if (status != GCOMP_OK) {
    // Over limit - wait for other jobs to free memory
}
```

### 3. Per-Thread Tracking

Alternatively, track memory per-thread and merge periodically:

```c
// Each thread has local tracking
__thread size_t local_memory = 0;

// Periodically merge to shared tracker (with mutex)
```

## Error Propagation

Worker threads cannot directly return errors to the caller. Instead:

1. **Record the first error** in shared state (protected by mutex)
2. **Check for errors** in the main thread after joining workers
3. **Surface errors** via the encoder/decoder error state

```c
// After joining all workers
if (parallel_state.error != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, parallel_state.error,
        "Worker thread failed: %s", gcomp_status_name(parallel_state.error));
    return parallel_state.error;
}
```

## Requirements

### Single-Threaded Mode Must Always Work

Every method that supports `threads.count > 1` **must** also work correctly with `threads.count = 1` (single-threaded). This ensures:

- Methods work on systems where threading may be problematic
- Users can disable parallelism for debugging
- Fallback behavior when `cutil` is not linked

### Thread Safety of Framework Components

The compress library framework is **not** internally thread-safe. Methods are responsible for:

- Protecting any shared access to `gcomp_memory_tracker_t`
- Not calling encoder/decoder APIs from multiple threads simultaneously
- Synchronizing access to shared buffers

### Cleanup on Error

If an error occurs, methods must:

1. Signal all worker threads to stop
2. Join all threads before returning
3. Free all allocated resources

Do not leave orphaned threads running after returning an error.

## Future Considerations

The following may be added to the framework if multiple methods need them:

- **Thread pool helper** - Shared pool to avoid thread creation overhead
- **Atomic memory tracker** - Lock-free memory accounting
- **Job queue abstraction** - Generic producer/consumer queue

These will be implemented when a concrete need arises, not speculatively.
