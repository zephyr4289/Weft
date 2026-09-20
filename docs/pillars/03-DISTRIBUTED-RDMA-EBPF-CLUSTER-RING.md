# 🏛️ Pillar 3: `weft-cluster` Sub-Microsecond Distributed RDMA & eBPF Mesh

> **Target Paradigm:** Obliterate Kafka, NATS, ZeroMQ, and WebSockets in ultra-low-latency distributed clusters.  
> **Core Metric:** **< 500 ns Cross-Server Memory Synchronization** (One-sided RoCEv2 RDMA writes).

---

## 1. Problem Statement & Motivation

Traditional message queues and distributed event brokers suffer from immense overhead:
- **TCP/IP Stack Overheads:** Context switches, syscalls, kernel buffer copies (`sk_buff`), interrupt handling, and TCP handshake/ack packet roundtrips.
- **Broker Hops:** Producer $\to$ Network $\to$ Broker (Kafka/NATS) $\to$ Disk / Memory Queue $\to$ Consumer. Latencies range from $2\,\text{ms}$ to $50\,\text{ms}$.

**The `weft-cluster` Solution:**
Direct kernel-bypass and hardware-accelerated memory fabric:
- **In Data Centers:** One-sided **RDMA (RoCEv2 / InfiniBand)** writes data from Server A's physical memory directly into Server B's ring buffer in $< 500\,\text{ns}$ without CPU involvement on the receiver.
- **On Standard Linux Servers:** Kernel-bypass using **`io_uring`** and **eBPF XDP (eXpress Data Path)** to route packets straight out of the NIC ring buffer.

---

## 2. Distributed Mesh Topology

```
┌────────────────────────────────────────────────────────┐
│                        NODE A                          │
│  ┌───────────────────────┐   ┌──────────────────────┐  │
│  │ User Space Producer   │   │ Local SHM Ring Buffer│  │
│  └───────────┬───────────┘   └──────────┬───────────┘  │
└──────────────┼──────────────────────────┼──────────────┘
               │ Direct Memory Write      │
               ▼                          ▼
      ┌──────────────────────────────────────────────┐
      │  RoCEv2 / InfiniBand Hardware NIC (RDMA)     │
      └──────────────────────┬───────────────────────┘
                             │
                             │ Hardware Fiber Link (<500ns)
                             │
      ┌──────────────────────▼───────────────────────┐
      │  Remote Hardware NIC (One-Sided RDMA Write)  │
      └──────────────────────┬───────────────────────┘
                             │ Direct DMA into Host Memory (No CPU Interrupt)
┌────────────────────────────┼───────────────────────────┐
│                        NODE B                          │
│              ┌─────────────▼─────────────┐             │
│              │ Remote SHM Ring Buffer    │             │
│              └─────────────┬─────────────┘             │
│                            │ 0-Copy L1 Read            │
│              ┌─────────────▼─────────────┐             │
│              │ User Space Consumer       │             │
│              └───────────────────────────┘             │
└────────────────────────────────────────────────────────┘
```

---

## 3. Core Protocols & Guarantees

1. **One-Sided Memory Sync:** Node A executes `RDMA_WRITE` to Node B's pre-registered memory region (`ibv_reg_mr`). Node B's CPU does not wake up until a watermarked notification arrives.
2. **eBPF / XDP Fast Path:** On non-RDMA hardware, eBPF XDP programs on the network interface parse and validate packet headers at line rate (100 Gbps) and DMA payloads directly into user-space ring buffers.
3. **Consensus & Heartbeats (Raft / CRDT on SHM):** Lock-free epoch counters embedded directly into the ring buffer header resolve partition conflicts and split-brain scenarios deterministically.
4. **Zero-Copy Serialization (`weftc` Native):** Every cluster payload is a frozen `.weft` struct, meaning the receiving node operates on incoming network bytes immediately without deserialization.

---

## 4. Work Distribution (Swarm-by-Layer)

- **Senior Engineer 1 (Protocol Engine & Lock-Free Multi-Node Ring):**
  - Distributed circular queue synchronization protocol and sequence monotonic sequencing.
  - Split-brain protection, lease timeouts, and leader election on shared memory rings.
- **Senior Engineer 2 (Kernel Bypass & Hardware Drivers):**
  - libibverbs / RoCEv2 one-sided RDMA driver integration.
  - Linux `io_uring` batching and eBPF XDP network filters.
- **Senior Engineer 3 (High-Level Cluster APIs & Observability):**
  - Multi-node clustering CLI and cluster topology manager.
  - Distributed tracing integrations with Perfetto and Prometheus.
