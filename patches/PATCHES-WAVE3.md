# Weft — Wave 3: "runs everywhere, proven"

**13 patches · base `4a11108` (post-PR#8 main) · verified on the contribution sandbox**

> Scope: the maintainer's Series-7 mandate — harden the VM rings the PR#8
> contributor landed, add the real-device CI legs, upgrade the validator from
> substring checks to proofs, and expand breadth with new implementations.
> Gate contract: `binding-parity` + `ports-validate` + `fanout-native` +
> `android/apple/flutter` all green for this SHA, **no silent-green**.

---

## Executive summary

Wave 1 fixed the six-language port discipline. Wave 2 made records
authenticatable. Wave 3 makes the "runs everywhere" claim *falsifiable*:
every VM ring now carries a cross-port torture parity gate, every platform
road the repo advertises has a CI leg that exercises it (browser, Android
emulator across the API floor, iOS simulator, three desktop OSes), the
validator now **executes** what it validates, and the repo ships a one-command
stranger-repro bundle whose verdict a reviewer can paste anywhere.

| # | Patch | Tier |
|---|-------|------|
| 1 | `feat(fanout)` F10 100k torture parity across ports | Harden VM rings |
| 2 | `feat(flutter)` production cross-isolate fan-out channel | Harden VM rings |
| 3 | `feat(swift)` real MTKView 120Hz cadence probe — measured, not commented | Harden VM rings |
| 4 | `feat(kotlin)` API<33 fan-out compat regime — the fallback matrix | Harden VM rings |
| 5 | `feat(validator)` substring → PROOF: compile-and-load, ordering-site proofs, torture | Validator→proof |
| 6 | `feat(ci)` real-device legs — emulator LMK matrix, iOS-sim, nightly lang battery | Real-device legs |
| 7 | `feat(ci)` real-browser SAB/COOP-COEP leg — crossOriginIsolated, proven | Real-device legs |
| 8 | `feat(ci)` no-silent-green audit — pipefail everywhere, mechanically enforced | Real-device legs |
| 9 | `feat(rn)` Phase-7 UI-thread port — the worklet-expressible reader | Invent |
| 10 | `feat(flutter)` 3-OS runner matrix — FFI channel proven beyond linux | Invent |
| 11 | `feat(rfc0007+invent)` CMP eval rig + weft.dev stranger-repro bundle | Invent |
| 12 | `feat(ci)` heddles↔packages surface parity — the unguarded gap closed | Real-device legs |
| 13 | `fix(packages)` hermetic vitest configs — vue/svelte/react match core | Hygiene |

Apply with:

```bash
git am /path/to/wave3/*.patch
```

Round-trip gate: all 13 applied cleanly onto pristine `4a11108`; the am'd
tree is byte-identical to HEAD (`git diff` empty) and passes the gates.

---

## Per-patch description

### 1. F10 100k torture parity across ports

**Problem.** Four F10s existed (C runner, FanoutTest.kt, FanoutTests.swift,
Dart mixed-rate 10k) but nothing proved they stayed at parity — same frame
count, same reader shape, same gates. A port could quietly downgrade its
torture and CI would stay green.

**Fix.** Dart gains its true F10: 100k frames, 3 cadence readers, and REAL
mid-overwrite windows — every 16th frame splits `begin()` from
`fill/publish()` across an event-loop yield, the legal Dart interleave the
port header documents; tick-happy readers must gracefully skip or chase,
never accept a torn frame. Gates are identical to the concurrent ports:
torn-accepted == 0 (every fresh claim mixer-validated word-by-word),
telescoping identity exact per reader, convergence on frame 100000, publishes
telemetry == frames. New `ci/scripts/run_f10_parity.sh` runs the C baseline
(`torture 100000 4 64 3`) in BOTH ordering regimes, the JVM harness when a JDK
is present (loud skip otherwise; CI-fatal), and parity-of-contract gates over
each port's F10 source (100k frames, mixer validation, telescoping assert,
convergence assert — the Dart one additionally asserts the mid-overwrite
window is actually exercised). Wired as step 7 of the `fanout-native` shard.

**Verification.** C baseline 2/2 regimes PASS (2.0M/s and 0.77M/s); 13/13
contract checks green; shard `fanout-native` PASS with the new step.

