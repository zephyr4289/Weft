# weft-cluster — the kernel-bypass cluster pillar (Pillar 3 / RFC-0019)

Sub-microsecond cross-node memory synchronization for WCR1 ring memory:
RoCEv2/InfiniBand one-sided RDMA, eBPF/XDP line-rate steering, io_uring
batched UDP, and an in-process loopback floor — selected by an honest
refusal cascade.

```
[ Node A: WCR1 ring ] -> [ rdma | xdp | uring | loopback ] -> [ Node B: WCR1 ring ]
                          one-sided writes / NIC DMA / batched
                          syscalls / one labeled copy
```

## Layout

| Path | What |
| :--- | :--- |
| `include/weft_wcr1.h` | the 40-byte region contract (Engineer 1's ring, frozen) |
| `src/weft_cluster_frame.{h,c}` | the 64-byte WCF1 wire header (FNV-1a double-entry) |
| `src/weft_cluster_core.{h,c}` | status ladder, caps probe, dlopen-first, deadlines |
| `backends/rdma/` | dlopen'd libibverbs engine + the mirrored ABI (+ `WEFT_RDMA_VERIFY_ABI`) |
| `backends/xdp/` | raw bpf(2) + the filter ASSEMBLER + XSK/UMEM plumbing; `bpf/` holds the kernel-side C reference |
| `backends/uring/` | raw-syscall io_uring transport (registered buffers, ZC ladder) |
| `backends/loopback/` | the in-process ground truth ([FALLBACK-COPY] by construction) |
| `fabric/` | the refusal cascade + best-rung selection |
| `bench/` | `weft-cluster-bench` — the multi-road latency scoreboard |
| `tests/` | the CL-series gates + the Law-1 malloc-audit interposer |

## Quick start

```
make all && ./test-rdma && ./test-xdp && ./test-uring && ./test-fabric
make asan
./weft-cluster-bench --iters 2000 --json /tmp/cl.json   # add --audit-strict
make evidence                                             # -> litmus/evidence/cluster/
```

Every engine refuses honestly when its hardware/capabilities are absent
(the sandbox state: no HCA, no CAP_BPF) and the fabric prints the full
chain — a node on loopback knows exactly why. On an HCA runner:

```
./weft-cluster-bench --transport rdma --iters 10000   # the < 500 ns harness
```

Docs: `rfcs/0019-kernel-bypass-cluster-mesh.md` (architecture + memory
contracts), `reports/D-32-WEFT-CLUSTER-NATIVE.md` (verification +
benchmarks + the hardware checklist). Shard:
`ci/scripts/run_weft_cluster_shard.sh` (extreme matrix: `weft-cluster`).
