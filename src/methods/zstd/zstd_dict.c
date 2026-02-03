/**
 * @file zstd_dict.c
 *
 * Zstd dictionary format parsing (RFC 8878 Section 5).
 *
 * ## Dictionary Formats
 *
 * **Formatted dictionary** (magic 0xEC30A437):
 * - Dictionary_ID (4 bytes LE)
 * - Entropy_Tables: Huffman table for literals, FSE tables for offset, match
 *   length, literals length, plus 3× repeat offsets
 * - Content: raw bytes used as initial window
 *
 * Encoder/decoder can use the entropy tables for the first block (repeat/tree-
 * less mode), reducing header size. Content is always used as initial window.
 *
 * **Raw content**: No magic, >= 8 bytes, content only (dict_id = 0). Encoder
 * can still set zstd.dictionary_id in the frame header for identification.
 * Decoder accepts raw dictionaries for any frame dict_id so external encoders
 * that use content-derived IDs still decode.
 *
 * ## Usage
 *
 * - Encoder: Options provide `zstd.dictionary` (bytes) and optionally
 *   `zstd.dictionary_id`. This module parses the dictionary; the encoder
 *   uses content as initial window and writes dict_id in the frame header.
 *
 * - Decoder: Same options. After parsing the frame header, the decoder
 *   preloads the window with dictionary content (when user provided a dict)
 *   and validates dict_id when the frame specifies one. Formatted dict
 *   entropy tables are used for the first block when present.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
#include <string.h>

#define ZSTD_DICT_MIN_RAW 8
#define FSE_MAX_SYMBOL 255

static void zstd_dict_clear(zstd_dict_parsed_t * out) {
  out->dict_id = 0;
  out->content = NULL;
  out->content_size = 0;
  out->has_entropy_tables = false;
  out->huf_table = NULL;
  out->huf_table_size = 0;
  out->huf_max_bits = 0;
  out->fse_ll_table = NULL;
  out->fse_ml_table = NULL;
  out->fse_of_table = NULL;
  out->fse_ll_size = 0;
  out->fse_ml_size = 0;
  out->fse_of_size = 0;
  out->fse_ll_log = 0;
  out->fse_ml_log = 0;
  out->fse_of_log = 0;
  out->rep_offset_1 = 0;
  out->rep_offset_2 = 0;
  out->rep_offset_3 = 0;
  out->_allocator = NULL;
}

gcomp_status_t zstd_dict_parse(const uint8_t * buf, size_t buf_size,
    const gcomp_allocator_t * alloc, zstd_dict_parsed_t * out) {
  if (!buf || !out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_dict_clear(out);

  if (buf_size < ZSTD_DICT_MIN_RAW) {
    return GCOMP_ERR_CORRUPT;
  }

  uint32_t magic = gcomp_read_le32(buf);
  if (magic != ZSTD_DICT_MAGIC) {
    // Raw content dictionary
    out->content = buf;
    out->content_size = buf_size;
    out->dict_id = 0;
    return GCOMP_OK;
  }

  // Formatted dictionary
  if (buf_size < 8) {
    return GCOMP_ERR_CORRUPT;
  }

  out->dict_id = gcomp_read_le32(buf + 4);
  if (out->dict_id == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  if (!alloc) {
    return GCOMP_ERR_INVALID_ARG;
  }

  out->_allocator = alloc;
  size_t pos = 8;

  // Huffman table for literals
  size_t huf_table_size = (size_t)1 << 11; // max 11 bits
  out->huf_table =
      gcomp_calloc(alloc, huf_table_size, sizeof(zstd_huf_entry_t));
  if (!out->huf_table) {
    return GCOMP_ERR_MEMORY;
  }

  size_t huf_bytes = 0;
  gcomp_status_t status = zstd_huf_read_table(buf + pos, buf_size - pos,
      out->huf_table, huf_table_size, &out->huf_max_bits, &huf_bytes);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, out->huf_table);
    out->huf_table = NULL;
    return status;
  }
  out->huf_table_size = huf_table_size;
  pos += huf_bytes;

  // FSE table for offsets (order per RFC 8878 §5: HUF, OF, ML, LL)
  if (pos >= buf_size) {
    goto cleanup_tables;
  }
  {
    int16_t norm_counts[FSE_MAX_SYMBOL + 1];
    unsigned max_symbol, table_log;
    size_t header_size;
    status = zstd_fse_read_table_header(buf + pos, buf_size - pos, norm_counts,
        &max_symbol, &table_log, &header_size);
    if (status != GCOMP_OK) {
      goto cleanup_tables;
    }
    out->fse_of_size = (size_t)1 << table_log;
    out->fse_of_table =
        gcomp_calloc(alloc, out->fse_of_size, sizeof(zstd_fse_entry_t));
    if (!out->fse_of_table) {
      goto cleanup_tables;
    }
    size_t fse_bytes = 0;
    status = zstd_fse_build_decoding_table(buf + pos, buf_size - pos,
        out->fse_of_table, out->fse_of_size, &out->fse_of_log, NULL,
        &fse_bytes);
    if (status != GCOMP_OK) {
      goto cleanup_tables;
    }
    pos += fse_bytes;
  }

  // FSE table for match lengths
  if (pos >= buf_size) {
    goto cleanup_tables;
  }
  {
    int16_t norm_counts[FSE_MAX_SYMBOL + 1];
    unsigned max_symbol, table_log;
    size_t header_size;
    status = zstd_fse_read_table_header(buf + pos, buf_size - pos, norm_counts,
        &max_symbol, &table_log, &header_size);
    if (status != GCOMP_OK) {
      goto cleanup_tables;
    }
    out->fse_ml_size = (size_t)1 << table_log;
    out->fse_ml_table =
        gcomp_calloc(alloc, out->fse_ml_size, sizeof(zstd_fse_entry_t));
    if (!out->fse_ml_table) {
      goto cleanup_tables;
    }
    size_t fse_bytes = 0;
    status = zstd_fse_build_decoding_table(buf + pos, buf_size - pos,
        out->fse_ml_table, out->fse_ml_size, &out->fse_ml_log, NULL,
        &fse_bytes);
    if (status != GCOMP_OK) {
      goto cleanup_tables;
    }
    pos += fse_bytes;
  }

  // FSE table for literals lengths
  if (pos >= buf_size) {
    goto cleanup_tables;
  }
  {
    int16_t norm_counts[FSE_MAX_SYMBOL + 1];
    unsigned max_symbol, table_log;
    size_t header_size;
    status = zstd_fse_read_table_header(buf + pos, buf_size - pos, norm_counts,
        &max_symbol, &table_log, &header_size);
    if (status != GCOMP_OK) {
      goto cleanup_tables;
    }
    out->fse_ll_size = (size_t)1 << table_log;
    out->fse_ll_table =
        gcomp_calloc(alloc, out->fse_ll_size, sizeof(zstd_fse_entry_t));
    if (!out->fse_ll_table) {
      goto cleanup_tables;
    }
    size_t fse_bytes = 0;
    status = zstd_fse_build_decoding_table(buf + pos, buf_size - pos,
        out->fse_ll_table, out->fse_ll_size, &out->fse_ll_log, NULL,
        &fse_bytes);
    if (status != GCOMP_OK) {
      goto cleanup_tables;
    }
    pos += fse_bytes;
  }

  // 3 × 4-byte repeat offsets
  if (pos + 12 > buf_size) {
    goto cleanup_tables;
  }
  out->rep_offset_1 = gcomp_read_le32(buf + pos);
  out->rep_offset_2 = gcomp_read_le32(buf + pos + 4);
  out->rep_offset_3 = gcomp_read_le32(buf + pos + 8);
  pos += 12;

  out->content = buf + pos;
  out->content_size = buf_size - pos;
  out->has_entropy_tables = true;

  // Per RFC: each repeat offset must be < dictionary content size
  if (out->rep_offset_1 >= out->content_size ||
      out->rep_offset_2 >= out->content_size ||
      out->rep_offset_3 >= out->content_size) {
    goto cleanup_tables;
  }

  return GCOMP_OK;

cleanup_tables:
  if (out->fse_ll_table) {
    gcomp_free(alloc, out->fse_ll_table);
    out->fse_ll_table = NULL;
  }
  if (out->fse_ml_table) {
    gcomp_free(alloc, out->fse_ml_table);
    out->fse_ml_table = NULL;
  }
  if (out->fse_of_table) {
    gcomp_free(alloc, out->fse_of_table);
    out->fse_of_table = NULL;
  }
  if (out->huf_table) {
    gcomp_free(alloc, out->huf_table);
    out->huf_table = NULL;
  }
  return GCOMP_ERR_CORRUPT;
}

void zstd_dict_destroy(zstd_dict_parsed_t * parsed) {
  if (!parsed) {
    return;
  }
  const gcomp_allocator_t * alloc = parsed->_allocator;
  if (alloc) {
    if (parsed->huf_table) {
      gcomp_free(alloc, parsed->huf_table);
      parsed->huf_table = NULL;
    }
    if (parsed->fse_ll_table) {
      gcomp_free(alloc, parsed->fse_ll_table);
      parsed->fse_ll_table = NULL;
    }
    if (parsed->fse_ml_table) {
      gcomp_free(alloc, parsed->fse_ml_table);
      parsed->fse_ml_table = NULL;
    }
    if (parsed->fse_of_table) {
      gcomp_free(alloc, parsed->fse_of_table);
      parsed->fse_of_table = NULL;
    }
  }
  zstd_dict_clear(parsed);
}
