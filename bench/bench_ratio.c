/**
 * @file bench_ratio.c
 *
 * Compression ratio against the reference implementations.
 *
 * WHAT THIS MEASURES
 * ==================
 *
 * For every file named on the command line, every method and level below is
 * used to compress it with this library and with the reference implementation
 * for that format, and the two sizes are printed side by side.  The reference
 * libraries are opened at run time, so a machine without one simply reports
 * nothing for that method rather than failing to build.
 *
 * References: zlib for deflate, libzstd for zstd, liblz4 for lz4.  Each is
 * given settings matching the case being measured -- the same level, and for
 * lz4 the same block size and block mode -- because comparing our defaults
 * against theirs measures the defaults, not the encoders.  An earlier version
 * of this harness did exactly that and reported lz4 as 4.7% behind liblz4
 * when, like for like, it was ahead.
 *
 * zlib is reached through compress2(), which writes the zlib container rather
 * than a raw deflate stream: six bytes of header and Adler-32 checksum, which
 * are subtracted so that the comparison is deflate against deflate.
 *
 * EVERY STREAM IS READ BACK
 * =========================
 *
 * Our output is decompressed and compared against the input before its size
 * is reported.  A ratio for a stream that cannot be read back is not a
 * measurement of anything, and an encoder that drops data always looks good.
 *
 * TIME IS MEASURED TOO
 * ====================
 *
 * A ratio on its own says nothing about whether it was worth having, so every
 * case is also timed: compressing, and decompressing what was compressed, for
 * both this library and the reference.  Each measurement repeats the work
 * until at least BENCH_MIN_SECONDS of wall time has gone by (up to
 * BENCH_MAX_REPEATS times) and keeps the *fastest* run, because interference
 * from the rest of the machine can only ever make a run slower.
 *
 * Times are reported as throughput over the uncompressed size, which is the
 * number that stays comparable across inputs of different sizes, and the
 * decompression side is charged the same way -- bytes produced per second.
 *
 * The round trip that validates the output is timed as the decompression
 * measurement, so nothing is compressed or decompressed merely to be timed.
 *
 * USAGE
 * =====
 *
 *   bench_ratio FILE...
 *
 * With no arguments it generates a small set of synthetic inputs instead:
 * text-like bytes, an incompressible run, and bytes drawn from a skewed
 * alphabet, which is the shape that makes Huffman length caps bind.
 *
 * Copyright 2026 by Corey Pennycuff
 */

// clock_gettime and CLOCK_MONOTONIC; -std=c17 alone does not declare them.
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

//
// Timing
//

/// Repeat a measurement until this much wall time has been spent on it.
#define BENCH_MIN_SECONDS 0.35

/// ...but never more than this many times, so a slow case still finishes.
#define BENCH_MAX_REPEATS 25

/// A measurement that produced nothing has no time worth reporting.
#define BENCH_NO_TIME 0.0

static double bench_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/**
 * @brief Throughput in MiB/s over the uncompressed size.
 *
 * Zero seconds means the measurement did not happen (the reference is not
 * installed, or the case failed), not that it was infinitely fast.
 */
static double bench_mb_s(size_t bytes, double seconds) {
  if (seconds <= 0.0) {
    return 0.0;
  }
  return ((double)bytes / (1024.0 * 1024.0)) / seconds;
}

static void bench_print_speed(double mb_s) {
  if (mb_s <= 0.0) {
    printf(" %9s", "-");
  }
  else {
    printf(" %9.1f", mb_s);
  }
}

//
// Reference implementations, opened at run time
//

// zlib
static int (*zlib_compress2)(
    uint8_t *, unsigned long *, const uint8_t *, unsigned long, int);
static unsigned long (*zlib_compressBound)(unsigned long);

// libzstd
static size_t (*zstd_compress)(void *, size_t, const void *, size_t, int);
static size_t (*zstd_compressBound)(size_t);
static unsigned (*zstd_isError)(size_t);

