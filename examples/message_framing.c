/**
 * @file message_framing.c
 *
 * Example: compressing a stream of discrete messages with
 * gcomp_encoder_flush().
 *
 * This is the case gcomp_encoder_finish() cannot serve.  A protocol, a log
 * shipper, a chat connection -- anything where the receiver has to act on
 * message N before the sender has message N+1 to give it.  Without a flush the
 * encoder is entitled to sit on a message indefinitely, waiting for enough
 * data to fill a block, and the peer waits with it.
 *
 * What a flush guarantees is exactly one thing: once it returns GCOMP_OK, a
 * decoder given the bytes produced so far will produce every byte the encoder
 * has consumed so far.  This example checks that after every message, rather
 * than asserting it -- the decoder is fed the partial stream and its output
 * compared against what was sent.
 *
 * It also shows the cost, because a flush is not free: the same messages are
 * compressed a second time with no flushing at all, and the two sizes printed
 * side by side.  Flush at message boundaries, not per write.
 *
 * Build: See Makefile target "examples"
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A conversation, as a protocol might carry it.
static const char * MESSAGES[] = {
    "{\"op\":\"hello\",\"client\":\"example\",\"version\":1}",
    "{\"op\":\"subscribe\",\"topic\":\"sensors/temperature\"}",
    "{\"op\":\"event\",\"topic\":\"sensors/temperature\",\"value\":21.5}",
    "{\"op\":\"event\",\"topic\":\"sensors/temperature\",\"value\":21.6}",
    "{\"op\":\"event\",\"topic\":\"sensors/temperature\",\"value\":21.4}",
    "{\"op\":\"unsubscribe\",\"topic\":\"sensors/temperature\"}",
    "{\"op\":\"goodbye\"}",
};
static const size_t MESSAGE_COUNT = sizeof(MESSAGES) / sizeof(MESSAGES[0]);

#define STREAM_CAPACITY 16384
#define CHUNK_SIZE 256

/// Everything the "sender" has put on the wire so far.
typedef struct {
  uint8_t bytes[STREAM_CAPACITY];
  size_t len;
} wire_t;

static int wire_append(wire_t * wire, const uint8_t * data, size_t len) {
  if (wire->len + len > sizeof(wire->bytes)) {
    fprintf(stderr, "wire buffer too small\n");
    return 0;
  }
  memcpy(wire->bytes + wire->len, data, len);
  wire->len += len;
  return 1;
}

/// Hand `len` bytes to the encoder, collecting whatever comes out.
static int send_bytes(gcomp_encoder_t * encoder, const uint8_t * data,
    size_t len, wire_t * wire) {
  uint8_t chunk[CHUNK_SIZE];
  gcomp_buffer_t in_buf = {(void *)data, len, 0};

  // update() is free to take only part of the input when the output buffer
  // fills, so keep offering the rest until it has all been taken.
  while (in_buf.used < in_buf.size) {
    gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
    if (gcomp_encoder_update(encoder, &in_buf, &out_buf) != GCOMP_OK) {
      fprintf(stderr, "encoder update failed\n");
      return 0;
    }
    if (!wire_append(wire, chunk, out_buf.used)) {
      return 0;
    }
  }
  return 1;
}

/**
 * Flush, so the peer can read everything sent so far.
 *
 * The GCOMP_ERR_LIMIT loop is the same contract finish() uses: it means "I
 * have more for you, drain and ask again", not that anything went wrong.
 * Stopping at the first call would leave the peer short.
 */
static int flush_stream(
    gcomp_encoder_t * encoder, gcomp_flush_t mode, wire_t * wire) {
  uint8_t chunk[CHUNK_SIZE];
  for (;;) {
    gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
    gcomp_status_t status = gcomp_encoder_flush(encoder, &out_buf, mode);
    if (!wire_append(wire, chunk, out_buf.used)) {
      return 0;
    }
    if (status == GCOMP_OK) {
      return 1;
    }
    if (status != GCOMP_ERR_LIMIT) {
      fprintf(stderr, "encoder flush failed: %s\n",
          gcomp_encoder_get_error_detail(encoder));
      return 0;
    }
  }
}

static int finish_stream(gcomp_encoder_t * encoder, wire_t * wire) {
  uint8_t chunk[CHUNK_SIZE];
  for (;;) {
    gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
    gcomp_status_t status = gcomp_encoder_finish(encoder, &out_buf);
    if (!wire_append(wire, chunk, out_buf.used)) {
      return 0;
    }
    if (status == GCOMP_OK) {
      return 1;
    }
    if (status != GCOMP_ERR_LIMIT) {
      fprintf(stderr, "encoder finish failed\n");
      return 0;
    }
  }
}