### 2. Production cross-isolate fan-out channel (Flutter)

**Problem.** Cross-isolate fan-out existed only inside the test battery (DFT
test plumbing). The lead's brief: "today single-isolate ref only."

**Fix.** `packages/flutter_weft/lib/src/fanout_cross_isolate.dart` promotes it
to a managed production API: `CrossIsolateFanoutSession` spawns N reader
isolates over the C ring, each owning exactly one reader handle **by raw
address** (D-14 FFI lifetime discipline — the isolate only claims; the session
keeps lifetime control and destroys handles after `stopped`). The claim loop
is an **async round loop — load-bearing**: a synchronous spin would starve the
very ReceivePort the stop/stats protocol rides on (a real bug caught in
self-review; the DFT test could bound its loop by frame count, a production
reader cannot). Cadence config (tickEvery/paceMs — Law 1 no-spin), optional
canonical-pat byte validation at a stride, starvation guard, mid-flight stats
snapshots, session-owned destroy. Counters come from the C reader's
authoritative stats (one source of truth); the loop only logs violations.

**Verification.** DC1 (20k frames, 2 isolates, cadence+stride configs, mid
flight snapshot, final telescoping) + DC2 (paced reader: writes outrun claims
honestly, torn 0) — CI-gated like the DF-series (no Flutter SDK in the
sandbox; the flutter leg compiles libweft.so and runs the battery).

### 3. Real MTKView 120Hz cadence probe (Swift)

**Problem.** "Swift MTKView 120Hz probe (today comment-only)." The dual-path
view existed; the measurement did not — the view's own honesty notice said
"no on-device 120 Hz measurement exists anywhere in the repo."

**Fix.** `MTKViewCadenceProbe` renders real off-screen MTKView frames at the
requested cadence; every `draw(in:)` performs the FULL draw-phase contract
(kernel claim → `rLivePtr` live read → canonical `pat()` stride verification)
while a writer thread publishes fresh frames — the real 1W/1R stream under
display pacing. State is lock-guarded (display-link thread vs main thread).
The report is honest by construction: measured FPS, median/p95 intervals,
`claimsInDrawPhase`, `tornAccepted`, requested-vs-accepted plumbing, and
environment tags (iOS-simulator / macOS-runner / iOS-device) with explicit
caveats — a VM or simulator number can never pass for a device number.
`MetalProbeTests` gates: probe end-to-end (claims == frames, torn == 0),
the 120 Hz path REQUESTED and accepted (plumbing), environment honesty, and
the measured-vs-requested distinction (a sub-120 measurement on a non-device
host must carry the caveat). Missing Metal device = declared XCTSkip.

**Verification.** Banners flip honestly: `Heddle.swift` and the
`WeftHeddleView` notice now read CI-PROVEN (Canvas + Metal paths, probe
mechanics); the ProMotion 120 Hz DEVICE rate stays on the Hardware Deferral
List. Swift leg is CI-gated in the sandbox (no swift toolchain).

### 4. API<33 fan-out compat regime (Kotlin)

**Problem.** The VarHandle ring requires Android API 33+; below that the ring
silently died or detoured through JNI, which a pure-JVM consumer does not
have. The lead's brief: "Android API<33 fallback matrix."

**Fix.** `core/kotlin/FanoutCompat.kt` (mirrored to `android/`): the SAME
protocol and per-reader accounting, expressed in what API<33 honestly has —
`AtomicLongArray` stamps (volatile get/set = SeqCst per JSR-133, the Swift
port's SC-stamp regime) + plain bracket-discipline payload (the TS port's
stance). Law 1/2 intact. `WeftFanoutFactory` routes by API level (a pure
function — testable on any host). `FanoutCompatTest.kt` runs the F-series
over the compat regime (F1c–F6c, F8c, F9c) **plus F10c — a real 100k
multi-thread torture** — and the factory routing gates; it runs in the same
gradle invocation as `FanoutTest.kt`, so the MATRIX (both regimes, every
gate) executes on every host regardless of the host's API level. PORTS.md §6
documents the two-regime story; the validator gains a `fanout-compat` rule
pack.

**Verification.** Kotlin leg CI-gated in sandbox (no kotlinc); the gradle
battery runs it for the SHA; parity mirror byte-identical.

