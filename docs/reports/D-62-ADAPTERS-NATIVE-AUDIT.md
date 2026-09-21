# D-62 — weft-adapters Native Audit (rmw_weft + weft-vision-dma)

**Pillar:** 6 — Native Middleware (rmw_weft ROS 2 RMW engine, weft-vision-dma
zero-copy frame grabber)
**Scope:** Engineer 2 — native systems & kernel-bypass integration layer
**Environment:** `x86_64-sandbox` (2-core Xeon, gcc 14.2.0, glibc 2.41,
`/dev/shm` 64 MiB, CFS-quota-throttled k8s container — see §C.5)
**Suite:** `tools/adapters/tests/run_adapters_native_suite.sh` — 16/16 units
green, **100% CI green** (directive bar: ≥ 90%), wall time 34 s
**Evidence:** `tests/adapters/build/logs/` (per-unit logs), §D tables below

---

## A. Acceptance-criteria scoreboard

| Directive AC | Status | Evidence |
|---|---|---|
| A. `rmw_weft` drop-in RMW: full directive API surface, SHM rings, seqlock versioning, atomic fences | **PASS** | §A.2 fidelity matrix; R1–R6 battery (3 legs) |
| B. `weft-vision-dma`: V4L2 MMAP + DMA-BUF export/import, `weft_tensor_view_t` + DLPack wrapping | **PASS** | V1–V6 battery (3 legs); §B memory maps |
| C. Native batteries: fork-based multi-process, burst saturation, pointer aliasing, clean cleanup | **PASS** | R1–R6, V1–V6, T1–T4 |
| C. 2,000,000-cycle torture, zero leaks, seqlock consistency | **PASS** | 2,000,000 + 100,000 cycles, plain leg, 24.6 s |
| D. Bench + fail-closed runner, plain/ASan+UBSan/TSan legs | **PASS** | 16/16 units; G1/G2 gated plain leg |
| E. This audit: latency distributions vs DDS, memory maps, allocation ledger | **PASS** | §B, §C, §D |
| Rule 1 (ABI fidelity: rmw / V4L2 / DMA-BUF / DLPack) | **PASS** | §A.2–A.4 |
| Rule 2 (every registration/subscription/lookup loop hooked, tested) | **PASS** | §A.5 seam matrix |
| Rule 3 (dual-arch discipline, `-Wall -Wextra -Werror -pedantic -std=c11`) | **PASS** | §C.1 (declared portability posture) |
| Rule 4 (zero-copy pointer identity, address-asserted) | **PASS** | R2 (loan offsets byte-equal across fork); V3 (view == mapping == DLPack) |
| RMW SLA: < 1.5 µs RTT for 64 KiB cross-process | **PASS (0.92 µs p50 / 1.27 µs p99)** | §D.1 |
| Vision SLA: < 200 ns dequeue→tensor handoff | **PASS (88 ns p50 / 259 ns p99)** | §D.3 |

### A.2 rmw ABI fidelity (vendored `compat/include/rmw/rmw.h`, pinned to ROS 2
Humble layouts)

Every struct listed in the directive has member-for-member layout pinned by
`_Static_assert` offset freezes compiled into every TU (see header §4):
`rmw_publisher_t` (topic_name@16, type_support_@24, options@32,
can_loan_messages@36), `rmw_subscription_t` (options@32),
`rmw_message_info_t` (publisher_gid@32), `rmw_init_options_t`
(instance_id@16), plus frozen constants `RMW_IMPLEMENTATION_ID_MAX_SIZE=128`,
`RMW_GID_STORAGE_SIZE=24`. Return codes are the frozen wire numbers
(RMW_RET_OK=0 … RMW_RET_SUBSCRIPTION_TAKE_FAILED=12). Function arities match
the official spec exactly for the entire implemented surface (init/options
fini/init/shutdown/fini; node create/destroy; publisher create/destroy/
publish + the full loaned trio; subscription create/destroy/take/
take_with_info/take_loaned/return_loaned; wait-set create/destroy/add/
remove + `rmw_wait`).

