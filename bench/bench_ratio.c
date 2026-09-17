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

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void * bench_dlopen(const char * name) {
  return dlopen(name, RTLD_NOW);
}

static void bench_load_references(void) {
  void * z = bench_dlopen("libz.so.1");
  if (z) {
    *(void **)&zlib_compress2 = dlsym(z, "compress2");
    *(void **)&zlib_compressBound = dlsym(z, "compressBound");
  }
  void * zs = bench_dlopen("libzstd.so.1");
  if (zs) {
    *(void **)&zstd_compress = dlsym(zs, "ZSTD_compress");
    *(void **)&zstd_compressBound = dlsym(zs, "ZSTD_compressBound");
    *(void **)&zstd_isError = dlsym(zs, "ZSTD_isError");
  }
  void * l4 = bench_dlopen("liblz4.so.1");
  if (l4) {
    *(void **)&lz4_compressFrame = dlsym(l4, "LZ4F_compressFrame");
    *(void **)&lz4_compressFrameBound = dlsym(l4, "LZ4F_compressFrameBound");
    *(void **)&lz4_isError = dlsym(l4, "LZ4F_isError");
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
    {"deflate-9", "deflate", "deflate.level", 9, 1, 0, 0},
    {"zstd-1", "zstd", "zstd.level", 1, 2, 0, 0},
    {"zstd-3", "zstd", "zstd.level", 3, 2, 0, 0},
    {"zstd-9", "zstd", "zstd.level", 9, 2, 0, 0},
    {"lz4-4M", "lz4", NULL, 0, 3, 0, 7},
    {"lz4-64K", "lz4", NULL, 0, 3, 0, 4},
    {"lz4-64K-linked", "lz4", NULL, 0, 3, 1, 4},
};

#define BENCH_CASE_COUNT (sizeof(k_cases) / sizeof(k_cases[0]))

//
// Ours, round-tripped
//

static size_t bench_ours(gcomp_registry_t * registry, const bench_case_t * c,
    const uint8_t * in, size_t n, const char ** error_out) {
  gcomp_options_t * options = NULL;
  if (gcomp_options_create(&options) != GCOMP_OK) {
    *error_out = "options";
    return 0;
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
  size_t result = 0;

  if (!out || !back) {
    *error_out = "memory";
    goto done;
  }
  if (gcomp_encode_buffer(registry, c->method, options, in, n, out, capacity,
          &out_len) != GCOMP_OK) {
    *error_out = "encode";
    goto done;
  }
  if (gcomp_decode_buffer(registry, c->method, NULL, out, out_len, back, n + 64,
          &back_len) != GCOMP_OK) {
    *error_out = "decode";
    goto done;
  }
  if (back_len != n || (n > 0 && memcmp(back, in, n) != 0)) {
    *error_out = "MISMATCH";
    goto done;
  }
  result = out_len;

done:
  free(out);
  free(back);
  gcomp_options_destroy(options);
  return result;
}

//
// Reference
//

static size_t bench_reference(
    const bench_case_t * c, const uint8_t * in, size_t n) {
  if (c->reference == 1) {
    if (!zlib_compress2 || !zlib_compressBound) {
      return 0;
    }
    unsigned long capacity = zlib_compressBound((unsigned long)n) + 64;
    uint8_t * out = (uint8_t *)malloc(capacity);
    if (!out) {
      return 0;
    }
    unsigned long out_len = capacity;
    int rc = zlib_compress2(out, &out_len, in, (unsigned long)n, c->level);
    free(out);
    // Subtract the zlib container: two header bytes and a four byte Adler-32,
    // so that this is deflate against deflate.
    return (rc == 0 && out_len > 6) ? (size_t)out_len - 6 : 0;
  }
  if (c->reference == 2) {
    if (!zstd_compress || !zstd_compressBound || !zstd_isError) {
      return 0;
    }
    size_t capacity = zstd_compressBound(n);
    uint8_t * out = (uint8_t *)malloc(capacity);
    if (!out) {
      return 0;
    }
    size_t out_len = zstd_compress(out, capacity, in, n, c->level);
    free(out);
    return zstd_isError(out_len) ? 0 : out_len;
  }
  if (c->reference == 3) {
    if (!lz4_compressFrame || !lz4_compressFrameBound || !lz4_isError) {
      return 0;
    }
    bench_lz4_prefs_t prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.frameInfo.blockSizeID = c->lz4_block_id;
    prefs.frameInfo.blockMode = c->lz4_linked ? 0u : 1u;
    size_t capacity = lz4_compressFrameBound(n, NULL);
    uint8_t * out = (uint8_t *)malloc(capacity);
    if (!out) {
      return 0;
    }
    size_t out_len = lz4_compressFrame(out, capacity, in, n, &prefs);
    free(out);
    return lz4_isError(out_len) ? 0 : out_len;
  }
  return 0;
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

static uint8_t * bench_read_file(const char * path, size_t * n_out) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return NULL;
  }
  uint8_t * data = (uint8_t *)malloc((size_t)size);
  if (!data) {
    fclose(f);
    return NULL;
  }
  *n_out = fread(data, 1, (size_t)size, f);
  fclose(f);
  return data;
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
  memset(raw_total, 0, sizeof(raw_total));
  memset(ours_total, 0, sizeof(ours_total));
  memset(ref_total, 0, sizeof(ref_total));

  printf("%-16s %-16s %10s %10s %10s %9s\n", "input", "case", "raw", "ours",
      "reference", "delta");

  int inputs = argc > 1 ? argc - 1 : 3;
  for (int a = 0; a < inputs; a++) {
    const char * name;
    uint8_t * data = NULL;
    size_t n = 0;

    if (argc > 1) {
      name = argv[a + 1];
      const char * slash = strrchr(name, '/');
      if (slash) {
        name = slash + 1;
      }
      data = bench_read_file(argv[a + 1], &n);
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
      free(data);
      continue;
    }

    for (size_t c = 0; c < BENCH_CASE_COUNT; c++) {
      const char * error = NULL;
      size_t ours = bench_ours(registry, &k_cases[c], data, n, &error);
      size_t reference = bench_reference(&k_cases[c], data, n);
      if (ours == 0) {
        printf("%-16s %-16s %10zu %10s\n", name, k_cases[c].label, n,
            error ? error : "FAIL");
        continue;
      }
      if (reference == 0) {
        printf("%-16s %-16s %10zu %10zu %10s\n", name, k_cases[c].label, n,
            ours, "-");
      }
      else {
        printf("%-16s %-16s %10zu %10zu %10zu %+8.2f%%\n", name,
            k_cases[c].label, n, ours, reference,
            100.0 * ((double)ours - (double)reference) / (double)reference);
        ref_total[c] += reference;
      }
      raw_total[c] += n;
      ours_total[c] += ours;
    }
    free(data);
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
  return 0;
}