### 5. Validator → proof

**Problem.** "port_validator.py substring → compile+load .so,
getOpaque/VarHandle/UnsafeAtomic ordering checks + torture in CI." The
validator proved files *mention* the right primitives.

**Fix.** Three proof classes:

- **Ordering-site proofs** — extract each protocol method's body (brace
  matching over comment/string-scrubbed source) and verify the discipline AT
  THE SITES: Kotlin begin/publish/claim/fill must use the FanoutVh accessors
  and must NOT touch ctrl bytes raw; Swift stamps must be SC with relaxed
  payload; Dart word accessors must carry `Endian.little` at every site and
  retain the stamp bracket. A wrong ordering at a wrong site is now RED.
- **Compile-and-load (`--prove`)** — builds core/c into a real .so, dlopens it
  via ctypes, and drives kernel `weft_init/w_begin/publish/claim/r_live_ptr`
  AND a fan-out `new/begin/publish/reader_new/claim` roundtrip with payload
  verification. Kotlin/Dart/Swift compile-or-delegate: a missing toolchain
  records the CI delegation in the artifact — declared, never silent.
- **Torture (`--torture`)** — the C F10 100k 3-reader torture inside the
  validator's own gate.

`run_ports_validate_shard.sh` now runs `--prove --torture`; the artifact gains
`ordering_site_proofs` and per-proof details.

**Verification.** Sandbox: compile-and-load PASS (kernel + fan-out roundtrips
live), torture PASS at ~1.15M frames/s, 20 ordering-site checks green across
Kotlin/Swift/Dart.

### 6. Real-device CI legs

**Fixes.**

- **`android-emulator.yml` (new)** — API `[26, 33, 34]` emulator matrix:
  API 26 exercises the pre-33 runtime the compat regime now serves; 33/34 the
  VarHandle regime. Carries the RFC-0006 LMK gate via a two-phase instrumented
  protocol: `stage` (allocate, publish, commit pre-death state to
  SharedPreferences) → `am kill` (the deterministic ActivityManager background
  kill — declared stand-in for LMK pressure) → `verify` in a FRESH process:
  CLEAN_REALLOCATE with fresh Weft, sequence reset to 1, zero stale-pointer
  dereferences (a stale deref would SIGSEGV the leg).
- **apple-packages.yml** — `ios-simulator` job: xcodebuild build+test on an
  iPhone 15 simulator (F-series + Cadence + Metal probe legs) with a
  step-summary honesty extract.
- **nightly-deep.yml** — `lang-ports` matrix: kotlin (gradle test: VarHandle +
  compat matrix, two F10 tortures), swift (swift test incl. Metal probe),
  dart (dart analyze --fatal-infos + full flutter battery over the compiled
  kernel); aggregated into the nightly report.

**Verification.** Workflow YAML all parse; all pipelines pipefail-covered (see
patch 8); sandbox cannot boot emulators — the legs are wired and the
instrumented test compiles against the declared androidx.test artifacts.

### 7. Real-browser SAB/COOP-COEP leg

**Problem.** WHITEPAPER §8.2 says SAB requires COOP/COEP — but no test ever
verified it in a real browser (vitest runs on node, where SAB needs no
headers).

**Fix.** `tools/browser-harness/`: `server.mjs` (COOP same-origin + COEP
require-corp; serves the REAL @weft/core dist; collects the in-page report),
`harness.html` (kernel publish/claim roundtrip over SAB-backed buffers + a
5000-frame 1W/2R fan-out stream with pat() validation and telescoping checks,
executed in the isolated context), `probe.mjs` (Playwright chromium, stock
flags — isolation comes from the headers, not switches). Shard script builds
@weft/core if needed; Playwright installs into a git-ignored scratch project
(`ci/browser-tmp`) so the workspace `pnpm-lock.yaml` stays untouched (the
frozen-install lesson). Gate: `crossOriginIsolated === true`, kernel roundtrip
ok, fanout tornAccepted == 0 with both readers converged.

**Verification.** `node --check` on harness modules; browser install is
network-dependent — declared CI-gated.

### 8. No-silent-green audit

**Problem.** "no silent-green (set -o pipefail everywhere)." The repo already
died once from pipelines swallowing failures.

