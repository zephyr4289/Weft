# fixtures/xlang-blend — cross-language Q12 blend parity gate (Series 8)

The cadence raster op (`out_c = (prev_c*(4096−alpha) + newest_c*alpha) >> 12`
per 8-bit channel of each packed RGBA8888 word) exists in five
implementations that MUST agree bit-for-bit:

| Implementation | Role | Tier |
|---|---|---|
| `core/c/blend_q12.c` | **Golden source** — scalar reference + SSE4.1/AVX2/NEON with runtime dispatch | native (Android NDK, Flutter FFI) |
| `packages/core/src/blend.ts` | reference port | JS/TS (parity + web) |
| `core/kotlin/BlendQ12.kt` | port (mirrored to `android/weft-core/.../dev/weft/`) | JVM |
| `core/swift/BlendQ12.swift` | port — stdlib `SIMD4<UInt32>` (NEON on arm64) | Apple |
| `core/dart/blend_q12.dart` | port (+ optional FFI hook to the C kernel) | Dart VM / Flutter |

## Mechanism

`blend_test.c --digests` replays a deterministic (size, alpha) grid —
per row: seed 0x5EEDBEEF, two xorshift32 draws per word over
`words + 8` guard words (prev then newest) — blends with the scalar
reference, and emits `size,alpha,fnv1a64` rows into
`golden-vectors.csv` (committed). Every port replays the same
construction through its own blend and must reproduce the same FNV-1a-64
digests. Any divergence in channel order, shift depth, rounding, or
endianness is a hard byte mismatch.

```csv
0,0,cbf29ce484222325          <- empty blend = the FNV basis
4097,4095,c50c5bb6bd061ba6
4097,4096,41a534317ea69560    <- alpha 4096 reproduces newest exactly
...
```

`run.sh` executes every leg whose toolchain is present and DECLARED-SKIPs
the rest (the repo's honesty pattern):

| Leg | Toolchain | Sandbox | CI owner |
|---|---|---|---|
| C kernel selftest + drift check | gcc | GREEN | simd-blend shard |
| TS verifier | node + @weft/core dist | GREEN (63/63) | simd-blend shard |
| Kotlin verifier | kotlinc + java | GREEN (63/63) | android-packages leg |
| Dart verifier | dart | GREEN (63/63) | flutter-packages leg |
| Swift port + digests | swiftc | DECLARED skip | apple-packages leg |

The drift check inside run.sh re-runs the kernel's digest mode and
compares against the committed CSV — editing one side of the contract
(the kernel arithmetic OR the CSV alone) is RED, never silent.

## The overflow argument (why the lane math is exact)

Per channel the numerator `prev_c*inv + newest_c*alpha` is at most
`255*4096 + 255*4096 = 2,090,880 < 2^21`. The SSE/AVX paths widen each
channel into a 32-bit lane before multiplying (16-bit madd sublanes
whose partner is zero — products ≤ 2^20, no signed overflow, no
cross-lane carry); the NEON and Swift paths do the same via vmovl /
SIMD4<UInt32>; the JVM/Dart tiers keep the scalar per-channel form. All
of them therefore compute the EXACT scalar value — the 154-check parity
sweep and the full 0..4096 alpha sweep in blend_test prove it on every
build.

## Performance (measured, x86_64 sandbox — blend_runner)

| Size | scalar | AVX2 | speedup |
|---|---|---|---|
| 786 KB hot (the consumer's regime) | 194 µs | 23 µs | **8.41x** |
| 1080p | 6.46 ms | 1.14 ms | 5.64x (21.7 GB/s) |
| 4K | 25.2 ms | 4.79 ms | 5.26x (20.8 GB/s) |
| 8K | 103 ms | 28.3 ms | 3.64x (14.1 GB/s) |

4K/8K streaming is DRAM-bound (~4 GB/s on this sandbox); sub-50 µs 4K
synthesis is the GPU ring's domain (RFC-0003). CPU SIMD clears
1080p/144 Hz with 7x headroom and makes the consumer's hot 786 KB tile
blend a rounding error in the frame budget.
