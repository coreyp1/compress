# Ghoti.io Compress Library

Cross-platform C library implementing streaming compression with no external dependencies.

## Overview

The `compress` library provides:
- Streaming compression and decompression for files, memory buffers, and pipes/sockets
- Support for multiple compression methods (deflate, gzip, zlib, lz4, lzw, rle, zstd)
- Parallel encoding via `threads.count` for zstd (concatenated frames) and LZ4
  (one frame, byte-identical to single-threaded output)
- `gcomp_encoder_flush()` for protocol framing: hand the peer everything
  consumed so far without ending the stream (sync and full modes, every method)
- Global default registry and explicit registries for compression methods
- Key/value option system for rich configuration
- Intelligent safety defaults with overridable resource limits

## Dependencies

- `cutil` - Ghoti.io core utilities.  Parallel compression is built on its
  `GCU_Pool` (worker threads) and `GCU_Sequencer` (reorder buffer), used
  together in `src/core/parallel_block.c`: the pool decides when a block is
  compressed, the sequencer decides what order the finished blocks are
  written in.  This library had its own copy of both and no longer does.

No third-party dependencies: every compression method is implemented from
scratch against libc.

## Building

```bash
make
```

## Testing

```bash
make test
```

### Running Tests with Valgrind

```bash
make test-valgrind
```

### Benchmarks

Micro-benchmarks for throughput and behavior are built with:

```bash
make bench
```

Run them (set `LD_LIBRARY_PATH` to the build `apps` directory first):

- **bench_deflate** – Deflate encode/decode throughput by data type and level; scaling check.
- **bench_lzw** – LZW encoder lookup modes (linear vs hash); reports encode throughput.

See `make bench` output for the exact run commands for your platform.

### Fuzz Testing

