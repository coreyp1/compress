# Asking the library about a stream

Four functions that answer questions a caller has to settle *before* it can
compress or decompress safely:

| Function | Question |
| --- | --- |
| `gcomp_encode_bound()` | How big must my output buffer be? |
| `gcomp_peek()` | What does this stream's header say? |
| `gcomp_detect()` | Which method wrote these bytes? |
| `gcomp_decode_alloc()` / `gcomp_encode_alloc()` | Just give me a buffer. |

None of them decode anything. `gcomp_peek()` and `gcomp_detect()` read a
header; `gcomp_encode_bound()` reads only the options and a length.

---

## `gcomp_encode_bound()` — sizing an output buffer

```c
size_t bound = 0;
gcomp_status_t s = gcomp_encode_bound(NULL, "zstd", opts, input_len, &bound);
if (s != GCOMP_OK) { /* handle */ }

void * out = malloc(bound);
size_t written = 0;
gcomp_encode_buffer(NULL, "zstd", opts, input, input_len, out, bound, &written);
```

A buffer of `bound` bytes is always enough: encoding cannot fail for want of
room, whatever the input bytes turn out to be.

**Pass the same options you will pass to the encoder.** They change the answer.
A gzip name and comment, an LZ4 block size, LZ4 or zstd checksums, and a zstd
window that lowers the maximum block size all move it — and for deflate the
window size moves it a lot (see below).

### What it does not cover

One whole stream: create, update as often as you like, finish. It does **not**
account for `gcomp_encoder_flush()`. A flush ends a block early and pads to a
byte boundary, so it adds output every time it is called, and how often a
caller calls it is not knowable from the input size. A caller that flushes must
size its own buffer or stream into one it can refill.

### Compression can make data bigger

That is not a defect, and the bound is where the library says so honestly.
Every method here falls back to storing data it cannot compress, and the bound
is what that fallback costs — the input, plus the framing around it.

For incompressible input, roughly:

| Method | Overhead | Why |
| --- | --- | --- |
| zstd | ~0.003% | 3 bytes per 128 KB block |
| lz4 | ~0.001% at 4 MB blocks | 4 bytes per block |
| gzip, zlib | ~0.03% | 5 bytes per stored block, plus a fixed header |
| deflate | 0.018% at `window_bits` 15 | as above |
| deflate | 0.122% at `window_bits` 8 to 12 | see below |
| LZW | up to ~50% | a full-width code per byte |
| RLE | up to 33% | one control byte per three input bytes |

**A small window costs a little, and used to cost a lot.** RFC 1951 §3.2.4's
stored block is the fallback that keeps the overhead to about five bytes per
block, and reaching it needs two things that have nothing to do with the match
finder's reach: a block long enough to be worth storing, and the block's bytes
still being somewhere the encoder can read them. Both used to be sized from
`window_bits`, so a 256-byte window meant very short blocks — 21% overhead at
`window_bits` 8, and the series was not even monotonic. Both now have floors of
their own, and the measured overhead on incompressible input is 0.122% from
`window_bits` 8 through 12 and lower above that — below zlib's 0.217% to
0.232% over the same range. (zlib refuses a raw `windowBits` of 8 outright and
silently writes 9 for the wrapped form.)

LZW and RLE expand on data that suits them badly, which is what their bounds
describe. RLE's worst case is not "every byte a literal" — it is data that
alternates between a byte that cannot run and a pair that can, costing four
output bytes for every three input bytes.

---

## `gcomp_peek()` — what a header says

```c
gcomp_stream_info_t info;
size_t needed = 0;
gcomp_status_t s = gcomp_peek(NULL, "zstd", NULL, buf, len, &info, &needed);
if (s == GCOMP_ERR_LIMIT) {
  /* Read up to `needed` bytes and call again. */
}
```

Every field is read out of the header. Nothing is inferred and nothing is
decoded.

The field most callers want is `content_size`, where a format states it. A
caller that knows it can allocate exactly and set `limits.max_output_bytes` to
the same number — a tighter and more honest bound than the expansion-ratio
heuristic that otherwise stands in for it.

