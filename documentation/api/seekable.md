# Reading part of a compressed file

A compressed stream is read from the beginning. To get the byte at offset
900 MB you decode the 900 MB in front of it, because a back-reference may point
anywhere behind. That is what makes a compressed log, archive or column store
awkward to sample.

The way out is to write the file as several independent frames and record where
they are. `gcomp_seekable_*` does both halves.

```c
#include <ghoti.io/compress/seekable.h>
```

## Reading

```c
gcomp_seekable_t *s = NULL;
gcomp_seekable_open_buffer(NULL, "zstd", NULL, data, size, &s);

unsigned char window[4096];
size_t got = 0;
gcomp_seekable_read(s, 900ull * 1024 * 1024, window, sizeof(window), &got);

gcomp_seekable_close(s);
```

The buffer must outlive the handle: the index points into it and reads decode
straight out of it, so nothing is copied.

A read past the end is a short read, not an error — `got` says how much there
was.

| Function | |
|----------|-|
| `gcomp_seekable_open_buffer()` | Index a file that is already in memory |
| `gcomp_seekable_size()` | Total decompressed size |
| `gcomp_seekable_frame_count()` | How many frames — the granularity of a seek |
| `gcomp_seekable_has_table()` | Did the file carry a seek table, or was one built by walking it? |
| `gcomp_seekable_read()` | Read a window at a decompressed offset |
| `gcomp_seekable_close()` | Done |

## Writing

```c
gcomp_options_t *o = NULL;
gcomp_options_create(&o);
gcomp_options_set_uint64(o, "zstd.seekable_frame_size", 256 * 1024);

size_t bound = 0;
gcomp_seekable_write_bound(NULL, "zstd", o, input_size, &bound);

void *file = malloc(bound);
size_t written = 0;
gcomp_seekable_write_buffer(NULL, "zstd", o, input, input_size,
                            file, bound, &written);
```

| Option | Default | |
|--------|---------|-|
| `zstd.seekable_frame_size` | 1 MiB | Decompressed bytes per frame |
| `zstd.seekable_checksum` | true | Record each frame's checksum in the table |

`zstd.seekable_frame_size` is the trade. Smaller frames mean finer seeking and
slightly worse compression, because each frame starts with no history: on 4 MiB
of structured text, 256 KiB frames cost about 1.6% of the input against 0.9%
for 1 MiB frames.

## The format, and what it costs you

The [Zstandard seekable
format](https://github.com/facebook/zstd/blob/dev/contrib/seekable_format/zstd_seekable_compression_format.md):
independent frames, then a skippable frame (RFC 8878 §3.1.2, magic
`0x184D2A5E`) holding per-frame compressed and decompressed sizes, and a
nine-byte footer ending in `0x8F92EAB1`.

It is a convention on top of Zstandard rather than a container of its own, and
the point is what it does **not** cost: a skippable frame is something every
Zstandard decoder already steps over, so a seekable file is an ordinary stream
to anything that does not know about the table. `zstd -d` reads it,
`gcomp_decode_buffer()` reads it, and so does any other decoder. Nothing has to
understand seeking to read the file.

Tests hold both ends of that: files libzstd writes are read here, and files
written here are read by libzstd's seekable reader *and* decompressed as plain
streams.

## Files nobody made seekable

A file with no seek table can still be opened, as long as every frame declares
its decompressed size in its own header (RFC 8878 §3.1.1.1.4). The index is
built by walking the compressed bytes — frame headers only, nothing decoded —
which makes any concatenated multi-frame file random-access for the cost of
reading its headers.

`gcomp_seekable_has_table()` says which happened. Both give the same answers.

A frame that declares no size cannot be placed without decoding everything
before it, so a file containing one is refused with `GCOMP_ERR_UNSUPPORTED`
rather than opened into a handle whose every read would decode the whole file.

## What is refused, and when

Everything that can be checked is checked at open, because that is the point at
which a broken file can still be reported as a broken file rather than as a
decode failure part way through somebody's read:

- A seek table whose frame sizes do not add up to the bytes in front of it —
  a truncated or edited file — is `GCOMP_ERR_CORRUPT`.
- A `Seek_Table_Descriptor` with reserved bits set is `GCOMP_ERR_CORRUPT`: a
  table whose shape this build does not know is one it would misread.
- A method other than `zstd` is `GCOMP_ERR_UNSUPPORTED`. LZ4's independent
  blocks can be decoded alone, but a compressed block's header does not say
  what it expands to, so an index cannot be built without decoding the file —
  which is the thing this exists to avoid.

## Not yet

Opening through read and seek callbacks rather than a buffer, so a file larger
than memory can be indexed. The format and the index do not change; only where
the bytes come from.

## See also

- [Threading](../threading.md) — `threads.count` on decode, which uses the same
  frame boundaries
- [`examples/seekable_read.c`](../../examples/seekable_read.c)