// liblz4.  The preferences struct is declared here because liblz4's headers
// are not always installed; the layout is the one liblz4 has used since 1.8.
typedef struct {
  unsigned blockSizeID;
  unsigned blockMode;
  unsigned contentChecksumFlag;
  unsigned frameType;
  unsigned long long contentSize;
  unsigned dictID;
  unsigned blockChecksumFlag;
} bench_lz4_frameinfo_t;

typedef struct {
  bench_lz4_frameinfo_t frameInfo;
  int compressionLevel;
  unsigned autoFlush;
  unsigned favorDecSpeed;
  unsigned reserved[3];
} bench_lz4_prefs_t;

static size_t (*lz4_compressFrame)(
    void *, size_t, const void *, size_t, const bench_lz4_prefs_t *);
static size_t (*lz4_compressFrameBound)(size_t, const void *);
static unsigned (*lz4_isError)(size_t);

// Decompression, so that the reference's read-back speed can be reported
// beside ours.  liblz4's frame decoder is the only one that is not a single
// call: it is fed until it says the frame is complete.
static int (*zlib_uncompress)(
    uint8_t *, unsigned long *, const uint8_t *, unsigned long);
static size_t (*zstd_decompress)(void *, size_t, const void *, size_t);
static size_t (*lz4_createDecompressionContext)(void **, unsigned);
static size_t (*lz4_freeDecompressionContext)(void *);
static size_t (*lz4_decompress)(
    void *, void *, size_t *, const void *, size_t *, const void *);

/// The version liblz4 expects from LZ4F_createDecompressionContext.
#define BENCH_LZ4F_VERSION 100

static void * bench_dlopen(const char * name) {
  return dlopen(name, RTLD_NOW);
}

static void bench_load_references(void) {
  void * z = bench_dlopen("libz.so.1");
  if (z) {
    *(void **)&zlib_compress2 = dlsym(z, "compress2");
    *(void **)&zlib_compressBound = dlsym(z, "compressBound");
    *(void **)&zlib_uncompress = dlsym(z, "uncompress");
  }
  void * zs = bench_dlopen("libzstd.so.1");
  if (zs) {
    *(void **)&zstd_compress = dlsym(zs, "ZSTD_compress");
    *(void **)&zstd_compressBound = dlsym(zs, "ZSTD_compressBound");
    *(void **)&zstd_isError = dlsym(zs, "ZSTD_isError");
    *(void **)&zstd_decompress = dlsym(zs, "ZSTD_decompress");
  }
  void * l4 = bench_dlopen("liblz4.so.1");
  if (l4) {
    *(void **)&lz4_compressFrame = dlsym(l4, "LZ4F_compressFrame");
    *(void **)&lz4_compressFrameBound = dlsym(l4, "LZ4F_compressFrameBound");
    *(void **)&lz4_isError = dlsym(l4, "LZ4F_isError");
    *(void **)&lz4_createDecompressionContext =
        dlsym(l4, "LZ4F_createDecompressionContext");
    *(void **)&lz4_freeDecompressionContext =
        dlsym(l4, "LZ4F_freeDecompressionContext");
    *(void **)&lz4_decompress = dlsym(l4, "LZ4F_decompress");
  }
}

//
// Cases
//

typedef struct {
  const char * label;
  const char * method;
  const char * level_key; ///< NULL when the method has no level option.
  int level;
  int reference;   ///< 1 zlib, 2 libzstd, 3 liblz4
  int lz4_linked;  ///< lz4 only: 1 for linked blocks
  unsigned lz4_block_id; ///< lz4 only: 4 = 64 KiB, 7 = 4 MiB
} bench_case_t;

