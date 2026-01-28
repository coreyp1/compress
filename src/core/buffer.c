/**
 * @file buffer.c
 *
 * Implementation of buffer-to-buffer convenience wrappers for the Ghoti.io
 * Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <string.h>

gcomp_status_t gcomp_encode_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output_data,
    size_t output_capacity, size_t * output_size_out) {
  /* Validate arguments */
  if (!method_name || !output_data || !output_size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  /* Allow NULL input_data only if input_size is 0 */
  if (!input_data && input_size > 0) {
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

  /* Initialize output size */
  *output_size_out = 0;

  /* Process input data */
  const uint8_t * input_ptr = input_data ? (const uint8_t *)input_data : NULL;
  size_t input_remaining = input_size;
  uint8_t * output_ptr = (uint8_t *)output_data;
  size_t output_remaining = output_capacity;

  /* Process input data */
  while (input_remaining > 0) {
    /* Prepare input buffer */
    gcomp_buffer_t input_buf = {
        .data = input_ptr,
        .size = input_remaining,
        .used = 0,
    };

    /* Prepare output buffer */
    gcomp_buffer_t output_buf = {
        .data = output_ptr,
        .size = output_remaining,
        .used = 0,
    };

    /* Update encoder */
    status = gcomp_encoder_update(encoder, &input_buf, &output_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return status;
    }

    /* Update pointers and counters */
    if (input_ptr != NULL) {
      input_ptr += input_buf.used;
    }
    input_remaining -= input_buf.used;
    output_ptr += output_buf.used;
    output_remaining -= output_buf.used;
    *output_size_out += output_buf.used;

    /* Check if output buffer is full and we still have input */
    if (output_remaining == 0 && input_remaining > 0) {
      gcomp_encoder_destroy(encoder);
      return GCOMP_ERR_LIMIT;
    }

    /* If no input consumed and no output produced, we're done with input */
    if (input_buf.used == 0 && output_buf.used == 0) {
      break;
    }
  }

  /* Finish encoding */
  while (1) {
    gcomp_buffer_t output_buf = {
        .data = output_ptr,
        .size = output_remaining,
        .used = 0,
    };

    status = gcomp_encoder_finish(encoder, &output_buf);
    if (status == GCOMP_OK) {
      /* Finished successfully */
      output_ptr += output_buf.used;
      output_remaining -= output_buf.used;
      *output_size_out += output_buf.used;
      break;
    }
    else if (status == GCOMP_ERR_LIMIT) {
      /* Output buffer too small for finish */
      gcomp_encoder_destroy(encoder);
      return GCOMP_ERR_LIMIT;
    }
    else {
      /* Other error */
      gcomp_encoder_destroy(encoder);
      return status;
    }
  }

  /* Clean up */
  gcomp_encoder_destroy(encoder);
  return GCOMP_OK;
}

gcomp_status_t gcomp_decode_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output_data,
    size_t output_capacity, size_t * output_size_out) {
  /* Validate arguments */
  if (!method_name || !output_data || !output_size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  /* Allow NULL input_data only if input_size is 0 */
  if (!input_data && input_size > 0) {
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

  /* Initialize output size */
  *output_size_out = 0;

  /* Process input data */
  const uint8_t * input_ptr = input_data ? (const uint8_t *)input_data : NULL;
  size_t input_remaining = input_size;
  uint8_t * output_ptr = (uint8_t *)output_data;
  size_t output_remaining = output_capacity;

  /* Process input data */
  while (input_remaining > 0) {
    /* Prepare input buffer */
    gcomp_buffer_t input_buf = {
        .data = input_ptr,
        .size = input_remaining,
        .used = 0,
    };

    /* Prepare output buffer */
    gcomp_buffer_t output_buf = {
        .data = output_ptr,
        .size = output_remaining,
        .used = 0,
    };

    /* Update decoder */
    status = gcomp_decoder_update(decoder, &input_buf, &output_buf);
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return status;
    }

    /* Update pointers and counters */
    if (input_ptr != NULL) {
      input_ptr += input_buf.used;
    }
    input_remaining -= input_buf.used;
    output_ptr += output_buf.used;
    output_remaining -= output_buf.used;
    *output_size_out += output_buf.used;

    /* Check if output buffer is full and we still have input */
    if (output_remaining == 0 && input_remaining > 0) {
      gcomp_decoder_destroy(decoder);
      return GCOMP_ERR_LIMIT;
    }

    /* If no input consumed and no output produced, we're done with input */
    if (input_buf.used == 0 && output_buf.used == 0) {
      break;
    }
  }

  /* Finish decoding */
  while (1) {
    gcomp_buffer_t output_buf = {
        .data = output_ptr,
        .size = output_remaining,
        .used = 0,
    };

    status = gcomp_decoder_finish(decoder, &output_buf);
    if (status == GCOMP_OK) {
      /* Finished successfully */
      output_ptr += output_buf.used;
      output_remaining -= output_buf.used;
      *output_size_out += output_buf.used;
      break;
    }
    else if (status == GCOMP_ERR_LIMIT) {
      /* Output buffer too small for finish */
      gcomp_decoder_destroy(decoder);
      return GCOMP_ERR_LIMIT;
    }
    else {
      /* Other error */
      gcomp_decoder_destroy(decoder);
      return status;
    }
  }

  /* Clean up */
  gcomp_decoder_destroy(decoder);
  return GCOMP_OK;
}
