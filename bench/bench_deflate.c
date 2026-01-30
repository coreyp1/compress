/**
 * @file bench_deflate.c
 *
 * Microbenchmark harness for deflate encode/decode throughput.
 *
 * METHODOLOGY
 * ===========
 *
 * This benchmark measures compression and decompression performance using the
 * buffer convenience API (gcomp_encode_buffer / gcomp_decode_buffer). Results
 * are reported in MB/s of input data processed.
 *
 * Test data types:
 * - TEXT: Simulated English text with common words and structure. Tests
 *   typical text compression scenarios with moderate redundancy.
 * - BINARY: Random bytes from a seeded LCG. Represents incompressible data;
 *   useful for measuring encoder overhead on worst-case inputs.
 * - REPEATED: A repeating 16-byte pattern. Represents highly compressible
 *   data; useful for measuring maximum throughput potential.
 * - MIXED: 50% repeated patterns, 25% text-like, 25% random. Represents
 *   real-world mixed content like archives or documents.
 *
 * Compression levels tested:
 * - Level 1: Fast compression with short hash chains, fixed Huffman
 * - Level 6: Default balanced compression with dynamic Huffman
 * - Level 9: Maximum compression with long hash chains
 *
 * Timing methodology:
 * - Each benchmark runs for at least MIN_TIMING_MS (100ms) to reduce noise
 * - Multiple iterations are averaged to produce stable results
 * - CLOCK_MONOTONIC is used for timing (not affected by system time changes)
 *
 * Quadratic behavior check:
 * - Tests throughput at different input sizes (64KB, 256KB, 1MB)
 * - Throughput should be roughly constant across sizes for O(n) algorithms
 * - Significant degradation (>3x slower) suggests algorithmic issues
 *
 * USAGE
 * =====
 *
 * bench_deflate [--size SIZE_MB] [--iterations N]
 *
 * Options:
 *   --size SIZE_MB    Input size in megabytes (default: 1)
 *   --iterations N    Minimum iterations per benchmark (default: 5)
 *
 * Build: make bench
 * Run:   LD_LIBRARY_PATH=./build/linux/release/apps ./build/.../bench_deflate
 *
 * Copyright 2026 by Corey Pennycuff
 */

// Enable POSIX features for clock_gettime
#define _POSIX_C_SOURCE 199309L

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Default benchmark parameters
#define DEFAULT_SIZE_MB 1
#define DEFAULT_ITERATIONS 5
#define MIN_TIMING_MS                                                          \
  100 // Minimum milliseconds for a benchmark to be considered valid

// Data types for benchmarking
typedef enum {
  DATA_TYPE_TEXT,     // Text-like data (printable ASCII, some repetition)
  DATA_TYPE_BINARY,   // Random binary data
  DATA_TYPE_REPEATED, // Highly repeated patterns (compresses well)
  DATA_TYPE_MIXED,    // Mix of patterns
  DATA_TYPE_COUNT
} data_type_t;

static const char * data_type_names[] = {
    "text",
    "binary",
    "repeated",
    "mixed",
};

// Compression levels to benchmark
static const int BENCH_LEVELS[] = {1, 6, 9};
#define NUM_LEVELS (sizeof(BENCH_LEVELS) / sizeof(BENCH_LEVELS[0]))

// Simple LCG random number generator for reproducibility
typedef struct {
  uint32_t state;
} bench_rng_t;

static void rng_init(bench_rng_t * rng, uint32_t seed) {
  rng->state = seed;
}

static uint32_t rng_next(bench_rng_t * rng) {
  // LCG parameters from Numerical Recipes
  rng->state = rng->state * 1664525u + 1013904223u;
  return rng->state;
}

static uint8_t rng_byte(bench_rng_t * rng) {
  return (uint8_t)(rng_next(rng) >> 24);
}