static const bench_case_t k_cases[] = {
    {"deflate-1", "deflate", "deflate.level", 1, 1, 0, 0},
    {"deflate-6", "deflate", "deflate.level", 6, 1, 0, 0},
    // Levels 7 and up parse by shortest path rather than deferring, and
    // these two sit either side of what that costs and buys: 7 is the first
    // level to do it, 9 one that spends more on it.
    {"deflate-7", "deflate", "deflate.level", 7, 1, 0, 0},
    {"deflate-9", "deflate", "deflate.level", 9, 1, 0, 0},
    {"zstd-1", "zstd", "zstd.level", 1, 2, 0, 0},
    {"zstd-3", "zstd", "zstd.level", 3, 2, 0, 0},
    {"zstd-9", "zstd", "zstd.level", 9, 2, 0, 0},
    // Levels 16 and up parse by shortest path rather than greedily, and the
    // two cases below sit either side of what that costs and buys: 16 is the
    // first level to do it, 19 one that spends more on it.  Without a case
    // above level 10 nothing here measures that half of the encoder at all.
    {"zstd-16", "zstd", "zstd.level", 16, 2, 0, 0},
    {"zstd-19", "zstd", "zstd.level", 19, 2, 0, 0},
    {"lz4-4M", "lz4", NULL, 0, 3, 0, 7},
    {"lz4-64K", "lz4", NULL, 0, 3, 0, 4},
    {"lz4-64K-linked", "lz4", NULL, 0, 3, 1, 4},
};

#define BENCH_CASE_COUNT (sizeof(k_cases) / sizeof(k_cases[0]))

//
// A measured case
//

typedef struct {
  size_t size;    ///< Compressed size, or 0 if the case produced nothing.
  double encode;  ///< Seconds for the fastest compression run.
  double decode;  ///< Seconds for the fastest decompression run.
} bench_result_t;

/**
 * @brief Whether a repeated measurement should go round again.
 *
 * @param iterations How many runs have been done.
 * @param spent Seconds spent on them.
 */
static bool bench_repeat_again(int iterations, double spent) {
  return iterations < BENCH_MAX_REPEATS && spent < BENCH_MIN_SECONDS;
}

//
// Ours, round-tripped
//

static bench_result_t bench_ours(gcomp_registry_t * registry,
    const bench_case_t * c, const uint8_t * in, size_t n,
    const char ** error_out) {
  bench_result_t result = {0, BENCH_NO_TIME, BENCH_NO_TIME};
  gcomp_options_t * options = NULL;
  if (gcomp_options_create(&options) != GCOMP_OK) {
    *error_out = "options";
    return result;
  }
  if (c->level_key) {
    gcomp_options_set_int64(options, c->level_key, c->level);
  }
  if (c->reference == 3) {
    gcomp_options_set_bool(
        options, "lz4.independent_blocks", c->lz4_linked ? false : true);
    gcomp_options_set_uint64(options, "lz4.block_size",
        c->lz4_block_id == 7 ? 4194304u : 65536u);
  }

  size_t capacity = n + n / 2 + 4096;
  uint8_t * out = (uint8_t *)malloc(capacity);
  uint8_t * back = (uint8_t *)malloc(n + 64);
  size_t out_len = 0;
  size_t back_len = 0;
  double best_encode = 0.0;
  double best_decode = 0.0;

  if (!out || !back) {
    *error_out = "memory";
    goto done;
  }

  // Compression.  The buffers are allocated above so that the loop times the
  // encoder and not the allocator.
  {
    double spent = 0.0;
    for (int i = 0; bench_repeat_again(i, spent); i++) {
      double t0 = bench_now();
      if (gcomp_encode_buffer(registry, c->method, options, in, n, out,
              capacity, &out_len) != GCOMP_OK) {
        *error_out = "encode";
        goto done;
      }
      double elapsed = bench_now() - t0;
      spent += elapsed;
      if (i == 0 || elapsed < best_encode) {
        best_encode = elapsed;
      }
    }
  }

  // Decompression, which is also the check that the stream is worth reporting
  // a size for at all.
  {
    double spent = 0.0;
    for (int i = 0; bench_repeat_again(i, spent); i++) {
      double t0 = bench_now();
      if (gcomp_decode_buffer(registry, c->method, NULL, out, out_len, back,
              n + 64, &back_len) != GCOMP_OK) {
        *error_out = "decode";
        goto done;
      }
      double elapsed = bench_now() - t0;
      spent += elapsed;
      if (i == 0 || elapsed < best_decode) {
        best_decode = elapsed;
      }
      if (back_len != n || (n > 0 && memcmp(back, in, n) != 0)) {
        *error_out = "MISMATCH";
        goto done;
      }
    }
  }

  result.size = out_len;
  result.encode = best_encode;
  result.decode = best_decode;

done:
  free(out);
  free(back);
  gcomp_options_destroy(options);
  return result;
}

