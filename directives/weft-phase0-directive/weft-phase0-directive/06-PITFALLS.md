# Pitfalls — Ground Truth, Per-Language Traps, Failure Modes

**Advisory but experience-tested.** §1 was verified live in this sandbox on the day of issue. Read §5 (false greens/reds) before your first full run — most debugging pain is pre-diagnosed there.

---

## 1. Environment ground truth (verified)

| Fact | Consequence |
|---|---|
| GCC 14.2.0, make, bash present | C toolchain ready as-is |
| **rustc/cargo NOT installed** (roadmap errata, A7) | Step 0: `curl -O https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init && chmod +x rustup-init && ./rustup-init -y --profile minimal` — network to static.rust-lang.org **verified reachable** |
| Node v24.19.0 runs `.ts` natively (type-stripping) — **verified live** | No tsc, no build step for the TS kernel. Plain `.ts` files + `package.json` `{"type":"module"}` |
| `SharedArrayBuffer` + `Atomics.exchange` work — **verified live** on main thread | The TS kernel design (02 §7) is viable exactly as planned |
| `worker_threads` present — **verified live** | Writer runs in a Worker; reader on main thread |
| Python 3.12.14; `nproc` = **2** | Driver runs languages sequentially; don't parallelize inside the machine |
| 2 cores | L5's `ratio ≤ 1.5` tolerance is calibrated for 2 cores; do not tighten it |

**Pre-flight script (Step 0, ~10 min):** run `node -e` probes for SAB/exchange/workers and `rustc --version` after install. If any probe fails, STOP — the substrate changed, file an advisory.

## 2. C11 traps

- `atomic_exchange_explicit(&latest, w_work, memory_order_acq_rel)` — the *explicit* form; the plain `atomic_exchange` is seq_cst (fine but noisy in the ordering audit). **Never** `memory_order_seq_cst` anywhere in the kernel — the ordering matrix (02 §5) is the audit; a structurally-enforced check ("no `seq_cst` token in kernel sources") is a one-line script worth adding to the validator.
- Envelope via `memcpy`, never `*(uint32_t*)buf` (alignment + strict aliasing).
- `posix_memalign(&p, 64, size)` — check both the return value AND that `size % 64 == 0` for clean tails. Free in `destroy` only; `reclaim` poisons but does not free (L7 must scan after poison).
- Telemetry counters: `atomic_fetch_add_explicit(..., memory_order_relaxed)` on `atomic_ullong` — print with `%llu` casts, not `%zu` habits.
- Reader hold injection: `nanosleep` in a loop until the deadline (single `nanosleep` can return early on signals — this silently shortens holds and weakens L1).
- TSAN build (A8): `-fsanitize=thread` is incompatible with `-Wl,--no-as-needed` habits; just `-O1 -g -fsanitize=thread`. If TSAN reports a race **inside a test that passed clean**, the TSAN result wins — file it.

## 3. Rust traps

- Zero crates, non-negotiable: `Cargo.toml` has **no `[dependencies]` section**; build with `--offline`. Any `use serde` PR = reject.
- `w_work`/`r_work` as `AtomicU32` with Relaxed: they are thread-private; the atomics exist **only** so `Weft` is `Sync` and scoped threads can share `&Weft`. Document this in a comment at the field — an auditor must not mistake them for protocol state (they are the *only* atomics that are not).
- Payload mutation through `&self` needs `unsafe` — confined to two helpers (`w_write_payload`, `r_read_slice`), each with a `SAFETY:` comment citing RFC-0001 §4 ("writer owns `w_work` exclusively between its exchanges; reader owns claimed buffer until next claim"). **No unsafe outside these two functions.** This is the roadmap's stated contract (§0c).
- `std::thread::scope` for test threads (no `'static` gymnastics, no `Arc`).
- `r_read_slice` must read the live buffer at call time — do not cache a `&[u8]` across the hold (A3 in Rust clothing: a borrowed slice that lives across the hold is actually FINE and preferable — it IS the live buffer; just never `to_vec()` before the hold).
- L4: acquire-loads via `t_publish.load(Ordering::Acquire)`; the catch-up spin uses `std::thread::yield_now()` between polls, bounded by `Instant`.
- Edition 2021; `cargo build` must be warning-clean with `-D warnings` in CI-hygiene terms — warnings are protocol-review noise.