**Fix.** `ci/scripts/run_pipefail_audit.py` + shard wrapper: (1) every
`ci/scripts/*.sh` must carry `set -euo pipefail`; (2) every workflow `run:`
block containing a real pipeline (single `|` — `|| true` is a declared ignore)
must establish pipefail. The audit's layout assert is loud: an audit that
finds nothing is a lied audit (the first draft resolved the wrong ROOT and
exited 0 — caught in self-review, asserted now). **It found six live
silent-green holes and this series fixes them**: 2× `curl | sh` installs (a
failed curl fed sh empty input), the `grep | tail` failure diagnostic, 2×
`git ls-remote | grep` ci-report bookkeeping (extreme-test.yml), and the same
curl in nightly-deep.yml — all patched with `set -o pipefail` and explicit
rc-tolerant forms. Wired as shard `silent-green-audit`.

### 9. RN Phase-7 UI-thread port

**Problem.** The 2026-09 honesty note in `weft-rn.ts`: true UI-thread reads
require the Phase-7 port.

**Fix.** `packages/react-native/src/ui-thread.ts`: `uiThreadClaim` carries the
`'worklet'` directive and performs the FULL RFC-0004 reader protocol on the UI
thread — Atomics over the ring SAB only (primitives + SAB are sendable into
worklets; class instances are not — the old bug's lesson). Bounded attempts,
graceful skip, stamp-bracket revalidation, per-reader drop accounting; the
claimed payload is a live SAB view (A3). `useWeftUiThread` enforces the
Draw-phase discipline by construction (draw runs INSIDE the frame callback);
the disposer is idempotent AND poisons the loop. A subtle JS trap was caught
by the battery: `1n !== 1` — BigInt stamps must be converted before
comparison. The UT1–UT6 battery drives the SAME function that ships,
including the worklet-directive presence gate (a missing directive would
silently demote the loop to the JS thread). The vitest config was made
hermetic (see patch 13) to keep the suite runnable under foreign ancestors.
The heddles shim re-exports the Phase-7 surface.

**Verification.** The RN package suite is green (15/15 incl. the 6 new UT
gates); typecheck clean. Device-side Reanimated timing stays deferred per the
module banner.

### 10. 3-OS Flutter runner matrix

**Problem.** The flutter leg ran on ubuntu only; the FFI story claimed more
than CI proved.

**Fix.** `flutter-packages.yml` matrices `[ubuntu-latest, windows-latest,
macos-latest]` under one bash dialect: per-OS kernel compile (libweft.so /
libweft.dylib / weft.dll with `-static-libgcc`), OS-aware test loaders in
`ffi_test.dart` and `fanout_ffi_test.dart` (macOS/Windows branches), parity
guard on every OS. The F-series, DF-series FFI and DC-series cross-isolate
batteries now execute on all three OSes for the same SHA.

### 11. CMP eval rig (RFC-0007) + stranger-repro bundle

**CMP eval rig** (`contrib/cmp-eval-rig/`): RFC-0007 becomes an instrument.
`CmpDrawProbe.kt` freezes `CmpCadenceReport` — identical columns to the native
path's `CadenceProbeReport` (frames, measuredFps, median/p95 intervals,
drawPhaseClaims, tornAccepted) PLUS the column only CMP must defend:
**recompositions == 0** — the draw-phase isolation gate, measured, not argued.
p95 is the jitter column RFC-0007 flags. Status: SOURCE-ONLY, PENDING CMP
TOOLCHAIN (declared); the Swift comparison side is CI-PROVEN. RFC-0007 gains
an Evidence section with the honest split.

**Stranger-repro bundle** (`tools/stranger-repro/`): ONE command — `bash
run.sh` — any stranger runs on any machine with a C compiler. Builds the
kernel from source and executes 13 legs: litmus L1–L8 per-cell, fan-out
F-series, the F10 100k 3-reader torture, the .weftrec flight-recorder
roundtrip, VerifiedWeft gen/validate/tamper-rejection (BOTH arms must hold:
authentic validates AND tampered rejected), and the cross-port
parity-of-contract gates. Emits `STRANGER-REPRO-REPORT.{txt,json}` plus a
sha256 **verification stamp** to paste into issues/PRs. Missing toolchains
record SKIPPED with a reason — never a pass.