// Get current time in milliseconds
static double get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// Generate test data of specified type
static void generate_data(
    uint8_t * buffer, size_t size, data_type_t type, uint32_t seed) {
  bench_rng_t rng;
  rng_init(&rng, seed);

  switch (type) {
  case DATA_TYPE_TEXT: {
    // Generate text-like data: printable ASCII with some word/line structure
    static const char * words[] = {"the ", "of ", "and ", "to ", "a ", "in ",
        "is ", "it ", "for ", "on ", "with ", "as ", "was ", "that ", "be ",
        "by ", "are ", "at ", "have ", "this "};
    const size_t num_words = sizeof(words) / sizeof(words[0]);
    size_t pos = 0;
    while (pos < size) {
      // Pick a word
      size_t word_idx = rng_next(&rng) % num_words;
      const char * word = words[word_idx];
      size_t word_len = strlen(word);

      // Copy word (or partial if at end)
      size_t to_copy = (pos + word_len <= size) ? word_len : (size - pos);
      memcpy(buffer + pos, word, to_copy);
      pos += to_copy;

      // Occasionally add newline
      if (pos < size && (rng_next(&rng) % 20) == 0) {
        buffer[pos++] = '\n';
      }
    }
    break;
  }

  case DATA_TYPE_BINARY:
    // Random binary data
    for (size_t i = 0; i < size; i++) {
      buffer[i] = rng_byte(&rng);
    }
    break;

  case DATA_TYPE_REPEATED: {
    // Highly repeated pattern (compresses very well)
    static const uint8_t pattern[] = "ABCDEFGHIJKLMNOP";
    const size_t pattern_len = sizeof(pattern) - 1;
    for (size_t i = 0; i < size; i++) {
      buffer[i] = pattern[i % pattern_len];
    }
    break;
  }

  case DATA_TYPE_MIXED: {
    // Mix: 50% repeated, 25% text, 25% random
    size_t quarter = size / 4;

    // First half: repeated with variations
    for (size_t i = 0; i < size / 2; i++) {
      buffer[i] = (uint8_t)('A' + (i % 26));
      if ((i % 100) < 10) {
        buffer[i] = rng_byte(&rng);
      }
    }

    // Third quarter: text-like
    for (size_t i = size / 2; i < size / 2 + quarter; i++) {
      buffer[i] = (uint8_t)(' ' + (rng_next(&rng) % 95));
    }

    // Fourth quarter: random
    for (size_t i = size / 2 + quarter; i < size; i++) {
      buffer[i] = rng_byte(&rng);
    }
    break;
  }

  default:
    memset(buffer, 0, size);
    break;
  }
}

// Benchmark result structure
typedef struct {
  double encode_throughput_mbps; // MB/s for encoding
  double decode_throughput_mbps; // MB/s for decoding
  double compression_ratio;      // original size / compressed size
  size_t compressed_size;
  int iterations_run;
} bench_result_t;

// Run a single benchmark for a given data type and compression level
static gcomp_status_t run_benchmark(gcomp_registry_t * registry,
    const uint8_t * input_data, size_t input_size, int level,
    int target_iterations, bench_result_t * result) {

  gcomp_status_t status = GCOMP_OK;
  gcomp_options_t * opts = NULL;

  // Allocate output buffer (worst case: slightly larger than input for
  // incompressible data)
  size_t output_capacity = input_size + (input_size / 10) + 1024;
  uint8_t * compressed = (uint8_t *)malloc(output_capacity);
  uint8_t * decompressed = (uint8_t *)malloc(input_size);
  if (!compressed || !decompressed) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }

  // Create options with compression level
  status = gcomp_options_create(&opts);
  if (status != GCOMP_OK) {
    goto cleanup;
  }
  status = gcomp_options_set_int64(opts, "deflate.level", level);
  if (status != GCOMP_OK) {
    goto cleanup;
  }

  // First pass: compress once to get compressed size
  size_t compressed_size = 0;
  status = gcomp_encode_buffer(registry, "deflate", opts, input_data,
      input_size, compressed, output_capacity, &compressed_size);
  if (status != GCOMP_OK) {
    fprintf(
        stderr, "Initial encode failed: %s\n", gcomp_status_to_string(status));
    goto cleanup;
  }

  result->compressed_size = compressed_size;
  result->compression_ratio = (compressed_size > 0)
      ? ((double)input_size / (double)compressed_size)
      : 0;

  // Benchmark encoding
  double encode_start = get_time_ms();
  int encode_iterations = 0;
  double encode_elapsed = 0;

  while (
      encode_iterations < target_iterations || encode_elapsed < MIN_TIMING_MS) {
    size_t out_size = 0;
    status = gcomp_encode_buffer(registry, "deflate", opts, input_data,
        input_size, compressed, output_capacity, &out_size);
    if (status != GCOMP_OK) {
      fprintf(stderr, "Encode iteration failed: %s\n",
          gcomp_status_to_string(status));
      goto cleanup;
    }
    encode_iterations++;
    encode_elapsed = get_time_ms() - encode_start;
  }

  double encode_seconds = encode_elapsed / 1000.0;
  double encode_total_bytes = (double)input_size * encode_iterations;
  result->encode_throughput_mbps =
      (encode_total_bytes / (1024.0 * 1024.0)) / encode_seconds;

  // Benchmark decoding
  double decode_start = get_time_ms();
  int decode_iterations = 0;
  double decode_elapsed = 0;

  while (
      decode_iterations < target_iterations || decode_elapsed < MIN_TIMING_MS) {
    size_t out_size = 0;
    status = gcomp_decode_buffer(registry, "deflate", NULL, compressed,
        compressed_size, decompressed, input_size, &out_size);
    if (status != GCOMP_OK) {
      fprintf(stderr, "Decode iteration failed: %s\n",
          gcomp_status_to_string(status));
      goto cleanup;
    }
    decode_iterations++;
    decode_elapsed = get_time_ms() - decode_start;
  }

  double decode_seconds = decode_elapsed / 1000.0;
  double decode_total_bytes = (double)input_size * decode_iterations;
  result->decode_throughput_mbps =
      (decode_total_bytes / (1024.0 * 1024.0)) / decode_seconds;

  result->iterations_run = encode_iterations; // Report encode iterations

