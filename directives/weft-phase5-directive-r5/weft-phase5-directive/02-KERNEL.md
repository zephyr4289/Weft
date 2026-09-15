# Kernel Specification — Corrected Triad Protocol

**Normative.** This file defines the only permitted kernel design. Source of truth: RFC-0001 §4 (`weft-docs/rfcs/0001-triad-exchange-protocol.md`); this document restates it operationally for Phase 0 and resolves every detail RFC-0001 left to implementations.

---

## 1. State

One `Weft` instance owns exactly:

| Field | Type | Init | Access discipline |
|---|---|---|---|
| `buf[3]` | 3 buffers, 64-byte aligned, `buf_size` each | zeroed; envelope seq=0 (= null frame), canary=0 | ownership rules below |
| `latest` | **single shared atomic index** (u32) | `0` | exchanged by writer & reader |
| `w_work` | writer-private index | `1` | writer thread only (Rust: atomic-Relaxed solely to satisfy `Sync`; documented) |
| `r_work` | reader-private index | `2` | reader thread only (same Rust note) |
| `revoked` | atomic bool | `false` | see §6 |
| `epoch` | atomic u32 | `0` | see §6 |
| `t_publish`, `t_claim`, `t_drop` | atomic u64 counters | `0` | Relaxed; telemetry only |
| `buf_size` | usize | `align64(16 + payload_max + 8)` | immutable after init |

**Buffer layout (per buffer):** `[0..16)` frame envelope · `[16..16+payload_max)` payload · `[buf_size−8..buf_size)` canary (u64 LE). Canary is written by the kernel in `w_publish`, value = seq.

`payload_max` is fixed at init. Phase 0 default: **1024 bytes** (L1) / 256 (L2, L3, L6, L7) — always passed via catalog/driver, never hardcoded in the kernel.

## 2. The protocol (complete — this is all of it)

```
writer.publish(seq, payload_len):
    if revoked.load(Relaxed):                      # §6: checked FIRST, before any byte write
        epoch.fetch_add(1, AcqRel)                 # ACK
        t_drop++; return DROPPED_REVOKED
    write envelope (v1, seq, payload_len) into buf[w_work]   # writer owns w_work
    write canary = seq at buf[w_work].tail
    old = latest.exchange(w_work, AcqRel)          # THE atomic: publish + take old latest
    w_work = old
    t_publish++ (Relaxed); return PUB_OK

reader.claim():
    mine = latest.exchange(r_work, AcqRel)         # THE atomic: acquire newest + hand back previous
    r_work = mine
    t_claim++ (Relaxed)
    return mine                                    # read buf[mine] IN PLACE; held until next claim

reader.release():                                  # implicit — folded into the next claim's exchange
```

### 2.1 Ownership argument (memorize; do not "improve" the design)

1. Every exchange on `latest` transfers **exactly one buffer** between the two parties: writer's publish hands `w_work` out and receives the previous `latest`; reader's claim hands `r_work` out and receives the previous `latest`.
2. The writer only ever writes the buffer it received from **its own** exchange; the reader's held buffer can only re-enter the writer's hands through the **reader's own** exchange.
3. The two exchanges are totally ordered (single variable, one RMW each). Therefore at every point in the order each buffer has exactly one owner. Torn reads are impossible **by construction**, not by timing.

### 2.2 Forbidden patterns (any one of these in review = reject the PR)

