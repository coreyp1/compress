# Dictionaries

Compression works by finding repetition, and a short message has almost none to
find. A 300-byte JSON record, log line or HTTP header block compresses badly
not because the data is incompressible but because there is no room for the
encoder to learn it.

A dictionary is history supplied up front, so the first byte of a message can
match against content the encoder has already been shown.

```c
#include <ghoti.io/compress/dict.h>
```

## Using one

Set it as an option and encode as normal. The decoder needs the same
dictionary.

```c
gcomp_options_set_bytes(opts, "zstd.dictionary", dict, dict_len);
```

| Method | Option | |
|--------|--------|-|
| `zstd` | `zstd.dictionary` | Raw content, or a formatted dictionary (RFC 8878 §5) |
| `zlib`, `deflate` | `zlib.dictionary`, `deflate.dictionary` | Raw content (RFC 1950 §2.2 preset dictionary) |
| `lz4` | `lz4.dictionary` | Raw content |

Measured on 1000 held-out log lines, one frame each: 1.11x without a
dictionary, 3.40x with one trained on 3000 others.

## Building one

```c
gcomp_dict_train(NULL, "zstd", NULL,
                 samples, sample_sizes, n_samples,
                 dict, sizeof(dict), &dict_size);
```

The samples should be what the dictionary will be used on: a few hundred real
records beat a megabyte of one. They are not copied or retained.

A dictionary much bigger than a hundredth of the total sample size mostly holds
content that appears once, which the encoder would have found on its own. The
usual shape is a 100 KB dictionary trained on several megabytes of samples.

| Option | Default | |
|--------|---------|-|
| `dict.segment_size` | 256 | `k`, the length of the pieces the dictionary is built from |
| `dict.dmer_size` | 8 | `d`, the length of the sequences whose frequency is counted |

`dict.segment_size` is clamped down to the median sample length, because a
segment cannot straddle two samples: without that, a 256-byte default finds
nothing at all in 170-byte log lines.

### The algorithm

FASTCOVER, the variant of COVER (Liao, Petri, Moffat and Wirth, *Effective
Construction of Relative Lempel-Ziv Dictionaries*, WWW 2016) that libzstd's
builder uses:

1. Count how often every `d`-byte sequence occurs across the samples.
2. Score each `k`-byte segment by the total frequency of the **distinct**
   sequences in it — so a segment made of many common pieces scores highly, and
   one that repeats a single common piece does not score highly twice.
3. Take the best segment, add it, and zero the frequency of everything in it so
   the next pick covers something new.
4. Repeat until the budget is full.

The best segment goes at the **end** of the dictionary. A dictionary is
history, and a match costs bits in proportion to how far back it reaches, so
the content most likely to be matched belongs closest to the data.

## The two shapes of a Zstandard dictionary

RFC 8878 §5 allows either:

- **Content-only** — just bytes. This is what `gcomp_dict_train()` writes, what
  every method here can use, and the only kind deflate and LZ4 have.
- **Formatted** — magic `0xEC30A437`, a `Dictionary_ID`, and pre-trained
  Huffman and FSE tables before the content. `zstd --train` writes these.

Both are accepted. A formatted dictionary's entropy tables help most on very
short messages, where even the tables cost more than the data; content is the
larger half of the benefit and the portable half.

## Short messages

`zstd` will not attempt to compress a block under 64 bytes **unless there is
history** — a dictionary, or an earlier block in the same frame. Without
history the sequences and entropy tables cost more than the bytes they
describe; with a dictionary the opposite is true, and a 60-byte log line
against a dictionary of log lines is nearly all match.

## Interoperability

A dictionary this library trains is raw content, which `zstd -D` accepts. On a
177-byte record: 47 bytes with ours, 52 with `zstd --train`'s own, 167 with
none.

## See also

- [Limits](limits.md) — a dictionary changes what ratios are reachable
- [`documentation/modules/zstd.md`](../modules/zstd.md)
