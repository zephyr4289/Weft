# D-31 — WEFT-CLUSTER-CORE: Technical Audit Report & Benchmark Scoreboard

**RFC:** 0018 (WCR1 — Weft Cluster Ring v1)
**Branch:** `feat/weft-cluster-core` (off `origin/main` @ `cdf2b0c`)
**Scope:** Pillar 3, Senior Systems Engineer 1 — protocol engine, memory
layout, multi-node ring, distributed consensus
**Status:** All gates green (plain / ASAN / TSAN); evidence committed under
`litmus/evidence/cluster/`

---

## 1. Executive summary

WCR1 lands the distributed fabric foundation: a **128-byte ring header +
64-byte slot header** (little-endian, cacheline-bracketed, `ibv_reg_mr`
ready), a **two-store atomic publish protocol** whose commit point is a
single DMA write completion, a **wait-free bounded-retry consumer** with
tear/overrun/alias detection, **in-band backpressure** (no ACKs, no
brokers), and a **ring-embedded Raft** — elections, leases, term epochs,
fencing tokens, quorum loss, eviction — with deterministic split-brain
arbitration and zero external coordination services.

Headline measured numbers (protocol engine over the local transport —
§5 methodology):

| Operation | ns/op | Notes |
|---|---|---|
| Two-store commit + stable read | **3.9** | the core primitive |
| Acquire + watermark consume | **5.0** | wait-free consumer path |
| Publish, 128 B slot | **31.0** | 1M ops, zero alloc |
| Publish, 192 B slot | **35.3** | Pillar-2-tensor-sized |
| Roundtrip publish+acquire+consume | **29.6** | |
| **5-node cluster-sync (per message)** | **38.3** | leader publish + follower poll, full stack |
| Consensus block read (stable) | **3.0** | 2M ops |
| Consensus write+read cycle | **12.0** | leader refresh primitive |
| Election convergence (3/5/7 nodes) | 23.75 ms virtual | deterministic; 3.2–14.4 µs wall |

The engine's share of the <500 ns cross-server budget is **38 ns
(≈8%)**; the remainder is fabric physics (RC RDMA write ~
0.5–2 µs one-way on current hardware), which is Engineer 2's transport
layer riding the §5 seam of RFC 0018.

---

## 2. Deliverables vs mandate

| Mandate item | Delivered | Verification |
|---|---|---|
| WCR1 128B ring header + 64B slot headers, magic/cluster_id/node_id/epoch/CRC-32 | `weft_cluster.h` (static-asserted offsets) | R2/R8 golden bytes |
| Pre-bracketed 64/128B-cacheline regions | `wcr1_ring_create` (128B align, mlock best-effort) | R2 |
| Monotonic sequence accounting + watermarked commits + remote offset calc (zero roundtrip RPCs) | `wcr1_publish` / `wcr1_consume` / `wcr1_slot_byte_offset` | R4/R9/R10 |
| Two-store atomic publish (hi/lo split), DMA-write-completion compatible | §3 of RFC 0018 | R5, R11 |
| Wait-free acquire + tear detection + bounded retry | `wcr1_acquire_next` | R5/R7, fork R11 |
| In-band backpressure markers in slot headers | `F_BP_MARK` + refusal ladder | R6, SB3 |
| Ring-embedded leader election / leases / term epochs | consensus engine | C1–C3, SB1/SB4 |
| Quorum + fencing tokens without zookeeper/etcd | §6 of RFC 0018 | C3/C4, SB3/SB4 |
| Node eviction + lease revocation on heartbeat timeout | §6.6 | C8, SB3 |
| `weft_cluster.h` / `weft_cluster_ring.c` / `weft_cluster_consensus.c` | `core/c/cluster/` | — |
| Test suite: ring, consensus, split-brain torture; leader election, partition healing, 100k+ zero-alloc | R/C/SB series | all green, evidence logs |
| CI shard script in the test matrix | `ci/scripts/run_weft_cluster_shard.sh` + `extreme-test.yml` | local full run PASS |
| RFC (normative) + audit report | `rfcs/0018-weft-cluster-protocol.md`, this file | — |
| Kernel freeze | `core/c/weft.{c,h}` untouched | empty diff vs origin/main |

---

## 3. Self-review: defects caught by my own gates before commit

The torture-first workflow earned its keep. Six protocol-level defects
were caught by the gates (each is now a regression test):

1. **Election abort on unreachable peers** — `start_election` returned
   `E_TRANSPORT` on the first partitioned peer and stopped campaigning.
   Elections must campaign *through* partitions; unreachable peers are
   now skipped and counted (`dropped_stores`). Caught by C3 (majority
   side never elected a leader).
