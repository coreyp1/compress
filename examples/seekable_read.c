/**
 * @file seekable_read.c
 *
 * Reading the middle of a compressed file without decoding the front of it.
 *
 * Writes a seekable Zstandard file, then reads a window out of the middle and
 * shows what it cost: the file is opened by reading its seek table, and the
 * read touches only the frames the window overlaps.
 *
 * The same file is then decompressed the ordinary way, because that is the
 * property worth demonstrating - a seekable file is a plain Zstandard stream
 * to anything that does not know about seeking. `zstd -d` reads it too.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/seekable.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  // Something with structure, so the numbers below mean something.
  const size_t n = 4u * 1024u * 1024u;
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
  // Smaller frames mean finer seeking and slightly worse compression: each
  // frame starts with no history. This is the knob that trade lives on.
  gcomp_options_set_uint64(options, "zstd.seekable_frame_size", 256u * 1024u);

  size_t bound = 0;
  if (gcomp_seekable_write_bound(NULL, "zstd", options, n, &bound) !=
      GCOMP_OK) {
    fprintf(stderr, "could not size the output\n");
    return 1;
  }
  unsigned char * file = malloc(bound);
  size_t file_size = 0;
  if (!file || gcomp_seekable_write_buffer(NULL, "zstd", options, data, n, file,
                   bound, &file_size) != GCOMP_OK) {
    fprintf(stderr, "could not write the file\n");
    return 1;
  }
  gcomp_options_destroy(options);

  printf("%zu bytes compressed to %zu (%.1f%%)\n", n, file_size,
      100.0 * (double)file_size / (double)n);

  gcomp_seekable_t * seekable = NULL;
  if (gcomp_seekable_open_buffer(NULL, "zstd", NULL, file, file_size,
          &seekable) != GCOMP_OK) {
    fprintf(stderr, "could not open the file for seeking\n");
    return 1;
  }
  printf("opened: %zu frames, %llu bytes of content, seek table %s\n",
      gcomp_seekable_frame_count(seekable),
      (unsigned long long)gcomp_seekable_size(seekable),
      gcomp_seekable_has_table(seekable) ? "present" : "rebuilt by walking");

  // A window from three quarters of the way in.
  const unsigned long long at = (unsigned long long)(n / 4u * 3u);
  unsigned char window[64];
  size_t got = 0;
  if (gcomp_seekable_read(seekable, at, window, sizeof(window), &got) !=
      GCOMP_OK) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  printf("read %zu bytes at offset %llu: %.*s\n", got, at, (int)got, window);
  printf("they %s the original\n",
      memcmp(window, data + at, got) == 0 ? "match" : "DO NOT match");

  gcomp_seekable_close(seekable);

  // And the same bytes are an ordinary Zstandard stream.
  gcomp_options_t * plain = NULL;
  gcomp_options_create(&plain);
  gcomp_options_set_uint64(plain, "limits.max_expansion_ratio", 0);
  unsigned char * back = malloc(n);
  size_t produced = 0;
  const gcomp_status_t s = gcomp_decode_buffer(
      NULL, "zstd", plain, file, file_size, back, n, &produced);
  printf("decoded as a plain stream: %s, %zu bytes\n",
      s == GCOMP_OK ? "yes" : "no", produced);
  gcomp_options_destroy(plain);

  free(back);
  free(file);
  free(data);
  return 0;
}
