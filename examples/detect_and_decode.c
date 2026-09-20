/**
 * @file detect_and_decode.c
 *
 * Read a compressed file without being told what it is.
 *
 * Works through the four introspection calls in the order a real caller needs
 * them: recognise the format, read its header, decide whether to accept it,
 * and only then decompress.
 *
 * The middle step is the one worth copying. A stream that states its
 * decompressed size lets the caller set an exact output ceiling, which is a
 * far better defence against a decompression bomb than a ratio heuristic -
 * and it is the difference between "this looks suspicious" and "this file
 * says it is 900 MB and I will not hold that".
 *
 *   cc detect_and_decode.c -lghoti.io-compress-0
 *   ./detect_and_decode some-file.gz
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/// Our own ceiling on what we are willing to hold, whatever a file claims.
#define MAX_ACCEPTABLE_OUTPUT (256u * 1024u * 1024u)

static unsigned char * read_file(const char * path, size_t * size_out) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);

  unsigned char * buf = malloc((size_t)n ? (size_t)n : 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *size_out = (size_t)n;
  return buf;
}

int main(int argc, char ** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <compressed-file>\n", argv[0]);
    return 2;
  }

  size_t in_len = 0;
  unsigned char * in = read_file(argv[1], &in_len);
  if (!in) {
    return 1;
  }

  // 1. What is it?  Only the formats that carry a magic number can be known;
  //    deflate, LZW and RLE begin with data and are never guessed at.
  const char * method = NULL;
  size_t needed = 0;
  gcomp_status_t s = gcomp_detect(in, in_len, &method, &needed);
  if (s == GCOMP_ERR_UNSUPPORTED) {
    fprintf(stderr, "%s: not a format that identifies itself\n", argv[1]);
    free(in);
    return 1;
  }
  if (s != GCOMP_OK) {
    fprintf(stderr, "%s: %s (needs %zu bytes)\n", argv[1],
        gcomp_status_to_string(s), needed);
    free(in);
    return 1;
  }
  printf("%s: %s\n", argv[1], method);

  // 2. What does its header say?
  gcomp_stream_info_t info;
  s = gcomp_peek(NULL, method, NULL, in, in_len, &info, &needed);
  if (s != GCOMP_OK) {
    fprintf(stderr, "  header: %s\n", gcomp_status_to_string(s));
    free(in);
    return 1;
  }
  printf("  header      %zu bytes\n", info.header_size);
  printf("  window      %" PRIu64 " bytes\n", info.window_size);
  printf("  checksum    %s\n", info.has_checksum ? "yes" : "no");
  if (info.has_dictionary) {
    printf("  dictionary  required, id %u\n", info.dictionary_id);
  }

  gcomp_options_t * opts = NULL;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    free(in);
    return 1;
  }

  // 3. Decide, before decompressing rather than during.
  if (info.has_content_size) {
    printf("  content     %" PRIu64 " bytes (stated)\n", info.content_size);
    if (info.content_size > MAX_ACCEPTABLE_OUTPUT) {
      fprintf(stderr, "  refusing: larger than we are willing to hold\n");
      gcomp_options_destroy(opts);
      free(in);
      return 1;
    }
    // An exact ceiling beats a ratio: the ratio has to guess, and a file with
    // a highly compressible opening trips it on what its first few kilobytes
    // looked like.
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", info.content_size);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0);
  }
  else {
    printf("  content     not stated\n");
    gcomp_options_set_uint64(
        opts, "limits.max_output_bytes", MAX_ACCEPTABLE_OUTPUT);
  }

  // 4. Decompress into a buffer the library sizes.
  void * out = NULL;
  size_t out_len = 0;
  s = gcomp_decode_alloc(NULL, method, opts, in, in_len, &out, &out_len);
  gcomp_options_destroy(opts);
  free(in);

  if (s != GCOMP_OK) {
    fprintf(stderr, "  decode: %s\n", gcomp_status_to_string(s));
    return 1;
  }

  printf("  decoded     %zu bytes\n", out_len);
  gcomp_buffer_free(NULL, out);
  return 0;
}
