# Litmus Suite Specification — L1–L8

**Normative. The litmus suite is the canonical artifact of the project.** The kernel exists to pass these tests; the tests do not exist to exercise the kernel. Every verdict predicate below is **mechanical** — a boolean computable from recorded metrics. If an executor cannot implement a predicate as written, that is a spec bug: file it, don't reinterpret it.

---

## §0 Shared definitions (implement once per language, identically)

### 0.1 Payload pattern `pat(seq, i) → u8`

```
mix32(x):                      # all ops mod 2^32
    x ^= x >> 16
    x *= 0x7FEB352D
    x ^= x >> 15
    x *= 0x846CA68B
    x ^= x >> 16
    return x
pat(seq, i) = low8( mix32( seq * 2654435761 + i * 2246822519 ) )
```

C: `uint32_t` arithmetic (overflow defined). Rust: `u32` `wrapping_mul`. TS: `Math.imul(v, k) >>> 0` for every multiply, `>>> 16` for shifts. **The three implementations must produce byte-identical sequences** — L6's differential property depends on it.

### 0.2 PRNG — xorshift32 (Marsaglia 13/17/5)

```
step(x):  x ^= (x << 13); x ^= (x >>> 17); x ^= (x << 5); return x   # u32 wrap
```

State seeded from the catalog (`seed`, default `0x00C0FFEE`); state 0 is invalid (reseed 0x9E3779B9). Delay sample = `step() % max_delay_us`. Identical seeds → identical schedules in all languages (A5).

### 0.3 Deadline pacing

```
next = now(); loop { work(); next += period; if (now() - next > period) next = now();
                     sleep(max(0, next - now)); }
```

Monotonic clock (`CLOCK_MONOTONIC` / `Instant` / `performance.now`). Never `sleep(period)` per iteration (A4).

### 0.4 Frame verification `verify(held_buffer, expected_seq, payload_len) → bool`

Reads the **live** held buffer: magic OK · envelope seq still == `expected_seq` · `payload[i] == pat(expected_seq, i)` for all i · canary == `expected_seq`. Any mismatch = one **torn frame**. Verification after a hold MUST read the live buffer (A3 — a claim-time snapshot makes this test unfalsifiable).

### 0.5 Hold injection

A **hold** is `sleep(hold_ms)` executed by the reader *between* `r_claim()` returning and verification beginning. That window is the maximum-exposure state: reader owns a buffer, writer churns at full rate. The holds are the experiment.

---

## L1 — tear

| | |
|---|---|
| **Adversary** | Reader hold stretched 5/10/50 ms between swap and read; writer at 2× display rate |
| **Setup** | Per hold value: fresh Weft, `payload_max=1024`. Writer thread: paced 240 Hz, `frames=600`, writes `pat(seq)` payload, publishes seq 1..600. Reader = main thread. |
| **Procedure** | Reader loops until drained: `idx = r_claim()`; read envelope seq `S`; if `S != last`: **hold**, then `verify(buf, S, 1024)`; `last = S`. If unchanged: sleep 1 ms (null-spin guard). Drain: after writer join, one more claim MUST return `S == 600` — else `drain_fail`. Guard: 30 s wall-clock timeout per config. |
| **Verdict** | `torn_frames == 0` AND `drain_fail == false` AND `claims >= 600` across **all three holds** |
| **Metrics** | `{"holds_ms":[5,10,50], "claims":N, "torn":N, "drain_ok":bool}` |
| **Hazards** | False green: verifying a snapshot (A3) · kernel that copies-on-read inside claim passes vacuously — also forbidden by 02 §2.2. False red: reader not re-reading seq after null-spin (missed final frame → drain_fail). |

## L2 — writer step bound (wait-free writer)