## 4. TypeScript traps

- Envelope math: every `DataView` accessor gets `littleEndian=true`. A single `false` produces L8a failures that look like kernel bugs.
- u32 arithmetic: `Math.imul` for multiplies, `>>> 0` after shifts/xors. `(x * 2654435761) | 0` is NOT u32 multiplication (precision loss) — pattern/PRNG divergence vs C/Rust will surface as L6 trial mismatches.
- `Atomics.exchange(i32a, index, value)` operates on **Int32** elements — `latest` must live in the Int32 control block, never in the byte data region.
- `Atomics.wait` throws on the main thread — polling loops use `setTimeout`/`Atomics.waitAsync`; only the Worker may block. Design the reader (main) to never need `wait`.
- Worker file: `new Worker(new URL('./writer_worker.ts', import.meta.url), { type: 'module' })`. Verified Node 24 strips TS in workers; if a probe says otherwise (06 §1 pre-flight), fall back to a plain-`.mjs` worker whose publish loop mirrors the kernel (≤40 lines, header comment "mirror of weft.ts w_publish — kept in sync by L6 differential trace") — this is the ONE sanctioned code duplication.
- GC noise: TS L5 rates jitter with GC; report raw, compare with tolerance, never re-run-until-green (that is dishonesty, Law 4).
- One `SharedArrayBuffer` per Weft sized `controlBlock(64B) + 3 × buf_size`; document offsets; align data region to 8 bytes minimum (envelope u32s), 64 preferred.

## 5. False-green / false-red registry (pre-diagnosed)

| Symptom | Likely cause | File |
|---|---|---|
| L1 green but meaningless | Reader verified a claim-time snapshot (A3) | 04 §0.4 |
| L1 `drain_fail` sporadically | Reader treats "same seq twice" as terminal instead of re-claiming after writer join | 04 L1 |
| L4 random reds | Catch-up spin missing → telemetry lag counted as future violation | 02 §5, 04 L4 |
| L5 red only under 50/100 ms holds | Sleep-per-frame drift, or reader holds starving the 2-core writer — deadline pacing fixes the first; if the second, it is a REAL finding (reader starvation) → STOP, file | A4 |
| L6 red in exactly one language at a specific trial | Kernel divergence — replay with the catalog seed; the schedule is identical by construction (A5) | 04 L6 |
| L7 `poison_intact=false` | Poison ran before ACK (kernel bug, A1) **or** writer touched bytes post-ACK (kernel bug) — both are STOP-and-file, neither is a harness bug | 02 §6 |
| L8b fails in one language | Payload offset hardcoded to 16 instead of `header_size` | 03 §2.5 |
| All green but suite finished in 2 s | Holds/sleeps not actually executing (early-return nanosleep, wrong units ms/µs) — verify L1 wall time ≈ holds × configs × claims | 06 §2 |

## 6. Hardening pass (A8) — after first full green

1. `make build-c-dbg && make tsan` — full L1–L8 under TSAN.
2. Expected: clean. A TSAN race report **blocks sign-off** even with 24/24 green.
3. Add the TSAN column to REPORT.md labeled `c11-tsan-x86_64-sandbox (supplementary)`.
4. Follow-up (file, do not execute): `L-loom` — Rust kernel under `loom` for exhaustive interleaving proof; the natural Phase 0.5.

## 7. Review checklist (PR gate, all languages)

- [ ] 02 §2.2 forbidden patterns: none present (this checklist item is pass/fail by inspection)
- [ ] Ordering matrix: kernel atomics match 02 §5 line-for-line; no `seq_cst` tokens
- [ ] No allocation in publish/claim paths (C: no malloc calls; Rust: no `vec!`/`clone` in hot path; TS: no new arrays/views per frame)
- [ ] `claim()` cannot fail; `publish()` returns only OK/REVOKED
- [ ] Envelope access via the 03 §2 decode rules only; payload offset = `header_size`
- [ ] I6: revoked checked at top of publish; ACK fetch_add; reclaim polls epoch before poison
- [ ] LOC within budget ±20%; zero dependencies; warnings clean
- [ ] Every metric name matches 04/05 exactly (driver schema will catch drift, but review anyway)