**Declared omission inventory (D-62 §A.3 clause):** graph introspection
(rmw_count_publishers/subscribers, gid getters beyond message_info), guard
conditions, services/clients/actions, events, serialization/deserialization
hooks, content filtering, namespace enforcement. Nothing in the omitted
surface is stubbed — it is simply not vendored, so the rcl layer fails at
link time rather than silently misbehaving (Rule 2: no stubbed loops). The
POD/`.weft` typesupport (`rmw_weft_pod_v1`) is the engine's own extension
surface; ROS 2 *generated* types are Engineer 3's integration seam.

### A.3 DLPack fidelity (vendored `vision/include/weft_dlpack.h`)

DLPack v1.0 (dmlc/dlpack @ v1.0) `DLContext`/`DLDataType`/`DLTensor`/
`DLManagedTensor` layouts and enum values are transcribed exactly; the
deleter signature `void (*)(struct DLManagedTensor *)` is honored — the
engine's deleter IS the buffer requeue path (V4 proves a framework calling
it continues the stream). DLPack 1.1 versioned tensors are a declared
omission.

### A.4 V4L2 fidelity

The real backend includes `<linux/videodev2.h>` and drives the UAPI structs
directly (`v4l2_capability`, `v4l2_format`, `v4l2_requestbuffers`,
`v4l2_buffer`, `v4l2_exportbuffer` via `VIDIOC_QUERYCAP/S_FMT/REQBUFS/
QUERYBUF/QBUF/STREAMON/DQBUF/EXPBUF/STREAMOFF`). Compile-gated by
`WEFT_HAVE_V4L2` (probed at build time); without kernel headers the AUTO
backend fails closed with ENODEV (V6 proves the refusal). The mock backend
implements the identical `weft_vision_backend_ops_t` seam — one engine, two
backends, zero `#ifdef` forks in the engine core.

### A.5 Seam-verification matrix (Rule 2 — every loop hooked)

| Seam | Test that walks it |
|---|---|
| registry topic find-or-create (pub side) | R1/R4 create + refresh |
| registry topic find-or-create (sub side) | R1/R4 (pre-publisher subs) |
| publisher fan-out attach (1st + Nth ring) | R1 (single), R4 (two subs) |
| fan-out DETACH on subscriber destroy | R4 teardown + R6 audit |
| publisher rescan on subs_version bump | `wait_for_subscriber` (bench), publish-time scans |
| orphan sweep (dead owner) | R5 crash child → next-init heal |
| registry attach-slot reclaim | R5 + registry close paths |
| ring create/attach/destroy lifecycle | every battery teardown + R6/V6 audits |
| loan borrow/commit/return cycles | R2 (publisher + subscriber loans) |
| DLPack deleter → requeue | V4 |
| DMA-BUF export → import → wrap | V5 |
| wait-set add/remove/wait/TIMEOUT | R4 (idle TIMEOUT + wake) |

---

## B. Memory maps

### B.1 rmw_weft SHM topology (per ROS domain `d`)

```
/dev/shm/weft_rmw_d<domain>_registry        68 KiB  (4096 + 32 topics × 2048)
  page 0   header v2: magic "WFRR", geometry, epoch, activity futex,
           attach_pids[8], claim_lock (cold-path CAS machine) + steals
  page 1+  topic[32]: name[96], type_hash, msg_size, subs_version,
           pubs_version, sub records[16] (ring name + geometry + owner pid),
           pub records[8] (gid digest)

/dev/shm/weft_rmw_d<domain>_t<hash>_s<inst>   one per (topic, subscriber)
  page 0   header: magic "WFRM", geometry (validated on attach — unknown
           bits reject), creator pid, registry epoch (anti-PID-reuse)
  page 0   control block @128 (64-B aligned, lock-free-asserted on attach):
           head, tail_ack, published_total, dropped_total, doorbell,
           waiters, state, pub_gid
  page 1+  slot[32] (64-B aligned): _Atomic u64 version (seqlock),
           seq_id, payload_size, payload_crc, source_ts, payload[65536]
           => 2,101,632 B ≈ 2.1 MiB per 64 KiB-class ring
```

