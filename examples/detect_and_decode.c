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
 *   cc detect_and_decode.c -lghoti.io-compress-0 -lghoti.io-cutil-0
 *   ./detect_and_decode some-file.gz
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/cutil/file.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/// Our own ceiling on what we are willing to hold, whatever a file claims.
#define MAX_ACCEPTABLE_OUTPUT (256u * 1024u * 1024u)

/// And one on the compressed file itself.  gcu_file_read() treats this as a
/// promise rather than a truncation: a larger file yields
/// GCU_FILE_ERR_LIMIT and nothing is allocated at all.
#define MAX_ACCEPTABLE_INPUT (64u * 1024u * 1024u)

int main(int argc, char ** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <compressed-file>\n", argv[0]);
    return 2;
  }

  // cutil reads in chunks rather than sizing the file first, so this works
  // on the things fseek/ftell quietly return nothing for - a pipe, a
  // character device, anything under /proc.
  void * in = NULL;
  size_t in_len = 0;
  GCU_File_Result fr =
      gcu_file_read(argv[1], MAX_ACCEPTABLE_INPUT, NULL, &in, &in_len);
  if (fr != GCU_FILE_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gcu_file_result_string(fr));
    return 1;
  }

  // 1. What is it?  Only the formats that carry a magic number can be known;
  //    deflate, LZW and RLE begin with data and are never guessed at.
  const char * method = NULL;
  size_t needed = 0;
  gcomp_status_t s = gcomp_detect(in, in_len, &method, &needed);
  if (s == GCOMP_ERR_UNSUPPORTED) {
    fprintf(stderr, "%s: not a format that identifies itself\n", argv[1]);
    gcu_file_free(NULL, in);
    return 1;
  }
  if (s != GCOMP_OK) {
    fprintf(stderr, "%s: %s (needs %zu bytes)\n", argv[1],
        gcomp_status_to_string(s), needed);
    gcu_file_free(NULL, in);
    return 1;
  }
  printf("%s: %s\n", argv[1], method);

  // 2. What does its header say?
  gcomp_stream_info_t info;
  s = gcomp_peek(NULL, method, NULL, in, in_len, &info, &needed);
  if (s != GCOMP_OK) {
    fprintf(stderr, "  header: %s\n", gcomp_status_to_string(s));
    gcu_file_free(NULL, in);
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
    gcu_file_free(NULL, in);
    return 1;
  }

  // 3. Decide, before decompressing rather than during.
  if (info.has_content_size) {
    printf("  content     %" PRIu64 " bytes (stated)\n", info.content_size);
    if (info.content_size > MAX_ACCEPTABLE_OUTPUT) {
      fprintf(stderr, "  refusing: larger than we are willing to hold\n");
      gcomp_options_destroy(opts);
      gcu_file_free(NULL, in);
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
  gcu_file_free(NULL, in);

  if (s != GCOMP_OK) {
    fprintf(stderr, "  decode: %s\n", gcomp_status_to_string(s));
    return 1;
  }

  printf("  decoded     %zu bytes\n", out_len);
  gcomp_buffer_free(NULL, out);
  return 0;
}