//
// Reference
//

/**
 * @brief Decompress one liblz4 frame, which takes a loop rather than a call.
 *
 * @return true if the whole frame was read back.
 */
static bool bench_lz4_read_back(
    const uint8_t * src, size_t src_size, uint8_t * dst, size_t dst_capacity) {
  void * dctx = NULL;
  if (lz4_createDecompressionContext(&dctx, BENCH_LZ4F_VERSION) != 0 || !dctx) {
    return false;
  }
  size_t src_pos = 0;
  size_t dst_pos = 0;
  bool complete = false;
  while (src_pos < src_size) {
    size_t dst_room = dst_capacity - dst_pos;
    size_t src_left = src_size - src_pos;
    size_t hint = lz4_decompress(
        dctx, dst + dst_pos, &dst_room, src + src_pos, &src_left, NULL);
    if (lz4_isError(hint)) {
      break;
    }
    dst_pos += dst_room;
    src_pos += src_left;
    if (hint == 0) {
      complete = true;
      break;
    }
    if (dst_room == 0 && src_left == 0) {
      break; // No progress: refuse to spin.
    }
  }
  lz4_freeDecompressionContext(dctx);
  return complete;
}

static bench_result_t bench_reference(
    const bench_case_t * c, const uint8_t * in, size_t n) {
  bench_result_t result = {0, BENCH_NO_TIME, BENCH_NO_TIME};

  size_t capacity = 0;
  if (c->reference == 1) {
    if (!zlib_compress2 || !zlib_compressBound) {
      return result;
    }
    capacity = (size_t)zlib_compressBound((unsigned long)n) + 64;
  }
  else if (c->reference == 2) {
    if (!zstd_compress || !zstd_compressBound || !zstd_isError) {
      return result;
    }
    capacity = zstd_compressBound(n);
  }
  else if (c->reference == 3) {
    if (!lz4_compressFrame || !lz4_compressFrameBound || !lz4_isError) {
      return result;
    }
    capacity = lz4_compressFrameBound(n, NULL);
  }
  else {
    return result;
  }

  uint8_t * out = (uint8_t *)malloc(capacity);
  uint8_t * back = (uint8_t *)malloc(n + 64);
  if (!out || !back) {
    free(out);
    free(back);
    return result;
  }

  bench_lz4_prefs_t prefs;
  memset(&prefs, 0, sizeof(prefs));
  prefs.frameInfo.blockSizeID = c->lz4_block_id;
  prefs.frameInfo.blockMode = c->lz4_linked ? 0u : 1u;

  // Compression, timed the same way ours is.
  size_t out_len = 0;
  double best_encode = 0.0;
  bool ok = true;
  double spent = 0.0;
  for (int i = 0; ok && bench_repeat_again(i, spent); i++) {
    double t0 = bench_now();
    if (c->reference == 1) {
      unsigned long len = (unsigned long)capacity;
      ok = zlib_compress2(out, &len, in, (unsigned long)n, c->level) == 0;
      out_len = (size_t)len;
    }
    else if (c->reference == 2) {
      out_len = zstd_compress(out, capacity, in, n, c->level);
      ok = !zstd_isError(out_len);
    }
    else {
      out_len = lz4_compressFrame(out, capacity, in, n, &prefs);
      ok = !lz4_isError(out_len);
    }
    double elapsed = bench_now() - t0;
    spent += elapsed;
    if (i == 0 || elapsed < best_encode) {
      best_encode = elapsed;
    }
  }
  if (!ok) {
    free(out);
    free(back);
    return result;
  }

  // Decompression.  A reference that cannot be read back is reported without
  // a decompression time rather than dropped: its size is still a fact.
  double best_decode = 0.0;
  bool readable = true;
  spent = 0.0;
  for (int i = 0; readable && bench_repeat_again(i, spent); i++) {
    double t0 = bench_now();
    if (c->reference == 1) {
      unsigned long len = (unsigned long)(n + 64);
      readable = zlib_uncompress &&
          zlib_uncompress(back, &len, out, (unsigned long)out_len) == 0 &&
          (size_t)len == n;
    }
    else if (c->reference == 2) {
      readable = zstd_decompress != NULL;
      if (readable) {
        size_t len = zstd_decompress(back, n + 64, out, out_len);
        readable = !zstd_isError(len) && len == n;
      }
    }
    else {
      readable = lz4_decompress && lz4_createDecompressionContext &&
          lz4_freeDecompressionContext &&
          bench_lz4_read_back(out, out_len, back, n + 64);
    }
    double elapsed = bench_now() - t0;
    spent += elapsed;
    if (i == 0 || elapsed < best_decode) {
      best_decode = elapsed;
    }
  }

  // Subtract the zlib container: two header bytes and a four byte Adler-32,
  // so that this is deflate against deflate.
  if (c->reference == 1) {
    out_len = out_len > 6 ? out_len - 6 : 0;
  }

  result.size = out_len;
  result.encode = best_encode;
  result.decode = readable ? best_decode : BENCH_NO_TIME;
  free(out);
  free(back);
  return result;
}

