# D-32 REPORT: weft-cluster — the Kernel-Bypass Transport Pillar (Pillar 3 / RFC-0019)

- **Directive**: Pillar 3 — "weft-cluster Kernel-Bypass Drivers, RoCEv2/RDMA
  & eBPF XDP Mesh" (the lead's mandate: dlopen'd libibverbs one-sided RDMA,
  XDP line-rate ingestion with BPF steering, io_uring batched fallback,
  < 500 ns RDMA synchronization harness with the Law-1 malloc-audit)
- **Status**: COMPLETED / PASS (4 transport engines + WCR1/WCF1 seam
  contracts + fabric cascade + latency scoreboard; 142 CL-series gates green
  in -O2, ASAN legs clean, kernel freeze byte-verified)
- **Date**: 2026-09-20
- **Environment**: `x86_64-sandbox` (gcc 14.2.0) · kernel
  `5.10.134-013.15.kangaroo.al8.x86_64` (io_uring LIVE — setup/enter/
  REGISTER_BUFFERS/WRITE_FIXED/READ_FIXED functional, features 0x7ff;
  SEND_ZC opcode refused `-EINVAL`, the honest < 5.19 verdict) · **zero
  effective capabilities** (CapEff = 0: no CAP_BPF/CAP_NET_ADMIN/
  CAP_NET_RAW — the XDP legs exercise their named PERMS refusals) · no
  rdma-core/libibverbs (the RDMA library leg refuses by name) ·
  RLIMIT_MEMLOCK = 64 KiB (bench regions sized under the floor)
- **Reference**: RFC-0019; builds on the Series-10 substrate (RFC-0016 §3
  xdp_rx UMEM precedent, RFC-0012 uring ladder), Pillar 2's seam discipline
  (`weft_tensor_view`), and Engineer 1's WCR1 ring contract

---

## 1. Executive Summary

Pillar 3 closes the machine boundary. One 40-byte frozen region view
(`weft_wcr1_region_t`) describes Engineer 1's ring memory to every
transport; one 64-byte frame header (WCF1, FNV-1a double-entered)
describes what crosses the wire; four engines move the bytes under the
four Laws:

- **RDMA** — libibverbs dlopen'd into a vtable (zero link-time deps), the
  provider ABI mirrored and static-asserted against the real verbs.h
  wherever rdma-core exists (`WEFT_RDMA_VERIFY_ABI`), QP ladder
  INIT→RTR→RTS with bounded rnr_retry, the WRH1 rkey handshake (128-byte
  golden-tested wire format), a pre-allocated WR pool, deadline-bounded
  CQ polling. The mock battery is the executable spec (state-machine
  order, one-sided opcodes, remote-address algebra); the real library's
  absence is a first-class gate.
- **XDP** — the .weft steering filter exists as three mirrors kept honest
  by gates: the kernel C source, the ~40-instruction eBPF program the
  driver ASSEMBLES at init (no clang/libbpf on the node), and the
  user-space validator. The filter strips 42 bytes of headers so the NIC
  DMA lands WCF1 at chunk offset zero — the same placement law as every
  other road.
- **io_uring** — raw syscalls, registered per-chunk buffers, and the ZC
  ladder: SEND_ZC attempted, refused by this kernel (-EINVAL CQE),
  downgraded **stickily** to WRITE_FIXED with the shadowed frame
  RESUBMITTED (capability discovery costs a syscall, never a frame) and
  the `[FALLBACK-COPY]` label in every report.
- **loopback + fabric** — the in-process ground truth and the refusal
  cascade that routes every node to the best road it honestly deserves,
  printing the full chain of named refusals when that road is loopback.

The scoreboard on this runner: **loopback p50 73 ns** (one labeled copy,
0 allocs), **io_uring kernel-UDP ping-pong p50 3.4 µs RTT** (registered
buffers LIVE, 0 allocs, one-way ≈ RTT/2 labeled estimate), **RDMA row
HARDWARE-DEFERRED** with the harness ready — never a simulated number.

## 2. Mandate Scoreboard