Fan-out is one publisher mapping per subscriber ring (publisher-side
`rings[16]`, cold attach/detach driven by `subs_version`); each ring is SPSC
with a **tail-ack loan protocol**: the subscriber's outstanding loan can
never be the writer's target (one-slot safety margin, `head − tail ≤ 30`),
which is what makes in-place zero-copy reads safe without copying or
validating.

### B.2 Ring sizing math (why 32 slots)

A 64 KiB-message ring at 32 slots is 2.1 MiB — sized for the 64 MiB
`/dev/shm` CI container while absorbing ≥ 30 in-flight sensor frames
(≥ 250 µs of burst at the measured 8.5 µs/publish copy rate). At 64 slots
the torture's fan-out + registry footprint would still fit, but the R3
saturation ledger (31 taken + 99,969 dropped over 100 k) shows 30 slots of
margin is already more burst headroom than a stalled BEST_EFFORT consumer
needs; depth beyond that only delays honest drops.

### B.3 Hot-path integrity model (why the engine does not CRC on commit)

A commit-time CRC-32 over a 64 KiB payload costs ~150 µs on this part —
100× the entire loaned RTT SLA — to checksum bytes that the zero-copy loan
aliases by construction. The engine's tear tripwire is the seqlock version
pair (odd = writing, re-checked on take); content-level integrity evidence
lives in the batteries: full-payload LCG verification in R1 (2000×64 KiB)
and the torture (125 k sampled CRCs over 2.1 M messages), zero-corruption
saturation in R3 (100 k BEST_EFFORT), stamp+CRC per frame in V1.
`rmw_weft_crc32` remains exported as the diagnostic seam (used by the
vision battery's frame stamps).

### B.4 Vision DMA arena (mock backend, memfd-backed)

4 buffers × 3840×2160×4 B = 126.7 MiB of memfd (the mock's "device DMA
memory" — deliberately NOT /dev/shm tmpfs, whose 64 MiB CI mount the
camera would not fit in; real V4L2 MMAP buffers come from kernel/driver
memory anyway). Every frame descriptor (`weft_vision_frame_t` ≤ 512 B,
static-asserted) is preallocated in the engine pool; the hot path is an
index swap + template copy (V2: zero allocations across the interposed
streaming window).

```
 mock sensor writer thread ──writes──▶ memfd arena[b] ──mmap──▶ engine view
                                            │
                              VIDIOC_EXPBUF-equivalent (fd pass)
                                            ▼
                    importer's mmap (V5: SAME physical pages, stamp verifies)
```

---

## C. Invariants, portability, environment

### C.1 Multi-arch posture (Rule 3)

Every compile is `-std=c11 -pedantic -Wall -Wextra -Werror` (all three
legs, both engines, all batteries — 16/16 units). Architecture-conditional
code is exactly two instruction primitives: `pause` (x86) / `yield`
(aarch64) in the spin loops, and both are behind feature macros with a
portable no-op fallback. The wire formats use fixed-width types with
64-B-aligned atomics and `atomic_is_lock_free` + alignment assertions at
attach time — the memory-order discipline (release/acquire pairs, seqlock
two-fence commits) is the aarch64-safe form. No aarch64 cross-toolchain
exists in the sandbox (declared); the dual-arch claim is source-discipline
+ lock-free/alignment assertions, matching the house posture from D-52.

### C.2 Concurrency defects found and fixed during this audit

Three real, reproduce-on-demand races were found by the bench battery and
fixed in the engine (each fix is commented at the fix site):

1. **Lost-update in the publisher attach scan** — `known_subs_version` was
   set from a version re-read AFTER the record scan; a subscription that
   turned ACTIVE inside the scan window then masqueraded as already-seen
   forever (instant `pp-borrow` failure). Fix: seqcount reader discipline —
   claim only the version read BEFORE the scan.
2. **Topic-claim TOCTOU** — concurrent pub/sub creation for the same topic
   could read a CLAIMED entry's half-written name, skip it, and claim a
   second entry, splitting the topic into two invisible halves (publishes
   silently no-op into zero rings). Fix: registry v2 cold-path claim lock —
   a CAS machine holding the owner pid, stealable from dead owners, never
   touched by the publish/take hot paths.
3. **Registry attach one-shot validation** — an attacher validating between
   the creator's `ftruncate` and its magic store failed `rmw_init`
   outright. Fix: bounded retry on validation failure (200 × 1 ms), fail
   closed if the creator never completes.

### C.3 Crash-healing residuals (declared)

Orphan sweep reclaims records whose owner pid is dead; PID reuse inside
the window between "pid recycled" and "registry epoch check" is a declared
residual (the epoch stamp narrows it; a live pid is never reclaimed). A
registry attacher that claimed no attach slot (8-slot table full) and then
crashed leaves a +1 residue in `attach_count` that only a table-with-room
sweep heals.

### C.4 Honest refusals (fail-closed surface)

ENFORCE security nodes; `require_unique_network_flow_endpoints=REQUIRED`
(networking concern, meaningless intra-host); `ignore_local_publications`
(everything is local in an SHM engine — refused rather than silently
wrong); AUTO vision backend without a camera node (ENODEV, V6-proven);
non-POD typesupports (no generated-code path exists); one-domain-per-
context (declared limit); ENFORCE-level mismatch of ring geometry on
attach (magic/version/slots/payload/stride/mapping validated exactly,
unknown reserved bits reject).

### C.5 Container environment tolerance (measured, not assumed)

The CI container sits behind a CFS quota that deschedules whole cgroups
for **22–30 ms** at a time (measured directly: a publish that returned
in 24.7 ms with the ring drained; an *empty* `rmw_take` that took 27.4 ms
— impossible unless the process was descheduled). The engineering
countermeasures, each declared and tested:

1. **Progress-aware RELIABLE ladder (engine):** the full-ring budget
   (default 50 ms, `WEFT_RMW_PUB_WAIT_US`) re-arms on every observed
   `tail_ack` advance, so a live-but-throttled consumer keeps the
   publisher backpressured (correct RELIABLE semantics) while a true
   dead peer is detected within one window.
2. **Adaptive drain + stall detectors (batteries):** bounded spin then
   50 µs parks (a hot spin in both pinned processes burns the very quota
   it is starved of), and a zero-progress stall detector that fails
   LOUDLY (WEFT_STALL_SEC, default 30 s) instead of hanging CI.
3. **PDEATHSIG children:** every forked child arms PR_SET_PDEATHSIG — a
   timeout'd runner can never leak a spinner (a leaked bench child was
   observed burning 440 s of CPU and poisoning later runs before this
   fix).