cleanup:
  if (opts) {
    gcomp_options_destroy(opts);
  }
  free(compressed);
  free(decompressed);
  return status;
}

// Check for quadratic behavior by comparing throughput at different sizes
static void check_quadratic_behavior(gcomp_registry_t * registry) {
  printf("\n");
  printf("========================================\n");
  printf("Checking for Quadratic Behavior\n");
  printf("========================================\n");
  printf("Testing decode throughput scaling with input size (should be roughly "
         "constant)\n\n");

  const size_t sizes[] = {
      64 * 1024,   // 64 KB
      256 * 1024,  // 256 KB
      1024 * 1024, // 1 MB
  };
  const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

  printf("%-12s %-12s %-15s %-15s\n", "Input Size", "Compressed", "Encode MB/s",
      "Decode MB/s");
  printf("%-12s %-12s %-15s %-15s\n", "----------", "----------", "-----------",
      "-----------");

  double prev_encode_throughput = 0;
  double prev_decode_throughput = 0;

  for (size_t i = 0; i < num_sizes; i++) {
    size_t size = sizes[i];
    uint8_t * data = (uint8_t *)malloc(size);
    if (!data) {
      fprintf(stderr, "Failed to allocate %zu bytes\n", size);
      continue;
    }

    // Use mixed data type
    generate_data(data, size, DATA_TYPE_MIXED, 12345);

    bench_result_t result = {0};
    gcomp_status_t status = run_benchmark(registry, data, size, 6, 3, &result);

    if (status == GCOMP_OK) {
      char size_str[32];
      if (size >= 1024 * 1024) {
        snprintf(size_str, sizeof(size_str), "%zu MB", size / (1024 * 1024));
      }
      else {
        snprintf(size_str, sizeof(size_str), "%zu KB", size / 1024);
      }

      char comp_str[32];
      if (result.compressed_size >= 1024 * 1024) {
        snprintf(comp_str, sizeof(comp_str), "%zu MB",
            result.compressed_size / (1024 * 1024));
      }
      else {
        snprintf(comp_str, sizeof(comp_str), "%zu KB",
            result.compressed_size / 1024);
      }

      printf("%-12s %-12s %-15.2f %-15.2f", size_str, comp_str,
          result.encode_throughput_mbps, result.decode_throughput_mbps);

      // Check for significant throughput degradation (> 2x slower suggests
      // quadratic)
      if (prev_encode_throughput > 0 &&
          result.encode_throughput_mbps < prev_encode_throughput * 0.3) {
        printf(" [WARN: encode slowdown]");
      }
      if (prev_decode_throughput > 0 &&
          result.decode_throughput_mbps < prev_decode_throughput * 0.3) {
        printf(" [WARN: decode slowdown]");
      }
      printf("\n");

      prev_encode_throughput = result.encode_throughput_mbps;
      prev_decode_throughput = result.decode_throughput_mbps;
    }
    else {
      printf("%-12zu FAILED: %s\n", size, gcomp_status_to_string(status));
    }

    free(data);
  }

  printf("\nNote: Throughput should be roughly constant across sizes.\n");
  printf("Significant degradation (>3x slower) may indicate quadratic "
         "behavior.\n");
}

