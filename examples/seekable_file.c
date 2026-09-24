/**
 * @file seekable_file.c
 *
 * Reading the middle of a compressed file that is never held in memory.
 *
 * The companion to seekable_read.c, which opens a file it has already got in a
 * buffer. This one hands the library a callback instead, so the only bytes that
 * are ever read are the seek table and the frames a read actually overlaps -
 * which is what makes the API usable on a file larger than memory, or one that
 * is not in memory at all: an object store, a decrypting layer, an mmap of
 * something enormous.
 *
 * The callback here is `fseek` plus `fread` and nothing else; it is thirteen
 * lines. It counts the bytes it hands over so the program can print what the
 * read cost, because "it did not read the file" is the whole claim and a
 * demonstration that does not measure it is not showing anything.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/seekable.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// What the callback is handed, and what it records.
typedef struct {
  FILE * fp;
  uint64_t bytes_read; ///< Everything the library has asked for so far.
  unsigned long calls;
} file_source_t;

/**
 * @brief A gcomp_seek_cb over a stdio file.
 *
 * Reads are not sequential: opening goes straight to the footer at the end of
 * the file, and every read after that jumps to a frame. So this seeks on every
 * call rather than assuming anything about where the last one left off.
 *
 * A short read is not an error here - the library asks again for the rest, the
 * way it would of read(2) - so end of file is reported as zero bytes and
 * GCOMP_OK, and only a real failure returns GCOMP_ERR_IO.
 */
static gcomp_status_t file_read(
    void * ctx, uint64_t offset, void * dst, size_t len, size_t * read_out) {
  file_source_t * src = (file_source_t *)ctx;
  *read_out = 0;
  if (fseek(src->fp, (long)offset, SEEK_SET) != 0) {
    return GCOMP_ERR_IO;
  }
  const size_t got = fread(dst, 1u, len, src->fp);
  if (got == 0u && ferror(src->fp)) {
    return GCOMP_ERR_IO;
  }
  src->bytes_read += (uint64_t)got;
  src->calls++;
  *read_out = got;
  return GCOMP_OK;
}

int main(void) {
  const char * path = "seekable_file_example.zst";

  // Something with structure, so the ratio and the costs below mean something.
  const size_t n = 8u * 1024u * 1024u;
  unsigned char * data = malloc(n);
  if (!data) {
    return 1;
  }
  for (size_t i = 0; i < n; i++) {
    data[i] = (unsigned char)('a' + ((i / 64u + i / 4096u) % 26u));
  }

  gcomp_options_t * options = NULL;
  if (gcomp_options_create(&options) != GCOMP_OK) {
    free(data);
    return 1;
  }
  // Smaller frames mean finer seeking and slightly worse compression, because
  // each frame starts with no history. This is the knob that trade lives on.
  gcomp_options_set_uint64(options, "zstd.seekable_frame_size", 256u * 1024u);

  // Write the file out. A real program would stream this; the writing side is
  // seekable_read.c's subject, not this one's.
  size_t bound = 0;
  if (gcomp_seekable_write_bound(NULL, "zstd", options, n, &bound) !=
      GCOMP_OK) {
    fprintf(stderr, "could not size the output\n");
    gcomp_options_destroy(options);
    free(data);
    return 1;
  }
  unsigned char * file = malloc(bound);
  if (!file) {
    gcomp_options_destroy(options);
    free(data);
    return 1;
  }
  size_t file_size = 0;
  if (gcomp_seekable_write_buffer(NULL, "zstd", options, data, n, file, bound,
          &file_size) != GCOMP_OK) {
    fprintf(stderr, "could not write the seekable file\n");
    free(file);
    gcomp_options_destroy(options);
    free(data);
    return 1;
  }
  {
    FILE * w = fopen(path, "wb");
    if (!w || fwrite(file, 1u, file_size, w) != file_size ||
        fclose(w) != 0) {
      fprintf(stderr, "could not write %s\n", path);
      free(file);
      gcomp_options_destroy(options);
      free(data);
      return 1;
    }
  }
  free(file); // From here on the file exists only on disk.

  printf("wrote %s: %zu bytes from %zu (%.1f%%)\n", path, file_size, n,
      100.0 * (double)file_size / (double)n);

  // Open it through the callback. Nothing but the seek table is read.
  file_source_t src;
  memset(&src, 0, sizeof(src));
  src.fp = fopen(path, "rb");
  if (!src.fp) {
    fprintf(stderr, "could not open %s\n", path);
    gcomp_options_destroy(options);
    free(data);
    return 1;
  }

  gcomp_seekable_t * s = NULL;
  const gcomp_status_t st = gcomp_seekable_open_cb(
      NULL, "zstd", NULL, file_read, &src, (uint64_t)file_size, &s);
  if (st != GCOMP_OK) {
    fprintf(stderr, "could not open the stream: %s\n",
        gcomp_status_to_string(st));
    fclose(src.fp);
    gcomp_options_destroy(options);
    free(data);
    return 1;
  }

  printf("\nopened: %llu decompressed bytes in %zu frames, seek table %s\n",
      (unsigned long long)gcomp_seekable_size(s), gcomp_seekable_frame_count(s),
      gcomp_seekable_has_table(s) ? "present" : "absent (indexed by walking)");
  printf("  opening read %llu bytes in %lu calls, of a %zu-byte file\n",
      (unsigned long long)src.bytes_read, src.calls, file_size);

  // Read a window out of the middle.
  const uint64_t at = (uint64_t)n / 2u;
  const size_t want = 64u;
  unsigned char got[64];
  const uint64_t before = src.bytes_read;

  size_t produced = 0;
  if (gcomp_seekable_read(s, at, got, want, &produced) != GCOMP_OK) {
    fprintf(stderr, "read failed\n");
    gcomp_seekable_close(s);
    fclose(src.fp);
    gcomp_options_destroy(options);
    free(data);
    return 1;
  }

  printf("\nread %zu bytes at offset %llu\n", produced,
      (unsigned long long)at);
  printf("  correct: %s\n",
      (produced == want && memcmp(got, data + at, want) == 0) ? "yes" : "NO");
  printf("  fetched %llu more bytes to answer it (%.2f%% of the file)\n",
      (unsigned long long)(src.bytes_read - before),
      100.0 * (double)(src.bytes_read - before) / (double)file_size);

  // A second read inside the same frame costs nothing: the decoded frame is
  // still there. This is why a sequential scan through a seekable file is not
  // one fetch per read.
  const uint64_t cached_before = src.bytes_read;
  if (gcomp_seekable_read(s, at + 64u, got, want, &produced) != GCOMP_OK) {
    fprintf(stderr, "second read failed\n");
    gcomp_seekable_close(s);
    fclose(src.fp);
    gcomp_options_destroy(options);
    free(data);
    return 1;
  }
  printf("\na second read in the same frame fetched %llu bytes\n",
      (unsigned long long)(src.bytes_read - cached_before));

  // The stream does not own the source: close it, then close the file.
  gcomp_seekable_close(s);
  fclose(src.fp);
  remove(path);

  gcomp_options_destroy(options);
  free(data);
  return 0;
}
