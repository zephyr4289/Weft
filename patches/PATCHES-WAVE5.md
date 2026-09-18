# PATCHES-WAVE5.md — Series 8: Predictive Cadence, SIMD Blend, RT QoS & Native Worklets

**Base:** `6a9a4db` (post-PR #13/#14 main) · **Branch:** `contrib/series8-depth-cycle` · **Patches:** 7 (6 code + 1 docs)

## Executive summary

The lead's Series-8 cycle asked for depth in four areas — SIMD-accelerated
cadence blending, real-time render-thread QoS, predictive cadence with
clock-drift compensation, and zero-overhead native worklets — under two
hard guardrails (the 23/23 binding parity and the 24-shard green matrix).
This series delivers all four as ONE architecture, not four hacks:

1. **RFC-0012 `PREDICTIVE_PACED`** — the fourth cadence policy (exactly
   the escape hatch RFC-0009's closed set provided for): a fractional Q16
   phase accumulator driven by a gap-EWMA + MAD-jitter filter, with a
   reactive fallback gate. Kills the 3:2-pulldown judder on non-integer
   content beats (30 Hz on 144 Hz = 2.4 ticks) and stays **tick-for-tick
   identical to PACED_INTERPOLATE on integer beats** (PC8, g=1..12).
2. **The Q12 raster blend as one spec'd op, SIMD-tiered** — C kernel
   (SSE4.1/AVX2/NEON, runtime dispatch) + TS/Kotlin/Swift/Dart ports,
   all byte-identical to the C golden digests (63/63 × 4 ports).
   **Measured: 8.41× on the consumer's hot regime, 5.26× at 4K.**
3. **Thread QoS + native worklets** — `weft_thread_apply_qos` with
   flags-not-silence semantics + `weft_worklet` (semaphore-acked SPSC
   pump, zero alloc per tick, 20,000/20,000 ticks proven, p50 latency
   sub-microsecond) + ports on all three runtimes.
4. **Guardrails exceeded, not just kept** — parity 23 → **27/27** pairs;
   matrix 24 → **26 shards**; litmus **24/24**; kernel freeze intact;
   silent-green audit green; PC3 protocol extended in place with the v1
   substream byte-preserved.

## Patch-by-patch

### 1. `feat(rfc,core-ts): RFC-0012 PREDICTIVE_PACED — the Q16 phase-accumulator cadence policy`

- `rfcs/0012-predictive-cadence.md`: full spec. State = gapQ16/varQ16
  (Q16 EWMA gain 1/4, split-division discipline) + phaseQ16/phaseRem
  (full-precision accumulator) + haveGap/predLastArrivalTick. Arrival
  ticks reuse PACED's window bookkeeping verbatim; non-arrival ticks
  advance the phase by `ONE_Q16²/gapQ16` **plus the division-remainder
  carry** — truncating the increment alone loses fractional bits and
  breaks integer equivalence (g=6, j=3 gives 2047 vs PACED's 2048; the
  carry is the fix, and the finding is documented in the RFC).
- Reactive gate: `varQ16*4 > gapQ16` degrades the tick to PACED's
  integer rule, counted in `reactiveTicks` — never silent.
- `packages/core/src/cadence.ts`: kind **3** (PROTOCOL), zero-alloc,
  identity-stable decision record, same elision key and Law-4
  telescoping as PACED. Warmup = "no gap KNOWN" (`gapQ16 <= 0`), which
  includes the post-first-arrival window whose filter state is still
  zero — the first implementation draft got this wrong (div-by-zero →
  NaN carry); the battery caught it.
- `packages/core/test/cadence-predictive.test.ts`: PC7a-c, PC8, PC9
  (+ the PACED-contrast proving the gate separates the policies), PC10,
  PC2/PC4/PC6 re-pins, stall honesty, PC11 FNV pin. **Bounds are
  measured, not aspirational** (the orbit probe in the commit history):
  gap EWMA converges into ±0.125 ticks of the 12/5 beat; ≤ 10 saturated
  holds per 10k ticks; every crafted-late window holds exactly once and
  no non-late window does.

### 2. `feat(core): PREDICTIVE_PACED VM ports + PC3 protocol v2 — 4-policy byte parity`

- Ports: `core/kotlin/Governor.kt` (+ android mirror, byte-identical),
  `core/swift/Governor.swift`, `core/dart/governor.dart` (+ flutter
  mirror) — arithmetic-identical, split-division truncation preserved.
- PC3 v2 (`fixtures/xlang-cadence/`): stream extends to 8 bytes/tick,
  kind order 0,1,2,3; v1's 3-policy substream is byte-preserved (ports
  0-2 arithmetic untouched — the old pin still validates it).
- **Sandbox-verified byte parity**: TS 160,001 bytes == Kotlin (kotlinc
  2.0.21 + OpenJDK 21) == Dart (3.5.4 VM); Swift = declared skip
  locally, apple-packages owns it.
- New pins (`scripts/gen_trace_refs.mjs`): LADDER unchanged
  `0x3c33156204c7cfdf`; CADENCE v2 `0x12eed7eec11b6a57` (80,000 bytes);
  PREDICTIVE-only `0xa6b942ef48046e0e`.
- VM batteries (Kotlin gradle / Swift XCTest / Dart flutter legs) pin
  the v2 hash and gain PC7/PC10 gates of their own.

### 3. `feat(core/c): SIMD Q12 raster blend — SSE4.1/AVX2/NEON with runtime dispatch`

- `core/c/blend_q12.{h,c}`: the raster op
  `out_c = (prev_c*(4096−alpha) + newest_c*alpha) >> 12` per RGBA8888
  word — scalar reference (the spec) + per-target intrinsics. The lane
  plan widens each channel into a 32-bit lane before multiplying
  (16-bit madd sublanes whose partner is zero): products ≤ 2^20, no
  overflow, no cross-lane carry — **exact scalar values by
  construction**, proven by 154 parity checks + the full 0..4096 alpha
  sweep + boundary + in-place-aliasing gates on every build.
- **Bug caught by the benchmark, not by review**: the AVX2 path was
  guarded by `#ifdef __AVX2__` (a `-mavx2` compile flag nobody sets) so
  dispatch silently ran scalar while printing "dispatch=avx2". The
  target attribute alone is the contract; removing the guard took the
  bench from 1.0× to the measured numbers below.
- `blend_runner.c` (sandbox, x86_64/AVX2):

  | Size | scalar | AVX2 | speedup |
  |---|---|---|---|
  | 786 KB hot (the consumer's regime) | 194 µs | **23 µs** | **8.41×** |
  | 1080p | 6.46 ms | 1.14 ms | 5.64× (21.7 GB/s) |
  | 4K | 25.2 ms | 4.79 ms | 5.26× (20.8 GB/s) |
  | 8K | 103 ms | 28.3 ms | 3.64× (14.1 GB/s) |

  The governed consumer's two-frame history is re-read every tick as
  alpha advances — the hot row is the honest headline. 4K/8K streaming
  is DRAM-bound here (~4 GB/s sandbox); sub-50 µs 4K synthesis is the
  GPU ring's domain (RFC-0003), and CPU SIMD clears 1080p/144 Hz with 7×
  headroom.

### 4. `feat(blend ports): Q12 blend in 4 languages + xlang-blend golden gate`

- `packages/core/src/blend.ts` (exported from `@weft/core`) +
  `core/kotlin/BlendQ12.kt` (+ mirror) + `core/dart/blend_q12.dart`
  (+ mirror, with the documented optional FFI seam to the C kernel) +
  `core/swift/BlendQ12.swift` (stdlib `SIMD4<UInt32>` → NEON on arm64).
- Consumer delegation: `GovernedFanoutConsumer.kt/.swift` and
  `governed_consumer.dart` drop their private hand-rolled blends and
  call the shared op — one spec, one test surface.
- `fixtures/xlang-blend/`: `blend_test --digests` emits the committed
  CSV (per-row reseeding — the first gate run caught cross-row state
  chains making the fixture unreproducible; fixed in the kernel); every
  port replays the identical xorshift32 pair-fill and byte-compares all
  63 digests. The drift check re-runs the kernel against the committed
  CSV so NEITHER side can move alone (and it caught its own author once:
  the first draft's `cmp` ran in a subshell whose relative path resolved
  to nothing — comparing against nothing is not parity).
- Sandbox verdict: C gates GREEN; **TS 63/63, Kotlin 63/63, Dart 63/63**;
  Swift declared skip (Apple CI).

### 5. `feat(core/c): thread QoS + the native render worklet`

- `thread_qos.{h,c}`: `weft_thread_apply_qos` — per-thread affinity +
  scheduling class returning FLAGS, not silence (applied / refused /
  declared-fallback / partial / unsupported). Big-core preference parsed
  from cpufreq (abstains on homogeneous topologies). Measured here: an
  unprivileged SCHED_FIFO attempt yields `WEFT_QOS_SCHED_UNPRIVILEGED`
  exactly as declared.
- `worklet.{h,c}`: the consumer loop OWNED by the runtime — dedicated
  QOS-hardened thread, semaphore-acked SPSC handoff, zero allocation
  after spawn, no locks.
- `worklet_test.c` (GREEN): W1 20,000/20,000 ticks; W2 every ordinal
  exactly once; W3 spawn flags honest; W4 inter-tick latency p50
  sub-µs / p99 0.8 µs / max 12.4 µs (jitter measured, not hidden); W5
  the governed blend loop on the worklet thread: 35 µs/frame at
  256×256 (22.7 GB/s).
- `qos_test.c` (GREEN): mask sanity, preset honesty, RT declared
  fallback, single-core affinity apply + readback.

### 6. `feat(qos ports): RenderQos/WeftWorklet for Kotlin, Swift, Dart`

- Kotlin (+ mirror): `RenderQos.bigCoreMask` (pure-Kotlin cpufreq
  parser), `applyRenderQos` (android `Process.setThreadPriority(-8)`
  when present / `Thread.MAX_PRIORITY` on plain JVM — both counted;
  `Os.sched_setaffinity` guarded API 26+), `WeftWorklet` (latch-honest
  spawn flags — the first draft raced; fixed before commit).
  **Sandbox-proven on the JVM**: 20,000/20,000 ordered worklet ticks,
  184.9 µs/frame governed blend — quantifying the gap the C/AVX2 tier
  closes (5.3×).
- Swift: `QOS_CLASS_USER_INTERACTIVE` via the Thread API (the mechanism
  iOS actually honors), Mach time-constraint seatbelted behind
  `allowTimeConstraint` (default false — hard RT without a measured
  rationale is a footgun), `WeftWorklet` with a DispatchSemaphore pump.
- Dart (+ mirror): FFI seam to `weft_thread_apply_qos` with the
  pure-Dart `UNSUPPORTED` fallback (declared, not silent); the pump
  contract documents the vsync seam honestly. `dart analyze`: clean.

### 7. `feat(ci): Series-8 gates` (this commit ships the doc)

- Two new shards (matrix 24 → 26): `simd-blend` and `qos-worklet`.
- `run_binding_parity.sh`: 23 → **27 pairs** (every new core file
  mirrored from day one); heddles surface parity 4/4 untouched.
- `port_validator.py` kotlinc proof compiles `BlendQ12.kt` +
  `RenderQos.kt` alongside the existing ten files.
- Both shard scripts: `set -euo pipefail`; the silent-green audit
  re-run covers them.

## Guardrail ledger

| Guardrail | Before | After | How verified |
|---|---|---|---|
| Binding parity | 23/23 | **27/27** | run_binding_parity.sh (this sandbox) |
| Heddles surface parity | 4/4 | 4/4 (untouched) | run_binding_parity.sh |
| Litmus matrix | 24/24 | **24/24** | C 8/8 + Rust 8/8 + TS 8/8 (this sandbox) |
| CI shard matrix | 24 shards | **26 shards** | extreme-test.yml (YAML-validated) |
| Kernel freeze | intact | **intact** | canonical-audit shard PASS |
| No silent green | enforced | enforced (+2 shards) | silent-green-audit PASS |
| PC3 cadence parity | 3 policies, 4 ports | **4 policies**, byte-preserved v1 | xlang-cadence (TS==Kotlin==Dart) |
| NEW: blend parity | — | 5 impls, 63 golden digests | xlang-blend (C==TS==Kotlin==Dart) |
| NEW: RFC-0012 | — | PC7–PC11 green | TS battery + VM batteries |

## Honesty wall (what is NOT proven here)

- **Swift**: no swiftc in this Linux sandbox — Governor.swift /
  BlendQ12.swift / RenderQoS.swift are source-verified by review and
  gold-gated by the apple-packages leg (the repo's long-standing
  declared-skip pattern; nothing here weakens it).
- **Real-device QoS**: `Process.setThreadPriority(-8)`,
  `Os.sched_setaffinity`, cpufreq cluster detection, and the Mach time
  -constraint path need a device/emulator; the JVM/CI legs prove the
  logic and the flags contract, the android-emulator / apple legs own
  the hardware claims.
- **Sub-50 µs at 4K on CPU**: memory physics says no on this sandbox
  (~4 GB/s DRAM); the measured claim is 8.41× hot / 5.26× 4K and the
  GPU ring (RFC-0003) is the 4K/8K-tier synthesis path.
- The reactive gate fires a bounded warmup transient (~7-10 ticks) on
  perfectly stable feeds before the MAD filter settles — bounded,
  counted, and it degrades to exactly what PACED would have done;
  documented in RFC-0012 §step, asserted by PC7.
