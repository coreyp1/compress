# Decode Codec: How Decoding Works in Ghoti.io Compress

This document describes how decompression (decoding) works in the compress library: the generic stream-based decode path and the deflate-specific building blocks and standards.

---

## 1. Generic decode path (stream API)

Decoding is method-agnostic at the API level. The same flow is used whether the method is passthru, deflate, or a future codec.

### 1.1 Public API

- **Create:** `gcomp_decoder_create(registry, method_name, options, &decoder_out)`
  Looks up the method by name in the registry and calls the method’s `create_decoder` hook. The method may use the pre-allocated `gcomp_decoder_t` or replace it with its own (e.g. a larger struct with method-specific state).

- **Update:** `gcomp_decoder_update(decoder, input, output)`
  Pushes compressed bytes from `input` and writes decompressed bytes into `output`. The core (`stream.c`) only validates arguments and then calls `decoder->update_fn(decoder, input, output)`. All decode logic lives in the method.

- **Finish:** `gcomp_decoder_finish(decoder, output)`
  Tells the decoder that no more input will be provided. The method flushes any internal state, validates trailers if the format has them, and may write final output. After this, the decoder should not be used for further updates.

- **Destroy:** `gcomp_decoder_destroy(decoder)`
  Calls the method’s `destroy_decoder` (if present), then frees the decoder object with the registry’s allocator.

### 1.2 Buffer semantics

- **Input buffer:** `data` + `size` + `used`. The decoder reads from `data` starting at offset `used`; “available” input is `size - used`. The method must advance `input->used` by the number of bytes consumed.

- **Output buffer:** Same layout. The decoder writes at offset `used`; “available” space is `size - used`. The method must advance `output->used` by the number of bytes produced.

- **Chunking:** The caller may call `update` many times with small input or output. The method is responsible for buffering internally if the format requires it (e.g. reading a block header before producing output).

### 1.3 Decoder structure (internal)

`stream_internal.h` defines the decoder type used by methods:

```c
struct gcomp_decoder_s {
  const gcomp_method_t * method;
  gcomp_registry_t * registry;
  gcomp_options_t * options;
  void * method_state;   // Method-specific decoder state
  gcomp_decoder_update_fn_t update_fn;
  gcomp_decoder_finish_fn_t finish_fn;
};
```

Only the core and method implementations include `stream_internal.h`. Method code assigns `update_fn` and `finish_fn` in `create_decoder` and may store state in `method_state` or in an extended decoder struct.

### 1.4 Callback API

`gcomp_decode_stream_cb(registry, method_name, options, read_cb, read_ctx, write_cb, write_ctx)` is a convenience wrapper: it creates a decoder, then repeatedly reads input via `read_cb`, calls `update` (and finally `finish`) with that input, and passes output to `write_cb`. The same method `update_fn` / `finish_fn` are used; only the driver (buffer-based vs callback-based) changes.

---

## 2. Deflate decode path and standards

The deflate method implements **raw DEFLATE** as specified by **RFC 1951** (no gzip wrapper). Gzip (RFC 1952) is a separate format that wraps DEFLATE with a header and trailer; this library’s deflate method is “deflate only.”

### 2.1 Standards

| Document   | Role |
|-----------|------|
| **RFC 1951** | DEFLATE Compressed Data Format. Defines the bit stream: block types (stored, fixed Huffman, dynamic Huffman), LZ77 back-references (length/distance), Huffman code construction from code lengths, and LSB-first bit order. |
| **RFC 1952** | GZIP file format. Wrapper around DEFLATE (optional); not implemented by the deflate method. |

The deflate decoder must:

- Read bits in **LSB-first** order (lowest bit first within a byte).
- Interpret the stream as a sequence of **blocks**, each with a 3-bit header (final block flag + block type).
- Support **block type 0** (stored): length and one’s-complement length, then raw bytes.
- Support **block type 1** (fixed Huffman): pre-defined literal/length and distance trees per RFC 1951.
- Support **block type 2** (dynamic Huffman): code lengths for literal/length and distance trees, then canonical Huffman decode.
- Decode **LZ77**: literal bytes and (length, distance) back-references into a 32 KiB (or smaller) sliding window.

### 2.2 Current deflate decode components

| Component | Location | Purpose |
|----------|----------|---------|
| **Bit reader** | `src/methods/deflate/bitreader.c`, `.h` | Reads 1–24 bits from a byte buffer in LSB-first order. Used by the decoder to read block headers, code lengths, and Huffman-coded symbols. Handles byte alignment and EOF (returns `GCOMP_ERR_CORRUPT` when not enough bits remain). |
| **Method registration** | `src/methods/deflate/deflate_register.c` | Registers the deflate method and provides stub `update_fn` / `finish_fn` that return `GCOMP_ERR_UNSUPPORTED`. Full decoder logic is not yet implemented (see tasks T3.4–T3.5). |

### 2.3 Planned deflate decode components (from task list)

- **Huffman table builder (T3.4):** Build decode tables from code lengths per RFC 1951; support canonical codes; detect over-subscribed or incomplete trees. Two-level decode table is recommended for speed.
- **Block decoder (T3.5):**
  - **Stored blocks (type 0):** Skip to byte boundary, read length and one’s-complement length, copy `length` bytes to output.
  - **Fixed Huffman (type 1):** Use the fixed literal/length and distance trees defined in RFC 1951; decode symbols until end-of-block; output literals and expand (length, distance) back-references.
  - **Dynamic Huffman (type 2):** Decode code length alphabet (HLIT, HDIST, HCLEN, then code lengths for literal/length and distance trees); build decode tables; same decode loop as fixed.
- **LZ77 sliding window:** Up to 32 KiB (window_bits 8..15). On (length, distance) decode, copy `length` bytes from the position that is `distance` bytes behind the current output position. Validate distance/length against the spec (invalid values → corrupt).
- **Limits and streaming:** Enforce `limits.max_output_bytes`; support chunked input (e.g. 1-byte chunks) and chunked output; `finish()` returns success only when the final block has been processed cleanly.

### 2.4 Data flow (decoder, when implemented)

1. **Stream layer:** `gcomp_decoder_update` / `finish` call deflate’s `update_fn` / `finish_fn` with input and output buffers.
2. **Deflate decoder:** Uses a **bit reader** over the input (or over an internal buffer fed from input). For each block:
   - Read 3-bit header (BFINAL, BTYPE).
   - If stored: align to byte, read length, copy bytes to output.
   - If fixed or dynamic: build or reuse Huffman tables, then decode symbols. Literals go to output; (length, distance) are expanded from the sliding window into output.
3. **Sliding window:** All decoded output is appended to a history buffer of at most 32 KiB; back-references read from this buffer.
4. **Finish:** When the block with BFINAL=1 is fully decoded and no more input is expected, `finish` succeeds. Otherwise it may return error or need more input.

---

## 3. Summary

- **Generic decode:** Create decoder from registry by method name → `update` (possibly many times) with input/output buffers → `finish` → destroy. Buffer semantics are standard (`used`/`size`); method does all format-specific work.
- **Deflate:** RFC 1951 raw DEFLATE; LSB-first bit I/O. Current code provides the bit reader and method registration; Huffman builder, block decoding (stored/fixed/dynamic), and LZ77 window are planned and documented in the task list (T3.4–T3.5).
- **Reference:** The passthru method (`tests/passthru_method.h`) is a minimal, correct implementation of the same vtable (create_decoder, update_fn, finish_fn, destroy_decoder) and is a good reference for how a method plugs into the decode path.