```c
if (info.has_content_size) {
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", info.content_size);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0);
}
```

`has_content_size` and `has_dictionary` exist separately from the values they
guard because zero is a legitimate content size and a legitimate dictionary ID.

### What each format can tell you

| | header | content size | window | checksum | dictionary |
| --- | --- | --- | --- | --- | --- |
| zstd | yes | when written | yes | yes | yes |
| lz4 | yes | when written | 64 KB | yes | yes |
| zlib | yes | no | yes | always | yes (FDICT) |
| gzip | yes | no | 32 KB | always | no |
| deflate | none | no | from options | no | no |
| LZW | none | no | none | no | no |
| RLE | none | no | none | no | no |

**gzip does not report a content size**, though it has one. `ISIZE` is in the
trailer, not the header; it is the size modulo 2^32; and a gzip file may hold
several members. Reporting it from a header peek would be wrong for any file
over 4 GB and for every multi-member file.

deflate, LZW and RLE begin with data. The call succeeds with `header_size` zero
and nothing stated — the honest answer for a format with no header, and the
reason each field is guarded by its own flag.

### Skippable frames

A stream that opens with a skippable frame reports **that frame**, not what
follows it. LZ4 and Zstandard define skippable frames on the same magic range
(`0x184D2A50`–`0x184D2A5F`) with the same layout, so such a frame belongs to
both formats and says nothing about the one after it. Step over it with
`skippable_size` and peek again.

---

## `gcomp_detect()` — which format is this?

```c
const char * method = NULL;
size_t needed = 0;
if (gcomp_detect(buf, len, &method, &needed) == GCOMP_OK) {
  gcomp_decode_alloc(NULL, method, NULL, buf, len, &out, &out_len);
}
```

There is no registry parameter: which format wrote a stream is a property of
the bytes.

**What it recognises with confidence** — gzip (`1f 8b 08`), zstd
(`28 b5 2f fd`) and lz4 (`04 22 4d 18`). Four bytes of magic do not occur by
accident.

**What it guesses at** — zlib has no magic number. RFC 1950 gives it two header
bytes with enough structure to test: a compression method of 8 (one byte value
in sixteen), a window of at most 32 KB (one in two), and the pair divisible by
31 (one in 31). About one random byte pair in 992 satisfies all three — a
measured figure, not the one in 31 that the divisibility test alone suggests.
zlib is reported only when nothing else matches. If a wrong answer is
expensive, treat it as a hint and confirm by decoding.

**What it will not guess at** — deflate, LZW and RLE begin with data. They are
never reported; a stream in one of them comes back `GCOMP_ERR_UNSUPPORTED`.
That is better than a guess the caller would act on.

Skippable frames are stepped over until a frame that identifies itself is
reached, for the reason given above.

---

## `gcomp_decode_alloc()` and `gcomp_encode_alloc()`

```c
void * out = NULL;
size_t out_len = 0;
gcomp_status_t s = gcomp_decode_alloc(NULL, "gzip", opts, in, in_len,
                                      &out, &out_len);
if (s == GCOMP_OK) {
  /* ... */
  gcomp_buffer_free(NULL, out);
}
```

The buffer belongs to the caller and is freed with `gcomp_buffer_free()`, which
uses the registry's allocator — the same one that allocated it. Pass the same
registry you passed in, or `NULL` for the default in both.

Encoding takes the bound, allocates it, and trims. Decoding takes a content
size from the header where one is offered, treats it as a hint rather than a
promise, and otherwise grows by doubling.

**Every enlargement is checked against `limits.max_output_bytes` first**, so a
decompression bomb is refused by the same ceiling that bounds the decoders
rather than by exhausting memory. A caller who sets that option to zero has
asked for no ceiling and gets none.

---

## Related

- [Streaming API](streaming.md) — for data that does not fit in memory
- [Safety limits](limits.md) — `max_output_bytes` and the rest
