# Fuzz Testing Guide

This document describes how to use fuzz testing to find bugs, crashes, and edge cases in the compress library.

## Overview

Fuzz testing (fuzzing) is a technique that feeds random or semi-random data to a program to find crashes, hangs, memory errors, and other bugs. We use [AFL++](https://github.com/AFLplusplus/AFLplusplus), a powerful coverage-guided fuzzer.

### What Fuzzing Can Find

- **Crashes**: Segmentation faults, null pointer dereferences
- **Memory errors**: Buffer overflows, use-after-free (when combined with ASan)
- **Hangs**: Infinite loops, excessive computation
- **Logic bugs**: Roundtrip mismatches (encode then decode doesn't match original)
- **Edge cases**: Handling of malformed, truncated, or adversarial input

## Prerequisites

### Install AFL++

On Ubuntu/Debian/WSL2:

```bash
sudo apt update
sudo apt install afl++
```

On other systems, see the [AFL++ installation guide](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/INSTALL.md).

### Verify Installation

```bash
afl-gcc --version
afl-fuzz --help
```

## Quick Start

```bash
# 1. Generate seed corpus from test vectors
make fuzz-corpus

# 2. Build fuzz harnesses with AFL instrumentation
make fuzz-build

# 3. Start fuzzing (pick one)
make fuzz-decoder    # Fuzz the decoder
make fuzz-encoder    # Fuzz the encoder
make fuzz-roundtrip  # Fuzz encode+decode roundtrip

# 4. Press Ctrl+C to stop fuzzing
```

## Available Fuzz Targets

### Decoder Fuzzer (`fuzz-decoder`)

Tests the DEFLATE decoder with arbitrary input bytes. This finds:
- Crashes on malformed compressed data
- Hangs on pathological input
- Memory safety issues in parsing

```bash
make fuzz-decoder
```

### Encoder Fuzzer (`fuzz-encoder`)

Tests the DEFLATE encoder with arbitrary plaintext. This finds:
- Crashes during compression
- Issues with different input patterns
- Memory safety in the encoder

```bash
make fuzz-encoder
```

### Roundtrip Fuzzer (`fuzz-roundtrip`)

Compresses input, then decompresses it, and verifies the output matches the input. This is powerful because any mismatch is definitely a bug.

```bash
make fuzz-roundtrip
```

## Gzip Fuzz Targets

The gzip method has dedicated fuzz harnesses that test the wrapper functionality:

### Gzip Decoder Fuzzer (`fuzz-gzip-decoder`)

Tests the gzip decoder with arbitrary input bytes. This finds:
- Header parsing bugs (magic, flags, FNAME, FCOMMENT, FEXTRA, FHCRC)
- Integration issues between gzip wrapper and deflate decoder
- Trailer validation (CRC32, ISIZE mismatch handling)
- Limit enforcement (header field sizes, expansion ratio)
- Concatenated member handling

```bash
make fuzz-gzip-decoder
```

### Gzip Encoder Fuzzer (`fuzz-gzip-encoder`)

Tests the gzip encoder with arbitrary plaintext. This finds:
- Header generation issues
- CRC32 computation bugs
- Integration with inner deflate encoder
- Memory safety in option handling

```bash
make fuzz-gzip-encoder
```

### Gzip Roundtrip Fuzzer (`fuzz-gzip-roundtrip`)

Compresses input as gzip, then decompresses, and verifies the output matches. Tests the complete encode/decode path including:
- Header/trailer roundtrip correctness
- CRC32/ISIZE computation and validation
- Optional fields (name, comment) preservation

```bash
make fuzz-gzip-roundtrip
```

### Gzip vs Deflate Fuzzing

| Aspect | Deflate Harnesses | Gzip Harnesses |
|--------|-------------------|----------------|
| Format | Raw DEFLATE stream | GZIP wrapper (header + deflate + trailer) |
| Decoder input | Compressed bytes | Full gzip file format |
| Validates | Stream integrity | Header, CRC32, ISIZE |
| Limit testing | Basic limits | Header field limits, expansion ratio |
| Concatenation | N/A | Multi-member gzip streams |

**Recommendation**: Run both deflate and gzip harnesses. Deflate harnesses test compression core; gzip harnesses test wrapper and format handling.

## Understanding AFL++ Output

When you run AFL++, you'll see a status screen:

```
       american fuzzy lop ++4.00c {default} (fuzz_decoder)
┌─ process timing ────────────────────────────────────┬─ overall results ─────┐
│        run time : 0 days, 0 hrs, 5 min, 23 sec      │  cycles done : 42     │
│   last new find : 0 days, 0 hrs, 0 min, 12 sec      │ corpus count : 156    │
│ last uniq crash : none seen yet                     │  saved crashes : 0    │
│  last uniq hang : none seen yet                     │   saved hangs : 0     │
├─ cycle progress ───────────────────┬─ map coverage ─┴───────────────────────┤
│  now processing : 23.1 (14.7%)     │    map density : 2.45% / 3.12%         │
│  runs timed out : 0 (0.00%)        │ count coverage : 4.21 bits/tuple       │
...
```

Key metrics:
- **saved crashes**: Number of unique inputs that caused crashes (bugs!)
- **saved hangs**: Number of inputs that caused timeouts
- **corpus count**: Number of interesting test cases found
- **map coverage**: Code coverage achieved

## Finding and Analyzing Bugs

### Crash Files

When AFL++ finds a crash, it saves the input to:
```
fuzz/findings/<target>/crashes/id:XXXXXX,...
```

To reproduce a crash:
```bash
# Run the harness directly with the crash input
./build/linux/release/apps/fuzz/fuzz_deflate_decoder < fuzz/findings/decoder/crashes/id:000000,...
```

### Analyzing with GDB

```bash
# Build without AFL instrumentation for debugging
gcc -g -O0 -o fuzz_debug fuzz/fuzz_deflate_decoder.c \
    -I include/ build/linux/release/apps/libghoti.io-compress-dev.a -lm

# Run under GDB
gdb ./fuzz_debug
(gdb) run < fuzz/findings/decoder/crashes/id:000000,...
```

### Using AddressSanitizer

For better crash analysis, build with ASan:

```bash
AFL_USE_ASAN=1 afl-gcc -O2 -g -fsanitize=address \
    -o fuzz_decoder_asan fuzz/fuzz_deflate_decoder.c \
    -I include/ build/linux/release/apps/libghoti.io-compress-dev.a -lm
```

## Advanced Usage

### Running Multiple Fuzzer Instances

AFL++ can run multiple parallel instances for faster fuzzing:

```bash
# Terminal 1 - Main instance
afl-fuzz -M main -i fuzz/corpus/decoder -o fuzz/findings/decoder \
    -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder

# Terminal 2 - Secondary instance
afl-fuzz -S secondary1 -i fuzz/corpus/decoder -o fuzz/findings/decoder \
    -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder
```

### Resuming a Fuzzing Session

AFL++ automatically resumes from previous findings:

```bash
# If fuzz/findings/decoder exists with previous results, AFL++ will resume
make fuzz-decoder
```

To start fresh, remove the findings directory:
```bash
rm -rf fuzz/findings/decoder
make fuzz-decoder
```

### Custom Fuzzer Options

Run AFL++ directly for more control:

```bash
afl-fuzz \
    -i fuzz/corpus/decoder \
    -o fuzz/findings/decoder \
    -t 5000 \                    # 5 second timeout
    -m 512 \                     # 512 MB memory limit
    -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder
```

### Minimizing Test Cases

After finding a crash, minimize it to the smallest reproducing input:

```bash
afl-tmin -i fuzz/findings/decoder/crashes/id:000000,... \
         -o minimized_crash.bin \
         -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder
```

## Seed Corpus

The seed corpus provides starting inputs for the fuzzer. Better seeds lead to better coverage faster.

### Generating Corpus

```bash
make fuzz-corpus
```

This creates:
- `fuzz/corpus/decoder/` - Valid DEFLATE streams and malformed edge cases
- `fuzz/corpus/encoder/` - Various plaintext patterns
- `fuzz/corpus/roundtrip/` - Various plaintext patterns

### Adding Custom Seeds

Add your own seed files to the corpus directories:

```bash
# Add a specific test case
cp my_interesting_file.bin fuzz/corpus/decoder/

# Add compressed files from the wild
gunzip -c some_file.gz > temp.deflate  # Note: gzip has header, need raw deflate
```

## Continuous Fuzzing

For thorough testing, run fuzzers for extended periods:

```bash
# Run overnight
timeout 8h make fuzz-decoder

# Run for a week (use tmux/screen)
timeout 168h make fuzz-roundtrip
```

## Troubleshooting

### "No instrumentation detected"

Make sure you're using `afl-gcc` or `afl-clang-fast`:
```bash
make fuzz-build AFL_CC=afl-gcc
```

### "Hmm, your system is configured to send core dump notifications..."

This is common on Ubuntu/WSL2. You have two options:

**Option 1: Fix core_pattern (recommended for serious fuzzing)**
```bash
echo core | sudo tee /proc/sys/kernel/core_pattern
```
This change persists until reboot.

**Option 2: Skip the check (for quick testing)**
```bash
make fuzz-decoder AFL_ENV='AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1'
```
Note: This may cause AFL to miss some crashes or misreport them as timeouts.

### "The target binary looks like a shell script..."

Ensure the fuzz harness is a compiled binary, not a script.

### Slow Fuzzing

- Use smaller input sizes (the harnesses limit input to prevent this)
- Run multiple parallel instances
- Use `afl-clang-fast` instead of `afl-gcc` (faster instrumentation)

## Directory Structure

```
fuzz/
├── fuzz_deflate_decoder.c   # Deflate decoder fuzz harness
├── fuzz_deflate_encoder.c   # Deflate encoder fuzz harness
├── fuzz_roundtrip.c         # Deflate roundtrip fuzz harness
├── fuzz_gzip_decoder.c      # Gzip decoder fuzz harness
├── fuzz_gzip_encoder.c      # Gzip encoder fuzz harness
├── fuzz_gzip_roundtrip.c    # Gzip roundtrip fuzz harness
├── generate_corpus.c        # Seed corpus generator
├── corpus/                  # Seed inputs (generated)
│   ├── decoder/             # Raw deflate test inputs
│   ├── encoder/             # Plaintext inputs for deflate
│   ├── roundtrip/           # Plaintext for deflate roundtrip
│   ├── gzip_decoder/        # Gzip format test inputs
│   ├── gzip_encoder/        # Plaintext inputs for gzip
│   └── gzip_roundtrip/      # Plaintext for gzip roundtrip
└── findings/                # AFL++ output (generated)
    ├── decoder/
    │   ├── crashes/         # Crash-inducing inputs
    │   ├── hangs/           # Hang-inducing inputs
    │   └── queue/           # Interesting test cases
    ├── encoder/
    ├── roundtrip/
    ├── gzip_decoder/
    ├── gzip_encoder/
    └── gzip_roundtrip/
```

## References

- [AFL++ Documentation](https://github.com/AFLplusplus/AFLplusplus/tree/stable/docs)
- [AFL++ Fuzzing Tutorial](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/fuzzing_in_depth.md)
- [Google Fuzzing Guide](https://github.com/google/fuzzing)