| Deliverable | What landed | Measured (this runner) | Status |
| :--- | :--- | :--- | :---: |
| **A. RoCEv2/IB one-sided RDMA engine** | `backends/rdma/`: dlopen'd vtable, ABI mirror + `WEFT_RDMA_VERIFY_ABI` drift tripwire, PD/CQ/QP lifecycle, WRH1 handshake (encode/decode pure + golden), WR pool, bounded poll, error WC names | 45 CL-R gates via the mock vtable (QP ladder order, one-sided opcode + address algebra, timeout law, pool BUSY bound, zero-alloc window); the real-library refusal gated by name (CL-R1) | **PASS** (hardware leg declared) |
| **B. eBPF/XDP line-rate ingestion** | `backends/xdp/`: raw bpf(2) wrappers (no libbpf), init-time bytecode assembler with label back-patching + u31 law, XSK/UMEM over the WCR1 span, fill/rx rings with NEED_WAKEUP, free-chunk stack, defense-in-depth WCF1 validation, `bpf/weft_xdp_filter.c` reference source | 35 CL-X gates (golden vectors on all 10 WCF1 rungs, assembler structure: jump targets/helpers/immediates/map-fd pseudo-loads, schema block ±2 insns, placement + pow2 laws); PERMS refusal with the named capability reason (CL-X1/X10) | **PASS** (LIVE leg hardware-gated) |
| **C. io_uring batched fallback** | `backends/uring/`: raw syscalls, per-chunk registered buffers (≤1024), TX ladder SEND_ZC→WRITE_FIXED→SEND with sticky downgrade + frame-preserving resubmit, RX ladder READ_FIXED→READ, deadline-bounded harvest, user_data kind-tag law | 41 CL-U gates — **REAL kernel UDP ping-pong end-to-end** (payload bit-identical both ways), registered=1, ZC refused → labeled `[FALLBACK-COPY]` downgrade (CL-U6), timeout law (CL-U7), **0 allocs** across send+flush+poll (CL-U8) | **PASS** |
| **D. Latency benchmark + audit harness** | `bench/weft-cluster-bench`: probe-first fabric chain, three roads, p50/p95/p99 from a pre-allocated sample array, malloc-audit window, `--audit-strict`, JSON evidence | loopback p50 **73 ns** / p99 75 ns; uring RTT p50 **3.4 µs** / p95 6.4 µs / p99 7.3 µs (2000 samples each, **0 allocs both roads**); RDMA row HARDWARE-DEFERRED with the on-HCA instructions; verdict PASS | **PASS** |
| **E. Fabric + refusal cascade** | `fabric/`: side-effect-free probes in preference order, the chain string (per-engine verdict + reason + status), best-rung selection; `backends/loopback/` ground truth | 21 CL-F gates: chain names all four engines, rdma refuses `driver_absent`, xdp refuses `perms_refused` naming the capabilities, uring `rung=2` (FIXED), loopback always OK; best() = first available | **PASS** |
| **F. Contracts + seam** | `include/weft_wcr1.h` (40 B, static-asserted, WFSH bridge frozen to the RFC-0004 layout), `src/weft_cluster_frame.h` (64 B WCF1, FNV-1a double-entry, u31 XDP law), `weft_cluster_core` (status ladder, caps probe, dlopen-first, deadlines) | Region refusal ladder + WFSH bridge geometry asserted (CL-R3/F5); schema-hash double-entry green (CL-X4); WCF1 rungs all gated (CL-X3) | **PASS** |

## 3. The Gates (CL-series, 142 per leg + ASAN)

| Suite | Checks | Legs | Highlights |
| :--- | :--- | :--- | :--- |
| CL-R (rdma) | 45 | -O2 + ASAN | Real-library DRIVER refusal names the soname; mock QP ladder (3 modify_qp calls, no out-of-order); one-sided write algebra (sge/remote_addr exact); Law-2 stall → TIMEOUT in [18 ms, 5 s]; WR pool BUSY + drain; error WC named; WRH1 golden + 3 refusal rungs |
| CL-X (xdp) | 35 | -O2 + ASAN | All 10 WCF1 rungs on golden vectors; assembler: schema block = exactly +2 insns, jumps in range, helpers ⊆ {1,44,51}, both map-fd pseudo-loads patched, protocol immediates present (dport LE-swap, magic, frag-mask 0xFF3F, strip-42); u31 law; PERMS refusal carries the capability text |
| CL-U (uring) | 41 | -O2 + ASAN | Live rings (probe details the register rung); **real loopback ping-pong, payload bit-identical**; sticky ZC downgrade with the `[FALLBACK-COPY]` label + resubmitted frame; empty-poll TIMEOUT honors the deadline; **zero allocations** around send+flush+poll ×8; >1024-chunk registration refused by name; user_data bit-62 law |
| CL-F (fabric) | 21 | -O2 + ASAN | Preference order; chain names all engines with verdicts; rdma honest `driver_absent`; loopback always OK; best() = first available; loopback end-to-end bit-identical + `[FALLBACK-COPY]` label; FIFO overflow is bounded BUSY, never a drop |
| bench | 3 roads | O2 + audit | verdict PASS, 0 allocs on every runnable road, JSON evidence at `litmus/evidence/cluster/cl-bench.json` |
| meta | — | ASAN+freeze | ASAN/UBSAN clean on all three ASAN legs; core/c zero diffs vs the branch base (Law 3) |

## 4. Honesty Ledger