//
// Synthetic inputs, used when no files are named
//

static uint8_t * bench_make_text(size_t n) {
  static const char * words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "then ", "returns ", "home ", "with "};
  const size_t count = sizeof(words) / sizeof(words[0]);
  uint8_t * out = (uint8_t *)malloc(n);
  if (!out) {
    return NULL;
  }
  size_t pos = 0;
  uint32_t seed = 12345u;
  while (pos < n) {
    seed = seed * 1103515245u + 12345u;
    const char * w = words[(seed >> 16) % count];
    for (const char * c = w; *c && pos < n; c++) {
      out[pos++] = (uint8_t)*c;
    }
  }
  return out;
}

static uint8_t * bench_make_noise(size_t n) {
  uint8_t * out = (uint8_t *)malloc(n);
  if (!out) {
    return NULL;
  }
  uint32_t x = 88675123u;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = (uint8_t)(x >> 19);
  }
  return out;
}

static uint8_t * bench_make_skewed(size_t n) {
  uint8_t * out = (uint8_t *)malloc(n);
  if (!out) {
    return NULL;
  }
  uint32_t x = 99137u;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    uint32_t r = x >> 8;
    unsigned symbol = 0;
    while (symbol < 200 && (r & 1u)) {
      symbol++;
      r >>= 1;
    }
    out[i] = (uint8_t)symbol;
  }
  return out;
}

/**
 * Read a corpus file.
 *
 * Through cutil rather than fseek/ftell: the size-first version returned NULL
 * for anything that reports no size, and assigned fread()'s result to *n_out,
 * so a short read quietly benchmarked fewer bytes than it had allocated.  The
 * buffer comes from cutil's allocator, so bench_free_input() below releases it
 * rather than free().
 */
static uint8_t * bench_read_file(const char * path, size_t * n_out) {
  void * data = NULL;
  size_t len = 0;
  if (gcu_file_read(path, GCU_FILE_UNLIMITED, NULL, &data, &len)
      != GCU_FILE_OK) {
    return NULL;
  }
  *n_out = len;
  return (uint8_t *)data;
}