The library includes fuzz testing infrastructure using [AFL++](https://github.com/AFLplusplus/AFLplusplus) to find edge cases and security issues.

**Prerequisites:**
```bash
sudo apt install afl++
```

**Running fuzz tests:**
```bash
# Generate seed corpus from test vectors
make fuzz-corpus

# Build fuzz harnesses with AFL instrumentation
make fuzz-build

# Run a fuzzer (Ctrl+C to stop)
make fuzz-decoder    # Test decoder with random compressed data
make fuzz-encoder    # Test encoder with random input
make fuzz-roundtrip  # Encode then decode, verify match

# Show help and available targets
make fuzz-help
```

Findings are saved to `fuzz/findings/<target>/crashes/`. See `documentation/testing/fuzzing.md` for detailed documentation.

### Oracle Testing

The oracle tests are the part of the suite that is not self-referential: they
compare this library's output against implementations nobody here wrote. Nine
test files carry ten availability sentinels that **fail** when their reference
is absent, because a skipped oracle test and an absent one are the same line in
a summary. `GCOMP_SKIP_ORACLE_TESTS=1` is how a machine without them says so
deliberately.

**The recommended way to run them is against pinned references, in a
container:**

```bash
make oracle-build      # once, and whenever a pin moves
make check-oracle      # the nine oracle suites, against the pinned set
make oracle-version    # print every reference and its version
```

Every version is recorded in `tools/oracle/containers/IMAGES` and checked at run
time, so a run states what answered rather than naming a set of tool names. In
there a **skip is a failure**: the references are present by construction, so a
skip means a test could not reach one the image promises.
`documentation/testing/oracles.md` is the full account, including what a
container cannot pin.

**To run them against this machine's own tools instead**, the references are:

| reference | what needs it | how it is reached |
| --- | --- | --- |
| `zstd` CLI | zstd frames, dictionaries, the seek table | `system()` |
| `liblz4` runtime library | the LZ4 frame and block formats | `dlopen` by soname |
| `gzip` / `gunzip` CLI | RFC 1952 member structure | `system()` |
| python `zlib` module | RFC 1950 and RFC 1951 | `python3 -c` |
| python `pyzstd` | the seekable format, dictionary training | `python3 -c` |

```bash
sudo apt install gzip zstd liblz4-1
pip3 install pyzstd
```

Two things that were wrong here for a long time and are worth stating, because
both cost somebody a wrong conclusion:

- **`liblz4` needs no development package.** The tests load it by name at run
  time, so `pkg-config --modversion liblz4` failing says nothing about whether
  they run. A survey of this workspace read that failure as fourteen skipping
  tests; all fourteen were passing.
- **Neither the `lz4` CLI nor the python `lz4` and `zstandard` modules are
  used.** This section used to ask for all three. `pyzstd` is what the seekable
  and dictionary-training oracles use, and it was not mentioned.

Verify a host set with:

```bash
make oracle-version GHOTI_ORACLE_MODE=host
```

which prints each reference's version and fails if any does not match its pin -
which is how a drifting reference is found rather than silently used.

### Continuous Integration

Every push and pull request runs `.github/workflows/ci.yml`, which is nothing
but the targets above, run in eight jobs (the first is a matrix of two
compilers):

| Job | Runs |
| --- | --- |
| Build and test (gcc) | `make`, `check-symbols`, `test`, `examples`, `install`, `tools/check-install.sh` |
| Build and test (clang) | the same, with `CC=clang CXX=clang++` |
| ASan + UBSan | `make test-asan` |
| ThreadSanitizer | `make test-tsan`, in its own build tree |
| Valgrind | `make test-valgrind-quiet`, which fails on a leak |
| Fuzz corpus replay | `make fuzz-replay AFL_CC=clang` over the tracked `fuzz/regression` corpus |
| Coverage floor | `make coverage COVERAGE_MIN=...`, which fails if line coverage drops below the floor |
| Oracles (pinned) | `make oracle-build`, `oracle-version`, `check-oracle` - the oracle suites against pinned reference versions |
| Windows (MSYS2) | the everyday gate again, natively on `windows-latest` under MINGW64 |

Two things there are not just a target being run:

- **`tools/check-install.sh`** compiles a program that knows nothing but the
  module name, links it with whatever `pkg-config` hands back, runs it, and
  round-trips a buffer through every method. The tests link the static archive
  with `--whole-archive` and include headers straight out of `include/`, so
  nothing else checks what a consumer of the *installed* library actually gets.
  It can be run by hand against any prefix:

  ```bash
  tools/check-install.sh /path/to/prefix
  ```

- **The oracle references are installed by CI rather than left to chance.**
  Nine test files carry ten sentinels asserting that their reference
  implementation is actually present, because a skipped oracle test and an
  absent one look identical in the summary line - a run that compared nothing
  against anything would otherwise report success. `GCOMP_SKIP_ORACLE_TESTS=1`
  is how a machine without them says so deliberately. Ten rather than nine
  because `test_lz4_spec_oracle.cpp` has two references and one sentinel cannot
  answer for both; it shipped with one that covered only the specification half.

  CI installs those references unpinned, which `make check-oracle` is the answer
  to: it runs the same suites against the versions
  `tools/oracle/containers/IMAGES` names.

A campaign with `afl-fuzz` is not in CI: it needs a corpus that persists
between runs to be worth anything, and the replay above is the part worth
running on every change. The campaigns themselves are documented in
`documentation/testing/fuzzing.md`.

`make fuzz-corpus` seeds every method - each of `fuzz/corpus/<method>_decoder`,
`_encoder` and `_roundtrip`, with deflate's being the unprefixed ones - and
then fails if any campaign target names a directory it did not fill. It reads
that list out of the Makefile rather than keeping a second copy, because the
two had already drifted: LZ4 and zstd had no seeds at all and gzip's roundtrip
directory was created empty, so those campaigns fell back to a single
hand-written frame apiece without saying so.

## Installation

```bash
sudo make install
```

## Usage

See the examples directory for usage examples.

## Documentation

- [Modules](@ref modules) - Detailed documentation for library modules
- [Examples](@ref examples) - Example programs demonstrating library usage
- [Function Index](@ref functions_index) - Complete API reference

## Macros and Utilities

The library provides cross-compiler macros in `include/ghoti.io/compress/macros.h`:

- `GCOMP_MAYBE_UNUSED(X)` - Mark unused function parameters
- `GCOMP_DEPRECATED` - Mark deprecated functions
- `GCOMP_API` - Mark functions for library export
- `GCOMP_ARRAY_SIZE(a)` - Get compile-time array size
- `GCOMP_BIT(x)` - Create bitmask with bit x set

Example:
```c
#include <ghoti.io/compress/macros.h>

void my_function(int GCOMP_MAYBE_UNUSED(param)) {
    // param is intentionally unused
}
```

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