static void print_usage(const char * prog) {
  printf("Usage: %s [OPTIONS]\n", prog);
  printf("\n");
  printf("Microbenchmark for deflate encode/decode throughput.\n");
  printf("\n");
  printf("Options:\n");
  printf("  --size SIZE_MB    Input size in megabytes (default: %d)\n",
      DEFAULT_SIZE_MB);
  printf("  --iterations N    Minimum iterations per benchmark (default: %d)\n",
      DEFAULT_ITERATIONS);
  printf("  --help            Show this help message\n");
  printf("\n");
  printf("The benchmark tests compression levels 1, 6, and 9 with various data "
         "types.\n");
}

int main(int argc, char * argv[]) {
  int size_mb = DEFAULT_SIZE_MB;
  int iterations = DEFAULT_ITERATIONS;

  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    }
    else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      size_mb = atoi(argv[++i]);
      if (size_mb < 1) {
        size_mb = 1;
      }
    }
    else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
      iterations = atoi(argv[++i]);
      if (iterations < 1) {
        iterations = 1;
      }
    }
    else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  size_t input_size = (size_t)size_mb * 1024 * 1024;

  printf("========================================\n");
  printf("Deflate Compression Benchmark\n");
  printf("========================================\n");
  printf("Input size: %d MB\n", size_mb);
  printf("Min iterations: %d\n", iterations);
  printf("\n");

  // Initialize registry (uses default with auto-registered deflate)
  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "Failed to get default registry\n");
    return 1;
  }

  // Verify deflate is available
  const gcomp_method_t * method = gcomp_registry_find(registry, "deflate");
  if (!method) {
    fprintf(stderr, "deflate method not found in registry\n");
    return 1;
  }
  printf("Using deflate method: %s (ABI version %d)\n", method->name,
      method->abi_version);
  printf("\n");

  // Allocate input buffer
  uint8_t * input_data = (uint8_t *)malloc(input_size);
  if (!input_data) {
    fprintf(stderr, "Failed to allocate %zu bytes for input\n", input_size);
    return 1;
  }

  // Run benchmarks for each data type
  for (data_type_t dt = 0; dt < DATA_TYPE_COUNT; dt++) {
    printf("----------------------------------------\n");
    printf("Data type: %s\n", data_type_names[dt]);
    printf("----------------------------------------\n");

    // Generate test data
    generate_data(input_data, input_size, dt, 42 + dt);

    printf("\n%-7s %-12s %-12s %-12s %-12s\n", "Level", "Comp Size", "Ratio",
        "Encode MB/s", "Decode MB/s");
    printf("%-7s %-12s %-12s %-12s %-12s\n", "-----", "---------", "-----",
        "-----------", "-----------");

    for (size_t lvl = 0; lvl < NUM_LEVELS; lvl++) {
      int level = BENCH_LEVELS[lvl];
      bench_result_t result = {0};

      gcomp_status_t status = run_benchmark(
          registry, input_data, input_size, level, iterations, &result);

      if (status == GCOMP_OK) {
        char comp_str[32];
        if (result.compressed_size >= 1024 * 1024) {
          snprintf(comp_str, sizeof(comp_str), "%.2f MB",
              (double)result.compressed_size / (1024.0 * 1024.0));
        }
        else {
          snprintf(comp_str, sizeof(comp_str), "%.2f KB",
              (double)result.compressed_size / 1024.0);
        }

        printf("%-7d %-12s %-12.2f %-12.2f %-12.2f\n", level, comp_str,
            result.compression_ratio, result.encode_throughput_mbps,
            result.decode_throughput_mbps);
      }
      else {
        printf("%-7d FAILED: %s\n", level, gcomp_status_to_string(status));
      }
    }
    printf("\n");
  }

  // Check for quadratic behavior
  check_quadratic_behavior(registry);

  free(input_data);

  printf("\nBenchmark complete.\n");
  return 0;
}
