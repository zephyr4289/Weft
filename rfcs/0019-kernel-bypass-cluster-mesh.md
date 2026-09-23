---
RFC: 0019
Title: Kernel-Bypass Cluster Mesh — weft-cluster (RDMA, XDP, io_uring)
Status: Draft
Authors: Senior Systems Engineer 2 (NPU/GPU Acceleration, Native Runtimes & Inference Engines)
Created: 2026-09-20
Supersedes / Superseded-by: stacks on RFC-0011 (WFSH IPC), RFC-0012 (uring ladder), RFC-0016 (heterogeneous mesh), RFC-0017 (tensor view seam)
---

# RFC 0019 — Kernel-Bypass Cluster Mesh: weft-cluster

## Summary

Weft's in-process memory discipline (zero copies, zero allocations, bounded
latency) currently stops at the machine boundary: the moment a frame must
reach another node it pays a full kernel network-stack traversal, at least
one staging copy, and a receiver-side wakeup. This RFC specifies
**weft-cluster**, the tools-layer transport substrate that moves WCR1 ring
chunks between nodes without those costs: a dlopen'd RoCEv2/InfiniBand
one-sided-RDMA engine (the receiver's CPU never wakes), an eBPF/XDP
line-rate steering filter that DMAs .weft frames straight into WCR1 UMEM
chunks, an io_uring batched-UDP fallback with registered buffers, and a
fabric selector whose refusal cascade routes every node to the best road it
honestly deserves — with the loopback engine as the always-available floor.

## Motivation

The cluster target is **< 500 ns cross-server memory synchronization**
without OS kernel intervention or broker hops. The measurable facts that
make the stock path unusable:

1. **Two copies + two stack traversals per message.** `send()` copies the
   payload into an skb; the receiver's `recv()` copies it out again — and
   both sides traverse the full TCP/IP stack (socket lock, softirq,
   scheduler wakeup). On commodity hardware this is 20–50 µs of floor per
   small message, two orders of magnitude over budget.
2. **Per-frame syscalls.** Even with the RFC-0012 ingestion ladder, the
   transmit side costs one syscall per frame (or per small batch); at
   100 Gbps line rate with 128-byte frames that is ~78M syscalls/second.
3. **Receiver wakeups destroy the latency floor.** Every UDP receive arms
   a softirq and potentially wakes a scheduler — the exact jitter the
   Weft Laws exist to remove.

Laws engaged: Law 1 (zero hot-path allocations — the transports' WR pools,
UMEM rings, and io_uring SQEs are all pre-allocated at init), Law 2
(bounded latency — every poll is deadline-bounded, QP retry policy is
bounded), Law 4 (honest boundaries — every capability refusal is probed,
named, and routed around, never guessed). Law 3 is honored by placement:
all code lives in `tools/weft-cluster/`; `core/c` is byte-frozen
(CI-gated).

## Guide-level explanation

A cluster node owns one **WCR1 region** (the frozen 40-byte view over
Engineer 1's ring memory — `include/weft_wcr1.h`). To send a frame, the
node writes a **WCF1 header** (64 bytes: cluster/schema steering keys,
placement offset, sequence) into a chunk and hands the chunk to a
transport engine:

- **RDMA** (`weft_rdma_driver`): the region is registered once
  (`ibv_reg_mr`); the peer's address/rkey arrive over a bounded WRH1 TCP
  handshake; each frame is an `IBV_WR_RDMA_WRITE` — the NIC DMAs the
  bytes directly into the peer's physical memory. The peer's CPU is never
  interrupted; its ring "just advances."
- **XDP** (`weft_xdp_driver`): an eBPF filter (assembled at init — no BPF
  toolchain on the node) matches cluster_id/schema_id at line rate in the
  NIC driver, strips the 42-byte L2/L3/L4 header, and steers the frame
  into the WCR1 UMEM chunk via AF_XDP. Ingestion costs zero syscalls in
  the steady state.
- **io_uring** (`weft_uring_driver`): chunks are pinned once as
  registered buffers; frames go out in batches (one `io_uring_enter` per
  batch) as `WRITE_FIXED`/`SEND_ZC` ops on a connected UDP socket. On
  kernels < 5.19 the ZC opcode is refused by the kernel and the engine
  downgrades stickily to `WRITE_FIXED`, labeled `[FALLBACK-COPY]`.
- **loopback** (`weft_loopback`): in-process ground truth with one
  memcpy, labeled `[FALLBACK-COPY]` by construction.

The **fabric** (`weft_cluster_fabric`) probes every engine in preference
order (rdma → xdp → uring → loopback) and prints the full refusal chain —
each engine's verdict, reason, and status code. A node that lands on
loopback knows exactly why.

`weft-cluster-bench` scores the roads: p50/p95/p99 RTT with the
malloc-audit interposer proving zero hot-path allocations, JSON evidence,
and honest HARDWARE-DEFERRED rows for engines the runner cannot exercise.

## Reference-level specification

### §2 The memory contracts

**WCR1 region (40 bytes, ABI v1)** — `include/weft_wcr1.h`:
`base` (page-aligned), `span`, `chunk0_offset`, `chunk_size` (multiple of
64), `chunk_count` (≥ 2), `node_id`, `flags`. Address law: chunk *k* at
`base + chunk0_offset + k*chunk_size`. Engines register `[base, span)`
with their kernel/hardware interface; the WFSH bridge
(`weft_wcr1_from_shm`) freezes the RFC-0004 layout
(`chunk0 = 64 + 16 + 8·slot_count`, chunk *k* = slot *k*'s payload).

**WCF1 frame header (64 bytes, LE, FNV-1a double-entry)** —
`src/weft_cluster_frame.h`: magic ".wft", version, hdr_len=64,
cluster_id (u31 on the XDP fast path — eBPF immediates are
sign-extended), schema_id, frame_seq, payload_len, timestamp_ns,
src/dst node, flags (ECHO/TS), wcr1_offset (destination chunk offset,
must be chunk-aligned — a mid-chunk landing tears Engineer 1's slot
protocol and is refused), 16 reserved zero bytes (nonzero on receive =
refusal). The layout string is hashed (`WEFT_WCF1_SCHEMA_HASH`) and
re-derived at runtime; the CL-F gate asserts the pair.

### §3 RDMA: one-sided writes over a dlopen'd provider

- **Loader discipline**: `libibverbs.so.1` (fallback `libibverbs.so`)
  dlopen'd at runtime; every symbol resolved into a vtable
  (`weft_rdma_abi.h`); a missing library/symbol is `WEFT_CLUSTER_E_DRIVER`
  — never a crash. Zero link-time dependencies.
- **ABI mirror**: the struct surface the driver passes by pointer
  (`ibv_qp_attr`, `ibv_send_wr`, `ibv_wc`, `ibv_mr`, the frozen QP head
  with `qp_num`@44) is mirrored in-tree and — on deployment machines with
  rdma-core installed — the `WEFT_RDMA_VERIFY_ABI` compile includes the
  REAL `infiniband/verbs.h` and static-asserts every frozen offset. A
  drifting rdma-core release fails the build, not a customer cluster.
- **QP pipeline**: create → INIT (REMOTE_WRITE access) → RTR (path MTU =
  min(both ends); min_rnr_timer 12) → RTS (timeout 14, retry 7,
  **rnr_retry 3 — bounded**, never the unbounded 7). The mock battery
  (CL-R2) enforces the ladder order; a second connect is a STATE refusal.
- **WRH1 handshake (128 bytes, LE)**: magic, version, lid, mtu, qpn, psn,
  rkey, mr_addr, mr_len, gid[16], chunk geometry, node_id, reserved-zero.
  Exchanged over TCP (client or accept road) under the config deadline
  (SO_RCVTIMEO — Law 2 applies to setup too). Encode/decode are pure and
  golden-tested (CL-R11).
- **Hot path**: pre-allocated WR pool (`max_outstanding` slots, each with
  its own SGE); `post_write` maps chunk geometry to `sge.addr` /
  `wr.rdma.remote_addr`, pool exhaustion is a BUSY refusal (bounded flow
  control — never an unbounded queue). `poll` busy-polls the CQ under a
  caller deadline; WC errors are named (`wr_flush_err` …) IO refusals.
  GID selection reads sysfs (`/sys/class/infiniband/*/ports/*/gids/*`),
  preferring the first non-link-local (RoCEv2) GID — no provider ABI
  involvement.

### §4 XDP: the three mirrors

The same parser exists three times, and the CL-X gates keep them from
drifting: the kernel-side C source (`backends/xdp/bpf/weft_xdp_filter.c`,
compiled with `clang -target bpf` on deployment runners), the embedded
bytecode the driver **assembles at init** (no clang, no libbpf, no ELF —
`weft_xdp_build_filter` emits ~40 eBPF instructions from the config with
label back-patching), and the user-space validator
(`weft_wcf1_validate`). Fast-path decisions: Eth/IPv4(IHL=5)/no-frag/UDP/
dport/".wft"/version/cluster/schema; everything else — VLAN, options,
fragments — PASSES to the stack where the io_uring road ingests it
correctly at a slower rate. On match: `bpf_xdp_adjust_head(ctx, 42)`
strips the L2/L3/L4 header so **the chunk receives WCF1 at offset zero —
the uniform placement law across all transports** — then XSKMAP lookup +
`bpf_redirect_map` steers into the UMEM.

The frag-check mask deserves its derivation (the one non-obvious
constant): the BE `flags:frag_off` u16 loads as a swapped LE value; frag
offset + MF bits map to LE bits {0–5, 8–15} minus DF/reserved — mask
`0xFF3F`. DF-set is allowed (we don't fragment; we just refuse to be a
fragment).

**UMEM placement law** (stricter than the WCR1 floor): the XDP road
requires `chunk0_offset` and `chunk_size` to be page multiples — the
steering ring is a dedicated cluster region, not a bridged fanout session
(RDMA/io_uring accept the bridge; XDP refuses it by name). Fill-ring
addresses are page-aligned raw offsets; the unaligned-chunk encoding
degenerates to the plain offset, which is exactly why the law exists:
no fragile encodings on the hot path.

**Capability ladder**: NONE (no CAP_BPF/CAP_NET_ADMIN — the common cloud
state, an honest PERMS refusal) → SETUP (xsk + UMEM + rings) → LIVE
(filter attached + XSKMAP bound). Attach uses `BPF_LINK_CREATE`; on
kernels without bpf_link XDP the refusal carries the manual `ip link set
dev <if> xdp fd <n>` instruction. The rx path is deadline-bounded,
recycles through a pre-allocated free-chunk stack, and validates every
frame a second time (defense in depth + the exact-length law
`desc.len == 64 + payload_len`).

### §5 io_uring: the batched fallback with a ZC ladder

Raw syscalls (`SYS_io_uring_setup/enter/register`), the RFC-0012 house
rule — no liburing, every refusal named. TX ladder:
`SEND_ZC`+fixed (≥ 5.19, zero-copy) → `WRITE_FIXED` (5.1+, pre-pinned,
one skb copy, labeled `[FALLBACK-COPY]`) → `SEND` (universal). RX ladder:
`READ_FIXED` → `READ`. The ZC verdict arrives as a CQE (`-EINVAL`), not
an enter error: the engine shadows the first ZC send, downgrades
**stickily**, and **resubmits the shadowed frame** — capability discovery
costs a syscall, never a frame. Registered buffers pin per-chunk iovecs
(≤ 1024, the uapi bound); an RLIMIT_MEMLOCK refusal degrades to the
plain road with the label in the error string. Every wait is
deadline-bounded by userspace polling (kernel 5.10 lacks
`IORING_ENTER_EXT_ARG`; the ≥ 5.11 timeout arg is the documented
improvement). user_data bit 62 is the driver's RX kind tag; callers
choosing it are refused by name.

### §6 The fabric cascade

`weft_fabric_probe_all` runs each engine's side-effect-free probe in
preference order; `weft_fabric_chain` renders the verdict per engine
(available / refused + reason + status code); `weft_fabric_best` selects
the first available rung. The chain string is the ops artifact: a node on
loopback shows the full ladder of refusals that put it there.

### §7 The bench

`weft-cluster-bench` scores loopback (in-process), uring (real kernel UDP
ping-pong over 127.0.0.1, RTT with a labeled RTT/2 one-way estimate —
true one-way needs PTP/hardware timestamping, out of scope), and rdma
(the one-sided write harness: post + bounded CQ poll; self-loopback
through the HCA when present, honest HARDWARE-DEFERRED row when not).
Percentiles from a pre-allocated sample array (allocation-free steady
state, qsort only after disarm), the malloc-audit window around the
timed loop, `--audit-strict` fails on any allocation, JSON evidence.

**Invariants touched**: none in the fanout kernel (I1–I8 unchanged —
Law 3); the WCR1 region view and WCF1 header are NEW frozen contracts
(static-asserted, hash double-entry). **Litmus impact**: the new CL-series
(142 gates: CL-R 45, CL-X 35, CL-U 41, CL-F 21) plus ASAN legs;
`litmus/evidence/cluster/*` is the evidence pack. **Envelope impact**:
additive (tools layer + one workflow registration); no existing API
changes.

## Boundary of the claim (Law 4)

- **MEASURED here** (the x86_64 CI/sandbox, kernel 5.10.134): the full
  CL-series via the mock vtable and the real (absent) library refusals;
  the io_uring road end-to-end on real kernel UDP (registered buffers
  LIVE, WRITE_FIXED after the sticky ZC downgrade, payload bit-identical,
  zero allocations in the audit window); the loopback ground truth; the
  fabric cascade on a capability-less runner.
- **CI-GATED**: ASAN/UBSAN legs; kernel freeze (core/c zero diffs);
  evidence regeneration.
- **DECLARED / HARDWARE-DEFERRED**: RDMA numbers (no HCA in the sandbox —
  the harness ships ready; D-32 §6 is the on-hardware checklist: install
  rdma-core, run `weft-cluster-bench --transport rdma`, expect the
  sub-µs post+poll completion on a ConnectX-5+ class NIC); XDP LIVE legs
  (need CAP_BPF/CAP_NET_ADMIN and CONFIG_XDP_SOCKETS; the BPF source's
  compiled-object identity is the deployment-side mirror check); SEND_ZC
  zero-copy TX (kernel ≥ 5.19).
- **Not claimed**: multi-node security (WCF1 carries no authentication —
  cluster_id is a steering key, not an ACL; a hostile peer with the rkey
  can write the MR — cluster fabrics are assumed trusted-link, the RFC
  notes the boundary); XDP TX (egress steering is out of scope); the
  unaligned-chunk UMEM encoding (deliberately refused — see §4).

## Alternatives considered

- **Link libibverbs (and liburing) directly.** Loses the mandate's
  explicit dlopen discipline and the capability honesty: a hard link
  makes "no HCA" a load-time crash instead of a routed refusal. The
  mirror-ABI burden is the price; the WEFT_RDMA_VERIFY_ABI compile pays
  it automatically where the real headers exist.
- **libbpf for the XDP filter.** Adds an ELF loader + a link-time or
  runtime dependency to solve a problem the init-time assembler solves in
  ~120 lines — and the assembler's output is structurally gate-checked
  (CL-X5–X9) instead of trusted. The C source remains the deployment
  reference for toolchain-rich environments.
- **RDMA CM (rdma_cm) for connection establishment.** Another library,
  another event model, and the WRH1 exchange needs 128 bytes over any
  bounded channel — a raw TCP accept/connect under SO_RCVTIMEO is the
  entire requirement.
- **Do nothing (io_uring only).** Keeps 20–50 µs floors and per-frame
  receive wakeups; the < 500 ns mandate is unreachable without one-sided
  writes.

## Drawbacks

- The mirror ABI is a maintenance surface (mitigated: static-asserted
  against the real header wherever rdma-core is installed, and the mock
  battery is the executable spec).
- The XDP placement law excludes bridged fanout sessions from the
  line-rate road (they ride io_uring instead) — honest narrowness.
- Busy-poll CQ harvesting burns CPU on the RDMA runner (the bounded-spin
  choice for determinism; interrupt-mode `ibv_get_cq_event` is the
  documented throughput-oriented alternative).
- The fabric adds a probe layer a single-transport deployment does not
  need (it is 300 lines and side-effect-free; the cascade is the point).

## Open questions

- Multicast one-sided writes (IBV_WR_RDMA_WRITE to a multicast QP) for
  fan-out across N nodes — the natural cluster shape, deferred until
  Engineer 1's WCR1 replication contract lands.
- Whether the WRH1 exchange should ride the existing Triad trust fabric
  (hmac'd channel) instead of raw TCP — currently out of scope, noted in
  the boundary.
- ZC notification CQE coalescing semantics on ≥ 5.19 kernels (two CQEs
  per send) — the engine counts notifs; throughput-oriented batching
  may want the `IORING_SEND_ZC_REPORT_NOTIF` flag tuned.

## Implementation plan

Owner: Senior Systems Engineer 2 (this pillar). Deliverables landed on
`feat/weft-cluster-bypass` (stacks on `feat/weft-tensor-accelerators`):

- `tools/weft-cluster/` — the seams (§2), four engines (§3–§6), the
  bench (§7), the CL-series gates, the audit interposer
- `rfcs/0019-kernel-bypass-cluster-mesh.md` (this document),
  `reports/D-32-WEFT-CLUSTER-NATIVE.md` (verification + benchmarks)
- `ci/scripts/run_weft_cluster_shard.sh` + extreme-matrix registration
- `litmus/evidence/cluster/*` — the evidence pack

Acceptance: CL-series green in `-O2` and ASAN, kernel freeze byte-verified,
bench verdict PASS with zero hot-path allocations on every runnable road.
Status flips to **Implemented** when the Lead merges the branch; the
HARDWARE-DEFERRED legs (RDMA NIC, XDP caps, ≥ 5.19 ZC) flip D-32's
checklist rows when the corresponding runners exist.

---

*Process notes: lazy consensus, 7 days — silence is consent; a substantiated
objection cites a Law, an invariant, or a litmus test. See
[CONTRIBUTING.md](../CONTRIBUTING.md) §3 and [GOVERNANCE.md](../GOVERNANCE.md).*