| | |
|---|---|
| **Adversary** | Instrumented publish while the reader is held for swept durations 0/10/25/50/100 ms |
| **Setup** | Per hold: fresh Weft, `payload_max=256`. Writer: `publishes=2000`, paced 2000 Hz. Reader: continuous `r_claim()` + hold sleep. |
| **Procedure** | Kernel exposes `t_wsteps` (incremented once per protocol RMW in `w_publish`). Writer wraps each publish: `d = t_wsteps(after) − t_wsteps(before)`; track max `d`. (Concurrent reader claims cannot pollute this — `t_rsteps` is a separate counter.) |
| **Verdict** | `max_wsteps <= 2` (bound from catalog; 1 protocol RMW + 1 slack) AND `publishes == 2000` (no back-pressure, no drop except revoked — none here) in every config |
| **Metrics** | `{"holds_ms":[0,10,25,50,100], "publishes":N, "max_wsteps":N, "bound":2}` |
| **Hazards** | A shared steps counter for both sides invalidates the measurement (reader claims land inside the writer's delta window). Two counters, non-negotiable. |

## L3 — reader step bound (wait-free reader)

Mirror of L2: writer storm at 4× (960 Hz), `claims=2000` on the reader, bound on `max_rsteps <= 2`, verdict additionally requires `claims == 2000` and zero claim failures (claim cannot fail — if your API returns an error here, 02 §2.2 was violated). Metrics: `{"claims":N, "max_rsteps":N, "bound":2}`.

## L4 — freshness

| | |
|---|---|
| **Adversary** | Writer at 4× reader rate |
| **Setup** | One Weft, `payload_max=256`. Writer: `frames=2000` at 960 Hz. Reader: paced ~240 Hz. |
| **Procedure** | Per claim: `P0 = t_publish (Acquire load)` → `claim` → read envelope `S` → `P1 = t_publish (Acquire load)`. If `S < P0`: freshness violation. If `S > P1`: spin-re-read `P1` (bounded 2 ms — telemetry lag, 02 §5) then still `S > P1` → future violation. **Final drain:** after writer join, claim → `S` MUST equal exactly `2000`. |
| **Verdict** | `freshness_violations == 0` AND `future_violations == 0` AND `drain_exact == true` |
| **Metrics** | `{"frames":N, "freshness_violations":N, "future_violations":N, "drain_exact":bool}` |
| **Why P0 ≤ S holds** | `latest` advances monotonically in seq; any publish completed before the claim's exchange entered the modification order is either the returned frame or an older one still... no — returned frame IS the newest at exchange time, so `S ≥ seq of any publish whose telemetry increment preceded P0's read` ⇒ `S ≥ P0` (conservative). The subtle case (telemetry increment lagging its own swap) only makes `P0` smaller ⇒ still safe. Do not "simplify" to `S ≥ P1`. |

## L5 — progress (writer never blocked)

| | |
|---|---|
| **Adversary** | Reader held 0/10/50/100 ms; plus a config where the reader is **suspended entirely** (never claims) |
| **Setup** | Per config: fresh Weft, `payload_max=256`. Writer: paced 2000 Hz for a 1.0 s window. |
| **Procedure** | Count `Δt_publish` over the window. `rate = Δ / 1.0s`. |
| **Verdict** | `max(rate)/min(rate) <= 1.5` across all five configs |
| **Metrics** | `{"rates_hz":{...}, "ratio":x, "configs":["hold-0","hold-10","hold-50","hold-100","suspended"]}` |
| **Hazards** | False red from per-frame sleeps (A4) and from GC/JIT noise in TS — use the deadline schedule and report the raw rates; **tolerance 1.5 is catalog-owned and may not be tuned by an executor**. |

## L6 — ownership (canary + randomized interleavings)

| | |
|---|---|
| **Adversary** | Randomized writer/reader delays (xorshift32, catalog seed), 200 trials |
| **Setup** | Per trial: fresh Weft, `payload_max=256`, 64 frames. Two threads; per-op delay = `rng() % 500` µs (independent states: writer seed, reader seed = `seed ^ 0x9E3779B9`). |
| **Procedure** | Reader verifies EVERY claimed frame (0.4-style, `payload_len=256`). Kernel-maintained canary checked as part of verify (§0.4). |
| **Verdict** | `violations == 0` across all trials, all languages, same seed → identical schedule |
| **Metrics** | `{"trials":200, "frames_per_trial":64, "violations":N, "seed":"0x00C0FFEE"}` |
| **Note** | This is the differential-trace test (A5): C/Rust/TS run byte-identical delay schedules. If C passes 200/200 and TS fails at trial 37 with the same seed, that is a **TS kernel bug**, findable by replay. |

## L7 — revocation (I6 under fire)

| | |
|---|---|
| **Adversary** | `release()` while a native writer spins uncapped |
| **Setup** | One Weft, `payload_max=256`. Writer thread: infinite `w_publish` loop (no pacing), counts `first_revoked_at` (its own iteration counter) and then performs 100 further publish attempts. |
| **Procedure** | Main: sleep 5 ms → `e0 = epoch` → `revoke()` → `reclaim(timeout=2000)` (waits ACK per 02 §6, then **poisons all buffers with 0xDE and keeps the mapping**) → join writer → scan all 3 buffers byte-wise. |
| **Verdict** | `reclaim_ok` (ACK arrived < timeout) AND writer's 100 post-revocation attempts **all** returned `DROPPED_REVOKED` AND every byte of all buffers == 0xDE (zero writes to poisoned pages) |
| **Metrics** | `{"reclaim_ok":bool, "post_revoke_revoked":N, "poison_intact":bool, "first_revoked_at":N}` |
| **Hazards** | Poison-before-ACK = the bug this test hunts (A1) — but note the test can also **false-green** it: if poison lands late, an in-flight write corrupts the poison and `poison_intact=false` (good); if poison lands early it may corrupt the writer's *legitimate* final publish and ALSO fail — that is correct behavior (the handshake was violated by the kernel). Either red = STOP and file. |

## L8 — envelope

Pure functions + one live Weft; no threads, <1 s.

| Subcheck | Procedure | Pass condition |
|---|---|---|
| a. round-trip | encode(v1, seq=7, plen=100) → decode | fields equal; re-encode byte-identical |
| b. unknown trailing fields | encode with `header_size=24`, 8 opaque 0xAA bytes; decode | `DECODE_OK`; `header_size==24`; payload located at +24; extras ignored |
| c. negotiation | full table in 03 §5 | all four rows exact |
| d. coexistence | Weft A stamped v1, Weft B stamped v2 (synthetic), same reader code decodes both | both `DECODE_OK`, versions preserved |

Verdict: all four true. Metrics: `{"roundtrip":bool,"unknown_fields":bool,"negotiation":bool,"coexist":bool}`.

---

## Suite runtime budget

C ≈ 20 s · Rust ≈ 20 s · TS ≈ 30 s. If a language's suite exceeds 90 s, reduce frames via the **catalog**, not by editing runners. Every runner prints exactly one JSON verdict line per test (05 §2); the driver assembles the matrix and enforces the cross-language rule (A6).