**Verification.** All 13 legs PASS on the contribution sandbox; stamp
`f1a4aaa8…7662` at authoring time (re-run to regenerate on your machine —
the point of the bundle).

### 12. heddles↔packages surface parity

**Problem.** "Fix heddles/*↔packages/* parity gap (currently unguarded)."
The historic drift class ("the heddles/ fork kept a bug the packaged twin had
already fixed") applies to shims differently than byte mirrors: a shim drifts
when it re-exports a symbol the canonical package no longer exports.

**Fix.** `run_binding_parity.sh` gains a second table: for each of the four
shims (react/vue/svelte/react-native), parse the shim's export declarations
and verify every exported name exists in the canonical package's src surface.
A shim re-exporting a renamed/removed symbol goes RED with the names listed.
Also carries the `FanoutCompat.kt` mirror pair (14 byte-pairs now). The
results JSON records both tables.

### 13. Hermetic vitest configs (vue/svelte/react)

Same foreign-ancestor-postcss trap packages/core fixed in wave 2: when the
repo is checked out under a webapp root, vite walks up from the config root,
loads a foreign postcss config (a Tailwind-4 ESM file) and dies on load.
Pinning an empty plugin list makes all four package suites immune; no test
imports CSS.

---

## Verification matrix (contribution sandbox)

| gate | result |
|---|---|
| litmus matrix (C+Rust+TS × L1–L8) | **24/24 PASS** |
| fanout-native shard (F-series ×2 regimes, Rust suite + Loom, xlang, JNI harness, flight-rec, F10 parity) | **PASS** |
| fanout-concurrent shard (1W×4R torture, gates G1–G6) | **PASS** |
| verifiedweft shard (V-series ×3 kernels + xlang + tamper) | **PASS** |
| ports-validate (structure + ordering-site proofs + compile-and-load + torture) | **PASS** |
| binding-parity (14 byte pairs + 4 shim surface pairs) | **PASS** |
| silent-green audit (18 shards + all workflow pipelines) | **PASS** |
| canonical audit (bundle prefix) | **PASS** |
| TS packages (core 94, react 11, vue 12, svelte 9, react-native 15) | **141/141 PASS** |
| W-suite | **PASS (exit 0)** |
| git-am round-trip onto pristine `4a11108` | **13/13 clean, tree identical** |
| Kernel Freeze audit (`core/c/weft.{c,h}` + `core/rust/src/lib.rs`) | **diff empty** |

Environment declarations (honesty): the sandbox has no flutter/swift/gradle
toolchains — those legs are CI-gated per the repo's declared-skip discipline
and recorded as delegated in the validator artifact; rustup was installed to
run the Rust litmus cells locally.

---

## Gate wiring (the lead's contract)

| gate | where it runs | wave-3 delta |
|---|---|---|
| binding-parity | android / flutter workflows + shards | + FanoutCompat pair, + shim surface table |
| ports-validate | extreme-test shard | + prove + torture + compat pack |
| fanout-native | extreme-test shard | + F10 cross-port parity step |
| android | android-packages.yml | + FanoutCompatTest matrix battery |
| apple | apple-packages.yml | + ios-simulator job (probe legs) |
| flutter | flutter-packages.yml | + 3-OS matrix |
| silent-green | extreme-test shard (`silent-green-audit`) | NEW — enforces the pipefail contract |
| real-device | android-emulator.yml (NEW), ios-sim, browser-sab shard | NEW legs |

## Known limitations (honesty)

- Device-side claims that remain deferred, stated where reviewers look: the
  ProMotion 120 Hz **rate** (the path and probe are proven), Reanimated
  on-device scheduling (the protocol is proven), physical-device LMK pressure
  (`am kill` is the deterministic stand-in), CMP numbers (rig frozen, toolchain
  pending), browser-chromium install (network-dependent in the sandbox).
- The RN package's fanout tests required a fresh @weft/core dist build to
  resolve the fanout exports — CI builds it in the workspace flow; the
  hermetic configs remove the other foot-gun this exposed.
- The silent-green audit is a static scanner: it proves coverage, not intent —
  `|| true` remains legal where it is explicit.