/**
 * The receiver's side: decode the stream as far as it goes.
 *
 * finish() is deliberately not called -- the stream is not over.  What this
 * returns is what a peer could have acted on by now.
 */
static size_t peer_reads(gcomp_registry_t * registry, const wire_t * wire,
    uint8_t * out, size_t out_cap) {
  gcomp_decoder_t * decoder = NULL;
  if (gcomp_decoder_create(registry, "deflate", NULL, &decoder) != GCOMP_OK) {
    return 0;
  }
  gcomp_buffer_t in_buf = {(void *)wire->bytes, wire->len, 0};
  gcomp_buffer_t out_buf = {out, out_cap, 0};
  if (gcomp_decoder_update(decoder, &in_buf, &out_buf) != GCOMP_OK) {
    gcomp_decoder_destroy(decoder);
    return 0;
  }
  gcomp_decoder_destroy(decoder);
  return out_buf.used;
}

/// Compress every message with no flushing, for the size comparison.
static size_t size_without_flushing(gcomp_registry_t * registry) {
  gcomp_encoder_t * encoder = NULL;
  if (gcomp_encoder_create(registry, "deflate", NULL, &encoder) != GCOMP_OK) {
    return 0;
  }
  wire_t wire;
  wire.len = 0;
  for (size_t i = 0; i < MESSAGE_COUNT; i++) {
    if (!send_bytes(encoder, (const uint8_t *)MESSAGES[i],
            strlen(MESSAGES[i]), &wire)) {
      gcomp_encoder_destroy(encoder);
      return 0;
    }
  }
  if (!finish_stream(encoder, &wire)) {
    gcomp_encoder_destroy(encoder);
    return 0;
  }
  gcomp_encoder_destroy(encoder);
  return wire.len;
}

int main(void) {
  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "no default registry\n");
    return 1;
  }

  gcomp_encoder_t * encoder = NULL;
  if (gcomp_encoder_create(registry, "deflate", NULL, &encoder) != GCOMP_OK) {
    fprintf(stderr, "failed to create encoder\n");
    return 1;
  }

  wire_t wire;
  wire.len = 0;

  // What the sender has handed over, so the peer's output can be checked
  // against it after every flush.
  char sent[STREAM_CAPACITY];
  size_t sent_len = 0;

  uint8_t received[STREAM_CAPACITY];

  printf("Sending %zu messages, flushing after each one.\n\n",
      MESSAGE_COUNT);
  printf("%-4s %-8s %-10s %-10s %s\n", "msg", "bytes", "wire", "peer sees",
      "status");
  printf("%-4s %-8s %-10s %-10s %s\n", "---", "-----", "----", "---------",
      "------");

  for (size_t i = 0; i < MESSAGE_COUNT; i++) {
    const char * message = MESSAGES[i];
    size_t message_len = strlen(message);

    if (!send_bytes(encoder, (const uint8_t *)message, message_len, &wire)) {
      gcomp_encoder_destroy(encoder);
      return 1;
    }
    memcpy(sent + sent_len, message, message_len);
    sent_len += message_len;

    // Without this the peer may see nothing at all until the stream ends.
    if (!flush_stream(encoder, GCOMP_FLUSH_SYNC, &wire)) {
      gcomp_encoder_destroy(encoder);
      return 1;
    }

    size_t seen = peer_reads(registry, &wire, received, sizeof(received));
    int ok = (seen == sent_len) && memcmp(received, sent, sent_len) == 0;

    printf("%-4zu %-8zu %-10zu %-10zu %s\n", i + 1, message_len, wire.len,
        seen, ok ? "ok" : "MISMATCH");

    if (!ok) {
      fprintf(stderr,
          "\nthe peer could not read everything that was sent -- this is the "
          "guarantee gcomp_encoder_flush() exists to make\n");
      gcomp_encoder_destroy(encoder);
      return 1;
    }
  }

  if (!finish_stream(encoder, &wire)) {
    gcomp_encoder_destroy(encoder);
    return 1;
  }
  gcomp_encoder_destroy(encoder);

  size_t unflushed = size_without_flushing(registry);

  printf("\nEvery message was readable by the peer as soon as it was sent.\n");
  printf("\nWhat that cost:\n");
  printf("  %zu bytes of messages\n", sent_len);
  printf("  %zu bytes on the wire, flushing after each message\n", wire.len);
  if (unflushed > 0) {
    printf("  %zu bytes on the wire, not flushing at all\n", unflushed);
    printf("\nFlushing ends a block early and pads to a byte boundary, so it\n"
           "costs ratio.  Flush at message boundaries, not per write.\n");
  }

  return 0;
}