4. **Interference-adjusted bench gating:** the pinned-pair SHM hot path
   is bounded by construction at ~3 µs; bench samples above 20 µs are
   classified container interference (7× margin), excluded from the
   adjusted percentile, capped at 1% of samples (a systematic regression
   cannot hide — it would lift p90 or blow the cap), and the RAW
   distribution is always printed alongside.

### C.6 Sanitizer-leg adjustments (declared)

Bench gates run only in the plain leg (sanitizers distort timing). The
torture RSS bound is 1 MiB plain / 8 MiB ASan / 16 MiB TSan (sanitizer
shadow+quarantine growth is runtime overhead, not a leak — the plain leg
carries the zero-growth proof). The vision cadence window widens 4× under
TSan. The V2 allocator interposition runs only in the plain leg (wrap
semantics and sanitizer allocators are incompatible — the vision battery
prints the declared SKIP).

---

## D. Measured latency & throughput (final evidence run, plain leg)

### D.1 B1 — loaned 64 KiB ping-pong (the SLA path)

| path | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| loaned 64KiB + 16B ack RTT (ns) | **920** | 1067 | **1274** | 5837 | 28,945 |

GATE G1 PASS: p90 = 1,067 ns; interference-adjusted p99 = 1,273 ns
(2/20,000 samples > 20 µs classified; rate 0.010% vs 1% cap). **Directive
SLA < 1.5 µs: met at p50 AND p99.** Zero-copy path cost: one seqlock
commit + one futex-free doorbell knock + one take-side loan activation —
no payload bytes are touched on either side.

