# Oracles, and the references they ask

Most of this suite checks the library against itself: an encoder against its own
decoder, a bound against what was written, a walk against the encoder that
produced the bytes. Those are round trips, and a round trip cannot see a
misreading of the specification that the encoder and decoder share.

The oracle tests are the part that is not self-referential. They compare against
implementations nobody here wrote: the `zstd` CLI, `liblz4`, `gzip`, Python's
`zlib` module, and `pyzstd`. Nine test files carry **ten availability
sentinels** (`OracleIsActuallyAvailable`, and `RealImplementationIsActuallyAvailable`
where one file has two references) which *fail* when their reference is absent,
because a skipped oracle test and an absent one are the same line in a summary.

This page is about the other half of that: not whether a reference is there, but
**which version of it answered.**

## The problem a container solves here

Presence was enforced; the version was not recorded anywhere. A run on this
machine compared against `zstd` 1.5.7, `liblz4` 1.10.0, `zlib` 1.3.1, `gzip`
1.13 and whatever `pyzstd` `pip` last resolved - and nothing in the repository
said any of that, so a green run named a set of tool names rather than a set of
behaviours. Two consequences, and the second is worse:

- A reference that changes an answer reads as a regression in this library.
- A reference that is *already* wrong for the comparison reads as correct
  forever, because nothing states what it was supposed to be.

`pkg-config` made the second concrete. A survey of this workspace's oracles ran
`pkg-config --modversion liblz4`, saw it fail, and concluded that fourteen LZ4
oracle tests were skipping. They were all passing: `liblz4.so.1` was installed
and every one of those tests loads it by name at run time with
`gcu_library_open`. `pkg-config` answers *can I build against this*; the tests
ask *can I load this*. There is no development package here and there does not
need to be.

## What is pinned

`tools/oracle/containers/IMAGES` is the manifest: one line per reference, with
the version it must claim when asked. All five lines name one image, built from
`tools/oracle/containers/refs/Containerfile`.

| reference | version | answers for |
| --- | --- | --- |
| `zstd` | zstd 1.5.7 | frames, blocks, dictionaries, the seek table |
| `liblz4` | liblz4 1.10.0 | the LZ4 frame and block formats |
| `zlib` | zlib 1.3.1 | RFC 1950 and RFC 1951 |
| `gzip` | gzip 1.13 | RFC 1952 member structure |
| `pyzstd` | pyzstd 0.19.1 zstd 1.5.7 | the seekable format, dictionary training |

Three of those version strings are deliberately not the name of a package:

- **`zlib`** is the zlib `python3` links at run time
  (`ZLIB_RUNTIME_VERSION`), not the interpreter's version. Only the first
  decides whether a stream round-trips.
- **`pyzstd`** names *two* versions, because `pyzstd` 0.19.1 declares
  `backports-zstd>=1.0.0` and, below Python 3.14, that dependency **is** the
  zstd implementation - a C extension with its own libzstd compiled in, linking
  none. Pinning only `pyzstd` would leave the thing that actually answers
  floating on an open range.
- **`liblz4`** is asked by loading the soname and calling
  `LZ4_versionString()`, which is how the tests reach it. No `lz4` CLI is
  installed; a CLI's version is a fact about a different file.

### One image, not five

Every other library in this workspace has an image per reference. This one
cannot, for two measured reasons:

- `/usr/bin/zstd` links `liblz4.so.1`. An image with a pinned zstd CLI and no
  liblz4 has no working zstd CLI either.
- What runs inside the image is a **compiled test binary**, not a driver script.
  A single binary consults up to three of these references in one run, and
  `liblz4` is reached by `dlopen` - so the only way to pin it at all is for the
  binary that loads it to be the process running in the image.

The binaries are built on the host, against the host's own compiler, and only
*run* inside. This image pins the references; it does not pin the build.

## Using it

```bash
make oracle-build      # build the image: once, and whenever a pin moves
make oracle-version    # print every reference and its version, or fail
make check-oracle      # run the nine oracle suites against the pinned set
make oracle-help       # the short version of this page
```

`check-oracle` is **not** in `TEST_GATES`, which is what every library that
landed this pattern also decided: `make test` must not require a container
engine. The ordinary suite still runs the same binaries against whatever the
machine has.

`GHOTI_ORACLE_MODE=host` uses this machine's own tools instead - and still
checks them against the pins. On a machine whose `pyzstd` is not 0.19.1 that
fails, which is the point rather than a gap: a seekable-format differential
against an unrecorded `pyzstd` is not the claim the gate prints.

## How it fails

Every one of these was produced deliberately before the gate was believed.

| provoked | result |
| --- | --- |
| `IMAGES` says `zstd 1.5.8`, image has 1.5.7 | refuses, printing both |
| the pinned image is not built | refuses, naming `make oracle-build` |
| a gate asks for a reference `IMAGES` does not name | refuses |
| any test skips inside the image | the run is **red** |
| a binary runs no tests at all | the run is red |

**A skip is a failure in here**, which is the rule that differs from the host
suite. On the host a skip means "this machine has no such reference" - a fact
about the machine. In the pinned image every reference is present by
construction, so a skip means a test could not reach something the image
promises, and that reads identically to a green run in every summary line.

The pin check runs before any binary does, so a missing or drifted reference
cannot reach a test at all. The skip count is the second line of defence, for a
reference that is present but unusable, and it needs nobody to have remembered
to write a sentinel - which matters, because `test_lz4_spec_oracle.cpp` shipped
a sentinel that answered for only one of its file's two references.

## What a container cannot pin

Four `golden_vectors.h` files - deflate, gzip, lz4 and zstd - hold committed
bytes attributed to a reference in a comment: *"VERIFIED using Python's lz4
library (version 4.4.5)"*, and a generation command written as
`python3 -c "import lz4.frame; ..."`. The version is recorded and the command is
an ellipsis, so the bytes cannot be regenerated from what is written, and no
image can retroactively pin what produced them.

That is a **materialised** reference rather than a live one, and it wants its own
piece of work: a real generator, run in a pinned image, proved to reproduce the
committed bytes exactly. Until then those suites check this library against
bytes whose provenance is a sentence. They are deliberately not in
`ORACLE_TEST_NAMES`, which names only the binaries that consult a live
reference.