- A second atomic participating in buffer-ownership decisions (the withdrawn two-variable `latest`+`claimed` design — this is the founding spec's §5 bug; see `01-ADVISORIES` and RFC-0001 §3).
- Candidate-set computation, exclusion sets, "pick a free slot" logic.
- CAS retry loops in publish/claim (wait-freedom = zero loops; `exchange` never retries).
- Returning `None`/failure from `claim()` — a claim **always** yields a buffer (initially the null frame, seq=0).
- Copy-on-read inside `claim()` (readers must observe the live buffer; A3).
- Allocation of any kind in `publish`/`claim` (all memory is owned from `init`; Law 2: zero is a contract).

## 3. Initialization and teardown

`init(payload_max)`: allocate 3 buffers (`posix_memalign` / `std::alloc` 64-aligned / one `SharedArrayBuffer`), zero them, write null envelopes (seq=0, magic, v1, header=16), set indices per §1. `init` may allocate; nothing else may.

`destroy()`: idempotent; frees nothing that `reclaim` already released (§6). If `revoke` was never called, plain free is correct (no writer exists). `destroy` without a prior `reclaim` while a writer may still run is a **caller error** — document, don't defend.

## 4. Public API contract (language-neutral; keep signatures 1:1 across C/Rust/TS)

```
init(payload_max)                      -> Weft
w_begin()                              -> payload write-cursor (ptr / &mut via helper / offset)
w_write_payload(src: bytes)            -> ok           # convenience: w_begin + fill
w_publish(seq: u32, payload_len: u32)  -> PUB_OK | PUB_DROPPED_REVOKED
r_claim()                              -> buffer index (u32; NEVER fails)
r_seq() / r_header_size() / r_magic()  -> envelope fields of the held buffer (live)
r_read_slice(dst, offset=16)           -> copies LIVE held-buffer bytes at call time (A3)
revoke()                               -> ()           # step 1 of §6
reclaim(timeout_ms)                    -> OK | TIMEOUT # steps 2–3 of §6
t_publish() / t_claim() / t_drop()     -> u64
```

Envelope encode/decode/negotiate are **pure free functions** (spec in `03-ENVELOPE.md`) so L8 can test them without threads.

**Concurrency contract:** exactly one writer thread and one reader thread (enforced by contract in Phase 0; fan-out is a v2 Heddle concern). `revoke`/`reclaim` are called from a third context (test main / Steward) — this is the supported lifecycle path.

## 5. Memory-ordering matrix (normative — identical in all three kernels)

| Location | Op | Ordering | Justification |
|---|---|---|---|
| `latest` | `exchange` (writer) | **AcqRel** | Release: publishes payload writes to the reader. Acquire: takes ownership of returned buffer, sees its final state. |
| `latest` | `exchange` (reader) | **AcqRel** | Same edges, reader direction. (Not plain Acquire: the reader's returned buffer becomes claimable by the writer only via this same RMW — symmetric.) |
| `revoked` | writer `load` | **Relaxed** | Advisory check only; correctness does not depend on seeing it this publish (the NEXT publish will see it; buffers stay valid until ACK, §6). |
| `revoked` | releaser `store` | **Release** | Pairs with ACK's `fetch_add(AcqRel)` chain into reclaim's Acquire poll — establishes the happens-before edge that makes poison/free safe. |
| `epoch` | writer `fetch_add` | **AcqRel** | The ACK. Publishes "I will never write again." |
| `epoch` | reclaim `load` (poll) | **Acquire** | Observes ACK; the edge `revoke(Release) → ACK → Acquire-poll` is what licenses poison/free. |
| telemetry | `fetch_add` | **Relaxed** | Statistics; never synchronization. |
| `w_work`/`r_work` | none (C: plain fields) | — | Thread-private by contract. |

L4's freshness argument additionally requires the **telemetry read** (`t_publish`) to be an Acquire load — an Acquire RMW chain from the writer's post-swap Relaxed increment is NOT enough; specify `t_publish` increments as Relaxed but L4's reads as Acquire **loads of `t_publish` combined with the claim's Acquire exchange**: the claim's Acquire edge synchronizes with the writer's Release-half, so any publish whose envelope the reader can observe is ordered before `P1`'s read. The full measurement protocol that makes this airtight is in `04-LITMUS.md` L4 — implement that, do not improvise.

## 6. I6 — writer revocation handshake (normative; A1)

```
REVOKE   releaser:  revoked.store(true, Release)
ACK      writer:    at the TOP of the next w_publish — before touching envelope,
                    payload, or canary — if revoked.load(Relaxed):
                        epoch.fetch_add(1, AcqRel); t_drop++; return DROPPED_REVOKED
RECLAIM  releaser:  e0 = epoch.load(Acquire) [taken BEFORE the revoke store]
                    revoke(); poll epoch.load(Acquire) != e0, bounded by timeout_ms
                    → only now: poison (memset 0xDE over all 3 buffers) or free
DESTROY  after poison verification (L7) or immediately if not poisoning
```

Why the ACK is load-bearing: between the writer's revocation check and its envelope write there is a window; freeing in that window is a use-after-free **across FFI** — the crash class that motivated I6. The ACK closes it: reclaim's Acquire poll observes the ACK's fetch_add, which is ordered after the writer's final byte write. Poison-before-ACK is the bug; poison-after-ACK is the protocol.

`DROPPED_REVOKED` is sticky: every publish after the first ACK returns `DROPPED_REVOKED`. "Within one publish" is structural.

## 7. LOC budgets and dependencies

| Kernel | Budget | Dependencies | Notes |
|---|---|---|---|
| C (`weft.h` + `weft.c`) | ~600 | C11, pthread **not required by kernel**, `stdatomic.h`, `posix_memalign` | threads belong to the runner |
| Rust | ~600 | `std` + `core::sync::atomic` only; **zero crates** (offline builds) | internal `unsafe` only for payload slices, each with a `SAFETY:` comment citing RFC-0001 §4 |
| TypeScript | ~700 | Node stdlib only (`worker_threads` in the runner, not the kernel) | one `SharedArrayBuffer` per Weft: control block (Int32Array) + data region |

Exceeding a budget by >20% is a review trigger, not a blocker — but treat it as a signal you are re-deriving the withdrawn design.