- **MEASURED here**: every gate above on this runner; the io_uring road
  end-to-end on real kernel UDP (registered buffers LIVE — the probe's
  NOP roundtrip + registration both succeeded; WRITE_FIXED + READ_FIXED
  confirmed at the opcode level by the diagnostic, then through the
  engine); loopback p50 73 ns; uring RTT p50 3.4 µs; zero hot-path
  allocations on both roads (the interposer window).
- **CI-GATED**: ASAN/UBSan legs; kernel freeze; evidence regeneration
  (`make -C tools/weft-cluster evidence`).
- **DECLARED / HARDWARE-DEFERRED**: RDMA provider behavior on real HCAs
  (the ABI mirror is assert-verified only where rdma-core headers exist;
  the sandbox carries the mock as the executable spec); XDP LIVE
  (CONFIG_XDP_SOCKETS + CAP_BPF/CAP_NET_ADMIN runners; the compiled
  identity of `bpf/weft_xdp_filter.c` is the deployment-side mirror
  check); SEND_ZC zero-copy semantics (kernel ≥ 5.19); the < 500 ns
  one-sided-write target (the harness ships; see §6).
- **Never claimed**: cross-node authentication (WCF1 steering keys are
  not ACLs); XDP TX; the unaligned-chunk UMEM encoding (refused by the
  placement law — RFC-0019 §4).

## 5. Engineering Notes (defects found and fixed, in-tree)

1. **The `-1` enum sentinel**: both capability probes used `static
   weft_xdp_cap_t g = -1;` with `if (g < 0)` — the enum's underlying
   type is unsigned, the guard was always false, and the probes returned
   cached garbage (empty detail strings, wrong rungs). Replaced with an
   explicit probed flag. Found by CL-U1's detail assertion.
2. **ZC discovery dropped the frame**: the < 5.19 verdict arrives as a
   CQE, not an enter error — the first SEND_ZC frame was lost while the
   receiver's poll starved. The engine now shadows the ZC attempt and
   poll_events resubmits it on the downgraded road (discovery costs a
   syscall, never a frame). Found by the opcode-level diagnostic
   (`scripts/uring_diag.c`, preserved): SEND_ZC → -EINVAL,
   WRITE_FIXED/READ_FIXED → live.
3. **The stale stats mirror**: `stats.tx_mode` recorded init-time mode
   and never followed the sticky downgrade — the bench would have
   printed SEND_ZC on a kernel that refused it. Synced at every
   downgrade; CL-U6 now gates the label, not the hope.
4. **The mock's provider-shape bug**: the fake `ibv_get_device_list`
   returned a sentinel pointer the driver dereferenced (list[0]) — the
   mock now returns a real array. The provider contract shape is part of
   what a mock must honor.

## 6. Hardware Verification Checklist (the DEFERRED legs)

| Leg | Runner requirement | Command | Expected |
| :--- | :--- | :--- | :--- |
| RDMA one-sided write | rdma-core installed, RoCEv2/IB HCA (ConnectX-5+ class) | `make -C tools/weft-cluster weft-cluster-bench && ./weft-cluster-bench --transport rdma --iters 10000` | post+bounded-poll completion p50 **< 500 ns** (the mandate budget); WR pool 0 allocs; HARDWARE-DEFERRED row flips to MEASURED |
| RDMA cross-node | two HCA nodes, WRH1 port 47912 reachable | node B: any listener; node A: `--transport rdma` + handshake client | one-sided writes land in B's WCR1 with **zero receiver wakeups** (verify: no `ibv_get_cq_event`, CPU idle) |
| XDP LIVE | CONFIG_XDP_SOCKETS kernel + CAP_BPF/CAP_NET_ADMIN + a uplink with XDP support | `sudo ./test-xdp` + `ip link set dev <if> xdp obj bpf/weft_xdp_filter.o sec xdp_weft` | SETUP→LIVE rung; CL-X skips flip to LIVE gates; line-rate steering with 0 rx syscalls |
| SEND_ZC | kernel ≥ 5.19 | `./weft-cluster-bench --transport uring` | tx_mode=SEND_ZC, notif CQEs counted, no [FALLBACK-COPY] label |
| ABI drift tripwire | any machine with `infiniband/verbs.h` | `make CFLAGS="-DWEFT_RDMA_VERIFY_ABI ..."` | static asserts burn any offset drift at compile time |

## 7. Reproduction

```
cd tools/weft-cluster
make all && ./test-rdma && ./test-xdp && ./test-uring && ./test-fabric
make asan
./weft-cluster-bench --iters 2000 --json /tmp/cl-bench.json
make evidence          # -> litmus/evidence/cluster/*
```

The full shard: `ci/scripts/run_weft_cluster_shard.sh` (registered in the
extreme matrix) — build, the four gate batteries, ASAN legs, the bench
with `--audit-strict`, the kernel-freeze check, and the evidence pack.
