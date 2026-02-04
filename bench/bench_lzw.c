/**
 * @file bench_lzw.c
 *
 * Microbenchmark for LZW encoder lookup modes: linear vs hash.
 *
 * Runs both lzw.encoder_lookup=linear and lzw.encoder_lookup=hash on the same
 * input and reports encode time/throughput (MB/s). Used to validate that the
 * hashed lookup improves performance. Not a CI gate; developer tool only.
 *
 * USAGE
 * =====
 *
 * bench_lzw [--size SIZE_MB] [--iterations N]
 *
 * Options:
 *   --size SIZE_MB    Input size in megabytes (default: 4)
 *   --iterations N    Minimum iterations per mode (default: 5)
 *
 * Build: make bench
 * Run:   LD_LIBRARY_PATH=./build/linux/release/apps ./build/.../bench_lzw
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 199309L

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SIZE_MB 4
#define DEFAULT_ITERATIONS 5
#define MIN_TIMING_MS 100

static double get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* Generate mixed data (some repetition, some random) for LZW. */
static void generate_data(uint8_t * buf, size_t size, uint32_t seed) {
  uint32_t state = seed;
  for (size_t i = 0; i < size; i++) {
    state = state * 1664525u + 1013904223u;
    if ((i % 100) < 60) {
      buf[i] = (uint8_t)('A' + (i % 26));
    }
    else {
      buf[i] = (uint8_t)(state >> 24);
    }
  }
}

typedef struct {
  double throughput_mbps;
  size_t compressed_size;
  int iterations_run;
} lzw_bench_result_t;

static gcomp_status_t run_lzw_encode_bench(gcomp_registry_t * registry,
    const char * lookup_mode, const uint8_t * input, size_t input_size,
    int target_iterations, lzw_bench_result_t * result) {

  gcomp_options_t * opts = NULL;
  gcomp_status_t status = gcomp_options_create(&opts);
  if (status != GCOMP_OK) {
    return status;
  }
  status = gcomp_options_set_string(opts, "lzw.encoder_lookup", lookup_mode);
  if (status != GCOMP_OK) {
    gcomp_options_destroy(opts);
    return status;
  }

  /* LZW can expand; reserve enough for worst case (e.g. 12-bit codes). */
  size_t out_cap = input_size + (input_size / 2) + 4096;
  uint8_t * compressed = (uint8_t *)malloc(out_cap);
  if (!compressed) {
    gcomp_options_destroy(opts);
    return GCOMP_ERR_MEMORY;
  }

  size_t compressed_size = 0;
  status = gcomp_encode_buffer(registry, "lzw", opts, input, input_size,
      compressed, out_cap, &compressed_size);
  if (status != GCOMP_OK) {
    free(compressed);
    gcomp_options_destroy(opts);
    return status;
  }

  result->compressed_size = compressed_size;

  double start = get_time_ms();
  int iterations = 0;
  double elapsed = 0;
  while (iterations < target_iterations || elapsed < MIN_TIMING_MS) {
    size_t out_len = 0;
    status = gcomp_encode_buffer(registry, "lzw", opts, input, input_size,
        compressed, out_cap, &out_len);
    if (status != GCOMP_OK) {
      free(compressed);
      gcomp_options_destroy(opts);
      return status;
    }
    iterations++;
    elapsed = get_time_ms() - start;
  }

  double sec = elapsed / 1000.0;
  double total_mb = (double)input_size * (double)iterations / (1024.0 * 1024.0);
  result->throughput_mbps = total_mb / sec;
  result->iterations_run = iterations;

  free(compressed);
  gcomp_options_destroy(opts);
  return GCOMP_OK;
}

static void print_usage(const char * prog) {
  printf("Usage: %s [OPTIONS]\n", prog);
  printf("\n");
  printf("LZW encoder lookup mode benchmark (linear vs hash).\n");
  printf("\n");
  printf("Options:\n");
  printf("  --size SIZE_MB    Input size in megabytes (default: %d)\n",
      DEFAULT_SIZE_MB);
  printf("  --iterations N    Minimum iterations per mode (default: %d)\n",
      DEFAULT_ITERATIONS);
  printf("  --help            Show this help\n");
}

int main(int argc, char * argv[]) {
  int size_mb = DEFAULT_SIZE_MB;
  int iterations = DEFAULT_ITERATIONS;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    }
    if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
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

  size_t input_size = (size_t)size_mb * 1024u * 1024u;

  printf("========================================\n");
  printf("LZW Encoder Lookup Mode Benchmark\n");
  printf("========================================\n");
  printf("Input size: %d MB\n", size_mb);
  printf("Min iterations per mode: %d\n", iterations);
  printf("\n");

  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "Failed to get default registry\n");
    return 1;
  }

  if (!gcomp_registry_find(registry, "lzw")) {
    fprintf(stderr, "lzw method not found in registry\n");
    return 1;
  }

  uint8_t * input = (uint8_t *)malloc(input_size);
  if (!input) {
    fprintf(stderr, "Failed to allocate %zu bytes for input\n", input_size);
    return 1;
  }
  generate_data(input, input_size, 12345);

  static const char * modes[] = {"linear", "hash"};
  const size_t num_modes = sizeof(modes) / sizeof(modes[0]);

  printf("%-10s %-12s %-14s %-10s\n", "Mode", "Comp Size", "Throughput MB/s",
      "Iterations");
  printf("%-10s %-12s %-14s %-10s\n", "----", "---------", "--------------",
      "----------");

  for (size_t m = 0; m < num_modes; m++) {
    lzw_bench_result_t result = {0};
    gcomp_status_t status = run_lzw_encode_bench(
        registry, modes[m], input, input_size, iterations, &result);

    if (status != GCOMP_OK) {
      printf("%-10s FAILED: %s\n", modes[m], gcomp_status_to_string(status));
      continue;
    }

    char size_str[32];
    if (result.compressed_size >= 1024 * 1024) {
      snprintf(size_str, sizeof(size_str), "%.2f MB",
          (double)result.compressed_size / (1024.0 * 1024.0));
    }
    else {
      snprintf(size_str, sizeof(size_str), "%.2f KB",
          (double)result.compressed_size / 1024.0);
    }

    printf("%-10s %-12s %-14.2f %-10d\n", modes[m], size_str,
        result.throughput_mbps, result.iterations_run);
  }

  free(input);
  printf("\nBenchmark complete.\n");
  return 0;
}
