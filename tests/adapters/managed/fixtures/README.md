# Pillar 6 managed fixtures (frozen golden)

Regenerate: `node tests/adapters/managed/fixtures/generate.mjs --out .`
Determinism is asserted by suite stage 2 (double-run byte-identical).

| Artifact             | Purpose                                                            |
|----------------------|--------------------------------------------------------------------|
| `fintech-stream.bin` | Framed ITCH 5.0 stream (2,048 msgs: S/A/F-size/E/X/D/U/P, big refs) |
| `sbe-schema.json`    | SBE schema descriptor (templates 1001 BookRefresh / 1002 ext)      |
| `sbe-stream.bin`     | u32 LE length-framed SBE records (every 8th has extension bytes)   |
| `rng1-ring.bin`      | RNG1 ring: 8×4096B slots, 10 records (1 torn, 2 overwritten)       |
| `scenario.json`      | Message counts + SHA-256 checksums — parity ground truth           |

The ITCH stream includes order refs with `hi = 0x00C0FFEE` (> 2^53 when
materialized as a double) to force exact lo/hi u32 handling in every language.

`expected-hashes/` (added by `freeze.mjs`, consumed by parity stage) holds the
frozen MDP1 checkpoint snapshots + SHA-256 manifests that TS and Python must
reproduce bit-for-bit.