2. **Slot headers bypassed the transport seam** — headers were written
   by direct LE puts into remote memory, invisible to the partition
   gate (and to Engineer 2's future ibverbs layer). Headers are now
   stack-built and shipped as one 64-byte transport write (also one
   fewer WR per publish). Caught by C3 diagnostics.
3. **Out-of-seqlock cflags election marker** — writing
   `ELECTION_OPEN` outside the heartbeat bracket broke the null-state
   invariant (`hb==0 ⇒ all-zero block`), burning readers' full retry
   budget during every candidacy window: 6 spurious
   `E_CONSENSUS_TORN` per boot, 18 across the torture. The marker was
   redundant (VOTE_REQUEST slots *are* the election signal on the data
   plane) and was removed. Caught by SB3's torn-read zero-invariant.
4. **Synchronized dueling candidates** — candidate retry used a flat
   one-lease window, destroying the node-id stagger: two candidates
   could re-campaign in lockstep forever. Retries now re-apply the
   staggered timeout. Caught by SB1/SB3 convergence failures.
5. **Missing Raft term gate on adoption** — a node that had seen term 5
   adopted a term-1 leader view (token-0 first-adoption path), letting
   the term ladder regress below promises already made. Adoption now
   requires `view.term >= local_term`. Caught by SB1's term-ladder
   assert.
6. **Eviction without a liveness echo** — header-only lease refreshes
   never advanced peer watermarks, so a quiet-but-alive peer would be
   falsely evicted. Leader refresh now publishes a header-only
   heartbeat slot whose consumption is the liveness signal. Caught by
   design review while writing C8.

Plus four test-authoring bugs the gates refused to bless (wrong
attach-error expectations, watermark parity poke, slot-offset golden
values, payload index vs ring-seq conflation — the last of which
documents a real property: control slots share the sequence space with
data, which the RFC now states explicitly).

---

## 4. Test matrix (all green)

**R-series (ring engine)** — R1 CRC/LE/two-store encoding golden values;
R2 create golden bytes; R3 validation ladder (12 distinct refusals);
R4 100k publish/acquire/consume zero-alloc; R5 two-store tear detection
(crashed-writer states); R6 in-band backpressure + ring-full refusal;
R7 overrun + message_seq alias guards; R8 slot golden layout + opt-in
CRC; R9 watermark protocol; R10 zero-roundtrip remote addressing; R11
**fork cross-process IPC over MAP_SHARED** — 100k messages, zero tears,
watermark convergence (ASAN leg exercised the honest torn-backoff path).

**C-series (consensus)** — C1 cold-boot election (deterministic node-1
leader, all converged); C2 lease stability across 20 lease periods
(same-token freshness path); C3 quorum loss (minority leader steps down,
majority elects, token strictly increases); C4 fencing refusals; C5
partition heal with per-tick invariants; C6 vote uniqueness
(wire-format injection); C7 clock-skew gate; C8 zombie eviction after 3
frozen leases; C9 consensus seqlock (stable/odd-torn/pairing-torn/null);
C10 **100k cluster sync ops, zero alloc, intact payloads**.

**SB-series (adversarial)** — SB1 dueling candidates (cross-grant tie,
third-candidate refusal, deterministic resolution, never two leaders);
SB2 byzantine same-term view injection (`E_SPLIT_BRAIN` surfaced,
lower-node-id winner, cluster intact); SB3 **7-node, 3000-round random
partition chaos**: 44,071 engine ops, 1,708 data publishes (1,701
delivered + 7 stranded at the honestly-evicted node's backlog — bounded
accounting proven), 24 elections, 13 quorum losses, 1 eviction, 1,591
link flips, 375 heals, **zero torn reads, zero unexpected error codes,
zero allocations, single leader at every single tick**, then full-heal
convergence; SB4 leader crash → re-election → fenced recovery.

Determinism: 3× byte-identical torture runs
(`sb-torture-determinism-3x.log`).

---

## 5. Benchmark scoreboard

Methodology: batch means (total time / ops) over the local direct-store
transport — the upper bound of engine overhead per operation; NIC/wire
time is additive in production. `calib` line = bare timing floor
(~0–1 ns/op, loop not optimized away — the checksum is consumed). All
runs on the dev sandbox (container, gcc -O2, x86-64); numbers are
indicative of the *protocol engine*, not the fabric.

```
{"mode":"calib","ops":1000000,"ns_per_op":0.00}
{"mode":"publish","slot":128,"ns_per_op":30.95}     # 1M ops
{"mode":"publish","slot":192,"ns_per_op":35.29}
{"mode":"publish","slot":512,"ns_per_op":47.47}
{"mode":"publish","slot":2048,"ns_per_op":192.93}
{"mode":"publish","slot":4096,"ns_per_op":549.54}   # bandwidth-bound
{"mode":"acquire+consume","ns_per_op":4.98}         # interleaved batches
{"mode":"roundtrip","ns_per_op":29.58}
{"mode":"seqreg","ns_per_op":3.92}                  # two-store commit+read, 10M ops
{"mode":"consensus-write+read","ns_per_op":11.99}
{"mode":"consensus-read","ns_per_op":2.96}
{"mode":"cluster-sync","nodes":5,"ns_per_op":38.33} # leader pub + follower poll
{"mode":"election","nodes":3,"virtual_ns":23750000,"wall_ns":3172}
{"mode":"election","nodes":5,"virtual_ns":23750000,"wall_ns":6775}
{"mode":"election","nodes":7,"virtual_ns":23750000,"wall_ns":14378}
```

Honest framing:

- **publish @4 KB (549 ns)** is memory-bandwidth-bound, not
  protocol-bound — the two-store commit itself is 3.9 ns; slot copies
  dominate above ~2 KB. At tensor-representative 192 B slots the whole
  publish is 35 ns.
- **acquire+consume 5.0 ns** vs **roundtrip 29.6 ns**: the roundtrip
  includes a publish; the consumer side is nearly free — consistent
  with the one-sided-write design goal (producers pay, consumers fly).
- **cluster-sync 38.3 ns** is the engine's share of the <500 ns
  cross-server north star: ~8%. The RC-QP one-sided write (~0.5–2 µs
  on current RoCEv2 hardware) dominates the physical budget — which is
  why the protocol is engineered so the engine adds almost nothing to
  it.
- **Election wall time is µs-scale for a full N-node simulated
  election** (virtual convergence is 2 leases + tick granularity by the
  deterministic timeout ladder — 23.75 ms at the default 10 ms lease;
  tunable down via cfg).

Full logs: `litmus/evidence/cluster/cluster-bench.log`,
`shard-full.log`.

---

## 6. Compliance proofs

- **Kernel freeze:** `git diff origin/main -- core/c/weft.c core/c/weft.h`
  → empty. All code under `core/c/cluster/` (+ Makefile targets, CI
  shard, workflow matrix entry, RFC, this report, evidence logs).
- **Law 1:** hook-counted zero allocations across R4 (100k), C10 (100k),
  SB3 (44k chaos ops + 1.7k publishes).
- **Law 2:** bounded loops throughout (statically visible retry caps);
  LE wire formatting explicit; determinism proven 3×.
- **Law 4:** 36-code named ladder; torture whitelist proves no
  unexpected code escapes; every refusal class in the matrix is
  exercised by at least one test.

---

## 7. Boundaries and follow-ups (honest, declared)

- **Transport is simulated at the store level** — the seam contract
  (issue-order, WR atomicity, delivery accounting) is specified and
  gated, but real RoCEv2 verbs integration is Engineer 2's deliverable.
  The torture's event-granularity partition model matches RC-QP
  semantics by construction.
- **Membership/reconfiguration (epoch bump, quorum resize)** — scaffolding
  and refusals in place, protocol deliberately out of v1 scope.
- **Single consumer thread per hosted ring** (v1 threading contract);
  multi-consumer coordination composes with Pillar 2's local rings.
- **Byzantine resistance boundary:** conflicting same-term views are
  *detected and deterministically arbitrated* (SB2), not
  cryptographically authenticated — a producer that writes arbitrary
  blocks can manufacture views; authentication rides the transport
  (Engineer 2) or a future epoch-signed block.
- **Clock skew** requires PTP-class sync (250 µs bound enforced at
  attach); lease floor formula documented and validated.

---

## 8. Handoff notes for Engineer 2 / Engineer 3

- **Engineer 2 (kernel-bypass drivers):** implement
  `wcr1_transport_t` over RC QPs — one QP per (producer → hosted-ring)
  pair; map `store_buf` → one RDMA WRITE (header/payload), `store32/64`
  → inline RDMA WRITE; completion poller feeds `true`/`false` for
  quorum accounting. The engine already issues the two-store commit as
  the final two stores of every publish — no batching changes needed.
- **Engineer 3 (orchestration/observability):** `wcr1_stats_t` counters,
  `quorum_mask`, `cflags`, `heartbeat_seq`, slot flags and the
  consensus-view snapshot are the observability surface; everything is
  readable with plain local loads (documented field ownership).
