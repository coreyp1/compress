# Zstd specification compliance

This document summarizes how the Ghoti.io Compress zstd implementation aligns with the Zstandard specification (RFC 8878 and upstream spec). It lists what is implemented, what is intentionally unsupported or limited, and any known gaps.

**Reference:** [RFC 8878](https://www.rfc-editor.org/rfc/rfc8878.html) (Zstandard compression format).

## Frame format

| Requirement | Status | Notes |
|-------------|--------|--------|
| Magic number (4 bytes, 0xFD2FB528) | Implemented | Validated in decoder; written in encoder/frame. |
| Frame descriptor (single segment, window log, dict ID, content size, checksum) | Implemented | Parsed and validated; reserved bits (descriptor bits 5–7) rejected with `GCOMP_ERR_CORRUPT`. |
| Window descriptor (1 byte when single segment) | Implemented | Window size derived and capped by limits. |
| Dict ID (0/1/2/4 bytes) | Implemented | Optional; validated when dictionary in use. |
| Content size (0/1/2/4/8 bytes) | Implemented | Optional; validated when present. |
| Content checksum (trailer, 4 bytes, xxHash32) | Implemented | Optional; validated when present; mismatch returns `GCOMP_ERR_CORRUPT`. |

Reserved or forbidden values in the frame header are rejected with `GCOMP_ERR_CORRUPT`.

## Block types

| Type | Value | Status | Notes |
|------|-------|--------|--------|
| Raw | 0 | Implemented | Uncompressed; copied directly. |
| RLE | 1 | Implemented | Single byte repeated. |
| Compressed | 2 | Implemented | Literals + sequences (FSE/Huffman). |
| Reserved | 3 | Rejected | Returns `GCOMP_ERR_CORRUPT`. |

Block type 3 (reserved) is explicitly rejected in the block/frame parsing path.

## Compressed blocks

- **Literals section:** Raw, RLE, and Huffman-compressed formats implemented. Treeless_Compressed and repeat-mode tables are supported. Code paths: `zstd_literals.c`, `zstd_huf.c`.
- **Sequences section:** FSE-encoded sequences with predefined and custom tables; repeat offsets and state updates. Code paths: `zstd_sequences.c`, `zstd_fse.c`.

Spec edge cases (e.g. empty literals, single-symbol Huffman) are handled; invalid or truncated data returns `GCOMP_ERR_CORRUPT` where appropriate.

## Dictionary

- **Format:** Dictionary format is parsed per RFC 8878 §5 (see `zstd_dict.c`). Parsed data is used to prime the decoder/encoder when `zstd.dictionary` is set.
- **Use:** Decoder uses dictionary for initial window/context; encoder can write with dictionary. Dictionary ID in the frame is optional and validated when present.
- **Limitation:** Full dictionary training (building dictionaries from samples) is out of scope; the library accepts pre-built dictionary bytes via options.

## Edge cases

| Case | Behavior |
|------|----------|
| Empty blocks | Handled; no output produced. |
| Maximum window size (2^31 bytes) | Capped by `limits.max_window_bytes`; decoder rejects excess. |
| Content size 0 | Allowed when present in header; validated. |
| Checksum absent | Decoder skips validation. |
| Checksum present | Decoder validates; mismatch → `GCOMP_ERR_CORRUPT`. |

## Intentionally unsupported / limits

- **Long-distance matching:** Window size is limited by options and platform; maximum per spec (2 GB) may be constrained by `limits.max_window_bytes` and memory.
- **Experimental or future frame flags:** Unknown descriptor bits are rejected.
- **Dictionary training:** Not implemented; only pre-built dictionary bytes are supported.

## Gaps and follow-up

- None currently. Reserved bits and block type 3 are rejected. If new spec errata or extensions appear, they should be reviewed and either implemented or explicitly rejected and documented here.
