/**
 * @file stream_cb.c
 *
 * Implementation of callback-based streaming API for the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Internal buffer size for reading/writing */
#define STREAM_CB_BUFFER_SIZE (64 * 1024) /* 64 KiB */

gcomp_status_t gcomp_encode_stream_cb(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, gcomp_read_cb read_cb,
    void * read_ctx, gcomp_write_cb write_cb, void * write_ctx) {
  /* Validate arguments */
  if (!method_name || !read_cb || !write_cb) {
    return GCOMP_ERR_INVALID_ARG;
  }

  /* Use default registry if none provided */
  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }

  /* Create encoder */
  gcomp_encoder_t * encoder = NULL;
  gcomp_status_t status =
      gcomp_encoder_create(registry, method_name, options, &encoder);
  if (status != GCOMP_OK) {
    return status;
  }

  /* Allocate input and output buffers */
  uint8_t * input_buf = malloc(STREAM_CB_BUFFER_SIZE);
  uint8_t * output_buf = malloc(STREAM_CB_BUFFER_SIZE);
  if (!input_buf || !output_buf) {
    free(input_buf);
    free(output_buf);
    gcomp_encoder_destroy(encoder);
    return GCOMP_ERR_MEMORY;
  }

  /* Main encoding loop */
  bool eof = false;
  size_t input_available = 0;
  size_t input_offset = 0;

  while (1) {
    /* Read more input if needed */
    if (!eof && input_available == 0) {
      size_t read_n = 0;
      status = read_cb(read_ctx, input_buf, STREAM_CB_BUFFER_SIZE, &read_n);
      if (status != GCOMP_OK) {
        /* Propagate error from callback */
        free(input_buf);
        free(output_buf);
        gcomp_encoder_destroy(encoder);
        return status;
      }

      if (read_n == 0) {
        /* EOF */
        eof = true;
      }
      else {
        input_available = read_n;
        input_offset = 0;
      }
    }

    /* Prepare input buffer for encoder */
    gcomp_buffer_t encoder_input = {
        .data = input_buf + input_offset,
        .size = input_available,
        .used = 0,
    };

    /* Prepare output buffer for encoder */
    gcomp_buffer_t encoder_output = {
        .data = output_buf,
        .size = STREAM_CB_BUFFER_SIZE,
        .used = 0,
    };

    /* Update encoder */
    status = gcomp_encoder_update(encoder, &encoder_input, &encoder_output);
    if (status != GCOMP_OK) {
      free(input_buf);
      free(output_buf);
      gcomp_encoder_destroy(encoder);
      return status;
    }

    /* Update input tracking */
    input_offset += encoder_input.used;
    input_available -= encoder_input.used;

    /* Write output if any was produced */
    if (encoder_output.used > 0) {
      size_t write_offset = 0;
      while (write_offset < encoder_output.used) {
        size_t written = 0;
        status = write_cb(write_ctx, output_buf + write_offset,
            encoder_output.used - write_offset, &written);
        if (status != GCOMP_OK) {
          /* Propagate error from callback */
          free(input_buf);
          free(output_buf);
          gcomp_encoder_destroy(encoder);
          return status;
        }

        if (written == 0) {
          /* Write callback returned 0 bytes - this is an error */
          free(input_buf);
          free(output_buf);
          gcomp_encoder_destroy(encoder);
          return GCOMP_ERR_IO;
        }

        write_offset += written;
      }
    }

    /* If we've consumed all input and hit EOF, break to finish */
    if (eof && input_available == 0) {
      break;
    }
  }

  /* Finish encoding */
  while (1) {
    gcomp_buffer_t encoder_output = {
        .data = output_buf,
        .size = STREAM_CB_BUFFER_SIZE,
        .used = 0,
    };

    status = gcomp_encoder_finish(encoder, &encoder_output);
    if (status == GCOMP_OK) {
      /* Finished successfully */
      if (encoder_output.used > 0) {
        size_t write_offset = 0;
        while (write_offset < encoder_output.used) {
          size_t written = 0;
          status = write_cb(write_ctx, output_buf + write_offset,
              encoder_output.used - write_offset, &written);
          if (status != GCOMP_OK) {
            free(input_buf);
            free(output_buf);
            gcomp_encoder_destroy(encoder);
            return status;
          }

          if (written == 0) {
            free(input_buf);
            free(output_buf);
            gcomp_encoder_destroy(encoder);
            return GCOMP_ERR_IO;
          }

          write_offset += written;
        }
      }
      break;
    }
    else if (status == GCOMP_ERR_LIMIT) {
      /* Output buffer too small - write what we have and retry */
      if (encoder_output.used > 0) {
        size_t write_offset = 0;
        while (write_offset < encoder_output.used) {
          size_t written = 0;
          status = write_cb(write_ctx, output_buf + write_offset,
              encoder_output.used - write_offset, &written);
          if (status != GCOMP_OK) {
            free(input_buf);
            free(output_buf);
            gcomp_encoder_destroy(encoder);
            return status;
          }

          if (written == 0) {
            free(input_buf);
            free(output_buf);
            gcomp_encoder_destroy(encoder);
            return GCOMP_ERR_IO;
          }

          write_offset += written;
        }
      }
      /* Continue loop to retry finish */
    }
    else {
      /* Other error */
      free(input_buf);
      free(output_buf);
      gcomp_encoder_destroy(encoder);
      return status;
    }
  }

  /* Clean up */
  free(input_buf);
  free(output_buf);
  gcomp_encoder_destroy(encoder);
  return GCOMP_OK;
}

