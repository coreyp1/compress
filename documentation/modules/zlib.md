# zlib Module (RFC 1950)

The zlib container: two header bytes, a DEFLATE stream, and a four-byte
Adler-32 of the uncompressed data.

```
+---+---+                     +---+---+---+---+
|CMF|FLG|    DEFLATE data     |    ADLER32    |
+---+---+                     +---+---+---+---+
   (+ 4-byte DICTID when FDICT is set)
```

Six bytes of overhead, and what PNG's IDAT stream, HTTP's `deflate`
content-coding, PDF's `/FlateDecode` and a great many embedded formats are
actually made of. Raw `deflate` is not interchangeable with any of them: it
has no header to identify it and no checksum to verify it.

## Registration

Registered automatically as `"zlib"`. It requires `"deflate"` in the same
registry, which is also automatic.

```c
gcomp_registry_t *registry = gcomp_registry_default();

gcomp_encoder_t *encoder = NULL;
gcomp_encoder_create(registry, "zlib", NULL, &encoder);
```

## Choosing between deflate, zlib and gzip

All three compress with the same algorithm and differ only in the wrapper:

| Method | Overhead | Checksum | Use when |
|---|---|---|---|
| `deflate` | 0 bytes | none | Something else already frames and verifies the data |
| `zlib` | 6 bytes | Adler-32 of the input | A format asks for RFC 1950 |
| `gzip` | 18+ bytes | CRC-32 of the input | Files on disk; carries a name, a timestamp and a stronger checksum |

Adler-32 is the weaker of the two checksums and notably poor on short inputs.
That is a property of RFC 1950, not a choice made here; where the format is
free, this library uses CRC-32.

## Header

RFC 1950 §2.2. Every bit is specified:

| Field | Bits | Meaning |
|---|---|---|
| CM | CMF 0–3 | Compression method. 8 (deflate) is the only legal value. |
| CINFO | CMF 4–7 | log2(window) − 8. At most 7, a 32 KiB window. |
| FCHECK | FLG 0–4 | Whatever makes `CMF*256 + FLG` a multiple of 31. |
| FDICT | FLG 5 | A preset dictionary is required. |
| FLEVEL | FLG 6–7 | Compression level hint, 0 (fastest) to 3 (maximum). |

Two things follow from that table:

- **CINFO tells the truth.** It is written from the window `deflate.window_bits`
  actually selects, so a decoder sizing its history from the header gets the
  right answer.
- **FCHECK is why a mangled header is cheap to spot.** The modulus is checked
  before anything is decompressed, which is what stops a raw deflate stream, a
  gzip file or a truncated download being fed into the decompressor at all.

FLEVEL is a hint, and RFC 1950 says outright that it "need not be set
correctly". It is set correctly anyway — a stream that says something false
about itself is a trap for whoever reads it next — and the values match
zlib's own boundaries, so the header bytes this library writes are byte for
byte the ones zlib would write for the same settings. There is a test that
checks exactly that.

## Options

Compression is configured with the deflate keys, which pass straight through:

| Key | Type | Default | Description |
|---|---|---|---|
| `deflate.level` | int64 | 6 | 0 (stored) to 9 (best) |
| `deflate.window_bits` | uint64 | 15 | 8 to 15; also what CINFO records |
| `deflate.strategy` | string | `default` | See [deflate](deflate.md) |

### zlib-specific

| Key | Type | Default | Description |
|---|---|---|---|
| `zlib.dictionary` | bytes | none | Preset dictionary. **Not supported**; see below. |

### Limits

| Key | Type | Default | Description |
|---|---|---|---|
| `limits.max_output_bytes` | uint64 | 512 MiB | Maximum decompressed output |
| `limits.max_memory_bytes` | uint64 | 256 MiB | Maximum working memory |
| `limits.max_expansion_ratio` | uint64 | 1000 | Decompression bomb protection |

## Preset dictionaries (FDICT)

RFC 1950 allows a stream to be compressed against a preset dictionary, flagged
by FDICT and identified by a four-byte Adler-32 of that dictionary. That needs
the *deflate* encoder and decoder to accept a dictionary, and this library's do
not yet.

This is the one part of RFC 1950 not covered, and it is refused rather than
fudged:

- Setting `zlib.dictionary` on an encoder fails at creation with
  `GCOMP_ERR_UNSUPPORTED`. Quietly clearing FDICT would produce a stream that
  decodes to the wrong bytes for anyone who had the dictionary.
- Decoding a stream with FDICT set fails with `GCOMP_ERR_UNSUPPORTED` and an
  error naming the dictionary id, rather than producing plausible nonsense.

FDICT is rare in practice: PNG forbids it outright (PNG §10.3), and HTTP and
PDF do not use it.

## Flushing

`gcomp_encoder_flush()` works as it does for [deflate](deflate.md#flushing) —
a sync flush writes the empty stored block that byte-aligns the stream, and
`GCOMP_FLUSH_FULL` additionally drops the match history. The Adler-32 is
accumulated as input arrives, so it needs nothing at flush time and is already
correct for whatever the flush emits.

See [Streaming API](../api/streaming.md#flushing).

## Peeking at a header

```c
gcomp_zlib_header_info_t info;
if (gcomp_zlib_peek_header(data, len, &info) == GCOMP_OK) {
    // It is a zlib stream; info.window_bits, info.level_hint,
    // info.has_dictionary, info.dictionary_id, info.header_size
}
```

Two bytes are enough unless FDICT is set, when six are needed. Returns
`GCOMP_ERR_CORRUPT` for anything that is not a zlib header — which is the
cheapest way to tell a zlib stream from a raw deflate one.

## Error handling

| Status | Cause |
|---|---|
| `GCOMP_ERR_CORRUPT` | CM is not 8, CINFO above 7, the FCHECK modulus fails, the Adler-32 does not match, or the stream is truncated |
| `GCOMP_ERR_UNSUPPORTED` | FDICT is set, or `zlib.dictionary` was given to an encoder |
| `GCOMP_ERR_LIMIT` | An output or expansion-ratio limit was exceeded |

The Adler-32 is checked at the end, against output the caller has already been
given. There is no way to un-hand those bytes; the mismatch is reported so the
caller knows not to trust them.

## Interoperability

Verified in both directions against the real zlib, at every compression level
and every window size, including the header bytes and the trailer compared
byte for byte. See `tests/methods/zlib/test_zlib_oracle.cpp`; those tests skip
rather than fail where Python's `zlib` module is unavailable.

## Security considerations

The same limits as every other method, and one thing particular to this one:
the container carries no length, so the end of the deflate stream is found by
deflate itself reaching its final block. A stream that never reaches one is
truncation, and is reported as such by `gcomp_decoder_finish()`.

## See also

- [deflate](deflate.md) — the compression underneath
- [gzip](gzip.md) — the other wrapper around it
- [Wrapper methods](../wrapper-methods.md) — the pattern both follow
- [Streaming API](../api/streaming.md) — including flush