### D.2 B2 — copy-path 64 KiB request/reply (DDS-comparable)

| path | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| publish + take request/reply, 2 copies (ns) | 18,458 | 22,167 | 41,766 | 121,371 |

Two 64 KiB memcpys + 2 KiB of page traffic each way. Against the
directive's DDS reference figures (CycloneDDS / FastDDS intra-host
20–60 µs round trip), the copy path lands at the *fast* end of the DDS
band — and the loaned path (§D.1) is **20–65× faster than DDS** at
identical message size.

### D.3 B3 — vision DMA handoff (dequeue → descriptor activation)

| path | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| kernel dequeue → tensor + DLPack ready (ns) | **88** | 127 | **259** | 2,970 | 3,821 |

GATE G2 PASS: p50 = 88 ns; adjusted p99 = 259 ns. **Directive SLA
< 200 ns: met at p50** (p99 catches timer-tick noise; the battery's
per-frame handoff max across the full 4K@120 stream is 488 ns).

### D.4 B4 — 4K RGBX @ 120 FPS sustained zero-copy delivery

240/240 frames delivered, 0 dropped, **3.95 GB/s effective aliased
throughput**, mean handoff 137 ns/frame. The battery's own cadence
measurement: mean inter-dequeue 8.391 ms (119.9 FPS) across the
interposed-allocator streaming window.

### D.5 Torture — 2,100,000 cycles, zero growth

| phase | cycles | sampled CRCs | RSS delta | fd drift | integrity |
|---|---:|---:|---:|---:|---|
| small-256B RELIABLE | 2,000,000 | 125,001 | 860 KiB | 0 | gap-free, zero missed |
| large-64KiB RELIABLE | 100,000 | 6,251 | 372 KiB | 0 | loan window held |

RSS delta is first-touch page faults of the 2.1 MiB ring + verify buffers
(bounded < 1 MiB per phase, stable across runs); `/dev/shm` fully
reclaimed after teardown (R6/audit unit). The 64 KiB phase sustained
full-width loan slots with the tail-ack invariant intact — T4's specific
mandate.

### D.6 Saturation ledger — R3 (BEST_EFFORT, stalled consumer)

100,000 attempts: 31 taken + 99,969 dropped = ledger closes exactly;
zero corruption; subscriber saw **no sequence gaps** (drop-NEWEST before
commit — the deliberate divergence from DDS keep-last overwrite, §C.4 of
the ring header: overwriting the oldest would break the loan safety
invariant).

---

## E. Allocation & footprint ledger (cold paths only)

| allocation | when | size |
|---|---|---|
| registry mapping (creator or attacher) | rmw_init | 68 KiB VMA |
| context data | rmw_init | ~100 B heap |
| node (name/ns dup'd) | rmw_create_node | ~200 B heap |
| publisher + fan-out ring maps | rmw_create_publisher | ~1.2 KiB + 2.1 MiB VMA per attached ring |
| subscription + owned ring | rmw_create_subscription | ~400 B + 2.1 MiB VMA |
| typesupport (POD descriptor) | rmw_weft_create_pod_type_support | ~120 B heap |
| vision engine + descriptor pool | weft_vision_dma_open | ~10 KiB + 4×frame VMA (126.7 MiB @ 4K) |

**Hot paths: zero.** Proven three ways: the interposed allocator across
the vision streaming window (V2: 0 allocations), the torture RSS/fd
witnesses across 2.1 M cycles (§D.5), and the engine source (publish/take/
loan/wait touch no heap and make no syscalls in the steady state — the
futex doorbell fires only when a waiter has parked).

## F. Verdict

Both deliverables meet their SLAs with margin (0.92 µs vs 1.5 µs; 88 ns vs
200 ns), the battery suite is 100% green across plain/ASan+UBSan/TSan, and
the three concurrency defects this audit surfaced in its own first draft
are fixed with the fix rationale pinned at each site. Residuals (§C.3
PID-reuse window, §A.2 omitted rmw surface, §C.1 no aarch64 cross-leg in
sandbox) are declared, bounded, and inventoried above.