gcomp_status_t gcomp_decode_stream_cb(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, gcomp_read_cb read_cb,
    void * read_ctx, gcomp_write_cb write_cb, void * write_ctx) {
  /* Validate arguments */
  if (!method_name || !read_cb || !write_cb) {
    return GCOMP_ERR_INVALID_ARG;
  }

  /* Use default registry if none provided */
  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }

  /* Create decoder */
  gcomp_decoder_t * decoder = NULL;
  gcomp_status_t status =
      gcomp_decoder_create(registry, method_name, options, &decoder);
  if (status != GCOMP_OK) {
    return status;
  }

  /* Allocate input and output buffers */
  uint8_t * input_buf = malloc(STREAM_CB_BUFFER_SIZE);
  uint8_t * output_buf = malloc(STREAM_CB_BUFFER_SIZE);
  if (!input_buf || !output_buf) {
    free(input_buf);
    free(output_buf);
    gcomp_decoder_destroy(decoder);
    return GCOMP_ERR_MEMORY;
  }

  /* Main decoding loop */
  bool eof = false;
  size_t input_available = 0;
  size_t input_offset = 0;

  while (1) {
    /* Read more input if needed */
    if (!eof && input_available == 0) {
      size_t read_n = 0;
      status = read_cb(read_ctx, input_buf, STREAM_CB_BUFFER_SIZE, &read_n);
      if (status != GCOMP_OK) {
        /* Propagate error from callback */
        free(input_buf);
        free(output_buf);
        gcomp_decoder_destroy(decoder);
        return status;
      }

      if (read_n == 0) {
        /* EOF */
        eof = true;
      }
      else {
        input_available = read_n;
        input_offset = 0;
      }
    }

    /* Prepare input buffer for decoder */
    gcomp_buffer_t decoder_input = {
        .data = input_buf + input_offset,
        .size = input_available,
        .used = 0,
    };

    /* Prepare output buffer for decoder */
    gcomp_buffer_t decoder_output = {
        .data = output_buf,
        .size = STREAM_CB_BUFFER_SIZE,
        .used = 0,
    };

    /* Update decoder */
    status = gcomp_decoder_update(decoder, &decoder_input, &decoder_output);
    if (status != GCOMP_OK) {
      free(input_buf);
      free(output_buf);
      gcomp_decoder_destroy(decoder);
      return status;
    }

    /* Update input tracking */
    input_offset += decoder_input.used;
    input_available -= decoder_input.used;

    /* Write output if any was produced */
    if (decoder_output.used > 0) {
      size_t write_offset = 0;
      while (write_offset < decoder_output.used) {
        size_t written = 0;
        status = write_cb(write_ctx, output_buf + write_offset,
            decoder_output.used - write_offset, &written);
        if (status != GCOMP_OK) {
          /* Propagate error from callback */
          free(input_buf);
          free(output_buf);
          gcomp_decoder_destroy(decoder);
          return status;
        }

        if (written == 0) {
          /* Write callback returned 0 bytes - this is an error */
          free(input_buf);
          free(output_buf);
          gcomp_decoder_destroy(decoder);
          return GCOMP_ERR_IO;
        }

        write_offset += written;
      }
    }

    /* If we've consumed all input and hit EOF, break to finish */
    if (eof && input_available == 0) {
      break;
    }
  }

  /* Finish decoding */
  while (1) {
    gcomp_buffer_t decoder_output = {
        .data = output_buf,
        .size = STREAM_CB_BUFFER_SIZE,
        .used = 0,
    };

    status = gcomp_decoder_finish(decoder, &decoder_output);
    if (status == GCOMP_OK) {
      /* Finished successfully */
      if (decoder_output.used > 0) {
        size_t write_offset = 0;
        while (write_offset < decoder_output.used) {
          size_t written = 0;
          status = write_cb(write_ctx, output_buf + write_offset,
              decoder_output.used - write_offset, &written);
          if (status != GCOMP_OK) {
            free(input_buf);
            free(output_buf);
            gcomp_decoder_destroy(decoder);
            return status;
          }

          if (written == 0) {
            free(input_buf);
            free(output_buf);
            gcomp_decoder_destroy(decoder);
            return GCOMP_ERR_IO;
          }

          write_offset += written;
        }
      }
      break;
    }
    else if (status == GCOMP_ERR_LIMIT) {
      /* Output buffer too small - write what we have and retry */
      if (decoder_output.used > 0) {
        size_t write_offset = 0;
        while (write_offset < decoder_output.used) {
          size_t written = 0;
          status = write_cb(write_ctx, output_buf + write_offset,
              decoder_output.used - write_offset, &written);
          if (status != GCOMP_OK) {
            free(input_buf);
            free(output_buf);
            gcomp_decoder_destroy(decoder);
            return status;
          }

          if (written == 0) {
            free(input_buf);
            free(output_buf);
            gcomp_decoder_destroy(decoder);
            return GCOMP_ERR_IO;
          }

          write_offset += written;
        }
      }
      /* Continue loop to retry finish */
    }
    else {
      /* Other error */
      free(input_buf);
      free(output_buf);
      gcomp_decoder_destroy(decoder);
      return status;
    }
  }

  /* Clean up */
  free(input_buf);
  free(output_buf);
  gcomp_decoder_destroy(decoder);
  return GCOMP_OK;
}
