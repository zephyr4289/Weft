# xlang-verifiedweft — RFC 0005 cross-language interop gate

Byte-compat contract between the three VerifiedWeft kernels (C, Rust via
the shared record format, TS). All three must produce and consume the same
authenticated frame records:

```
[envelope v1 16B | payload | HMAC-SHA256 tag 32B]
auth_key = HMAC-SHA256(secret, "Weft-VerifiedWeft-v1:key")
tag      = HMAC(auth_key, envelope || payload)
```

## What runs

`bash run.sh` (or `pnpm --filter fixture-xlang-verifiedweft xlang`):

1. **C → TS**: `core/c/verified-runner gen` writes a record file; `reader.mjs`
   verifies every record (tag, envelope geometry, seq, payload bit-exactness
   against the shared 04-LITMUS §0.1 mix32 generator).
2. **TS → C**: `writer.mjs` (pure-TS kernel module `core/ts/verified.ts`)
   writes the same format; `verified-runner validate` verifies everything.
3. **Negative leg**: a single-bit flip must be rejected by BOTH kernels.
   A verify that accepts a tampered record fails the gate.

## Vectors

`hmac-vectors.json` holds the primitive-level conformance vectors:
RFC 4231 test cases 1–4, 6, 7 (HMAC-SHA256) plus two Weft boundary cases
(empty payload, one-byte payload). Digests were cross-checked against
`node:crypto` at generation time and are embedded byte-identically in the
C test (`core/c/verified_test.c`) and the Rust tests
(`core/rust/src/verified.rs`).

## Requirements

- node 18+ (type stripping for the .ts import — node 22.6+/24 recommended)
- gcc; `make -C ../../core/c verified-runner`