/**
 * Release an input buffer.
 *
 * The generated corpora come from malloc() and the read ones from cutil, and
 * an allocator has to be given back what it handed out.  The flag is carried
 * rather than guessed.
 */
static void bench_free_input(uint8_t * data, int from_file) {
  if (from_file) {
    gcu_file_free(NULL, data);
  }
  else {
    free(data);
  }
}

int main(int argc, char ** argv) {
  bench_load_references();
  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "no registry\n");
    return 1;
  }

  size_t raw_total[BENCH_CASE_COUNT];
  size_t ours_total[BENCH_CASE_COUNT];
  size_t ref_total[BENCH_CASE_COUNT];
  // Throughput is aggregated as total bytes over total seconds rather than by
  // averaging the per-file rates, so that a large file counts for more than a
  // small one -- an average of rates would let data.csv outvote code.c.txt.
  size_t timed_bytes[BENCH_CASE_COUNT];
  size_t ref_timed_bytes[BENCH_CASE_COUNT];
  size_t ref_dec_bytes[BENCH_CASE_COUNT];
  double ours_encode_s[BENCH_CASE_COUNT];
  double ours_decode_s[BENCH_CASE_COUNT];
  double ref_encode_s[BENCH_CASE_COUNT];
  double ref_decode_s[BENCH_CASE_COUNT];
  memset(raw_total, 0, sizeof(raw_total));
  memset(ours_total, 0, sizeof(ours_total));
  memset(ref_total, 0, sizeof(ref_total));
  memset(timed_bytes, 0, sizeof(timed_bytes));
  memset(ref_timed_bytes, 0, sizeof(ref_timed_bytes));
  memset(ref_dec_bytes, 0, sizeof(ref_dec_bytes));
  memset(ours_encode_s, 0, sizeof(ours_encode_s));
  memset(ours_decode_s, 0, sizeof(ours_decode_s));
  memset(ref_encode_s, 0, sizeof(ref_encode_s));
  memset(ref_decode_s, 0, sizeof(ref_decode_s));

  // A table of bare numbers is a table nobody can read six months later, so
  // say what they are and which way is better before printing any.
  printf("Sizes are bytes; smaller is better, and a negative delta means our\n"
         "output is smaller than the reference's.  Speeds are MiB of\n"
         "UNCOMPRESSED data per second, so compressing and decompressing are\n"
         "charged the same way; higher is better.  A ratio of ours to the\n"
         "reference is written 1.00x for parity, 0.50x for half the speed.\n\n");

  printf("%-16s %-16s %10s %10s %10s %9s %9s %9s\n", "input", "case", "raw",
      "ours", "reference", "delta", "enc MiB/s", "ref MiB/s");

  int inputs = argc > 1 ? argc - 1 : 3;
  for (int a = 0; a < inputs; a++) {
    const char * name;
    uint8_t * data = NULL;
    size_t n = 0;
    int data_from_file = 0;

    if (argc > 1) {
      name = argv[a + 1];
      const char * slash = strrchr(name, '/');
      if (slash) {
        name = slash + 1;
      }
      data = bench_read_file(argv[a + 1], &n);
      data_from_file = 1;
    }
    else {
      n = 1u << 20;
      if (a == 0) {
        name = "text";
        data = bench_make_text(n);
      }
      else if (a == 1) {
        name = "noise";
        data = bench_make_noise(n);
      }
      else {
        name = "skewed";
        data = bench_make_skewed(n);
      }
    }
    if (!data || n == 0) {
      bench_free_input(data, data_from_file);
      continue;
    }

    for (size_t c = 0; c < BENCH_CASE_COUNT; c++) {
      const char * error = NULL;
      bench_result_t ours = bench_ours(registry, &k_cases[c], data, n, &error);
      bench_result_t reference = bench_reference(&k_cases[c], data, n);
      if (ours.size == 0) {
        printf("%-16s %-16s %10zu %10s\n", name, k_cases[c].label, n,
            error ? error : "FAIL");
        continue;
      }
      if (reference.size == 0) {
        printf("%-16s %-16s %10zu %10zu %10s %9s", name, k_cases[c].label, n,
            ours.size, "-", "-");
      }
      else {
        printf("%-16s %-16s %10zu %10zu %10zu %+8.2f%%", name,
            k_cases[c].label, n, ours.size, reference.size,
            100.0 * ((double)ours.size - (double)reference.size) /
                (double)reference.size);
        ref_total[c] += reference.size;
      }
      bench_print_speed(bench_mb_s(n, ours.encode));
      bench_print_speed(bench_mb_s(n, reference.encode));
      printf("\n");

      raw_total[c] += n;
      ours_total[c] += ours.size;
      if (ours.encode > 0.0) {
        timed_bytes[c] += n;
        ours_encode_s[c] += ours.encode;
        ours_decode_s[c] += ours.decode;
      }
      if (reference.encode > 0.0) {
        ref_timed_bytes[c] += n;
        ref_encode_s[c] += reference.encode;
      }
      if (reference.decode > 0.0) {
        ref_dec_bytes[c] += n;
        ref_decode_s[c] += reference.decode;
      }
    }
    bench_free_input(data, data_from_file);
  }

  printf("\n%-16s %-16s %10s %10s %10s %9s\n", "TOTAL", "case", "raw", "ours",
      "reference", "delta");
  for (size_t c = 0; c < BENCH_CASE_COUNT; c++) {
    if (raw_total[c] == 0) {
      continue;
    }
    if (ref_total[c] == 0) {
      printf("%-16s %-16s %10zu %10zu %10s\n", "TOTAL", k_cases[c].label,
          raw_total[c], ours_total[c], "-");
      continue;
    }
    printf("%-16s %-16s %10zu %10zu %10zu %+8.2f%%\n", "TOTAL",
        k_cases[c].label, raw_total[c], ours_total[c], ref_total[c],
        100.0 * ((double)ours_total[c] - (double)ref_total[c]) /
            (double)ref_total[c]);
  }

  // Throughput, in MiB of *uncompressed* data per second, so that the
  // compression and decompression columns are charged the same way.
  printf("\nSPEED: MiB of uncompressed data per second, higher is better.\n");
  printf("  \"ours/ref\" is our rate divided by the reference's: 1.00x is "
         "parity.\n\n");
  printf("%-16s %-16s %9s %9s %9s %9s %9s %9s\n", "SPEED", "case",
      "enc ours", "enc ref", "ours/ref", "dec ours", "dec ref",
      "ours/ref");
  for (size_t c = 0; c < BENCH_CASE_COUNT; c++) {
    if (timed_bytes[c] == 0) {
      continue;
    }
    double ours_enc = bench_mb_s(timed_bytes[c], ours_encode_s[c]);
    double ours_dec = bench_mb_s(timed_bytes[c], ours_decode_s[c]);
    double ref_enc = bench_mb_s(ref_timed_bytes[c], ref_encode_s[c]);
    double ref_dec = bench_mb_s(ref_dec_bytes[c], ref_decode_s[c]);

    printf("%-16s %-16s", "SPEED", k_cases[c].label);
    bench_print_speed(ours_enc);
    bench_print_speed(ref_enc);
    if (ref_enc > 0.0 && ours_enc > 0.0) {
      printf(" %8.2fx", ours_enc / ref_enc);
    }
    else {
      printf(" %9s", "-");
    }
    bench_print_speed(ours_dec);
    bench_print_speed(ref_dec);
    if (ref_dec > 0.0 && ours_dec > 0.0) {
      printf(" %8.2fx", ours_dec / ref_dec);
    }
    else {
      printf(" %9s", "-");
    }
    printf("\n");
  }
  return 0;
}
