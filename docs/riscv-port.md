# RISC-V (RV64GC) Port — Issue #18-2

**Status: CI-GATED (QEMU user-mode) · hardware-deferred (no RV64 silicon in the evidence fleet yet)**
**Series 9 (D-21) · zero kernel/driver code changes required**

---

## 1. What was validated (and how)

The entire C core compiles for `riscv64-linux-gnu` (RV64GC: `rv64imafdc`,
A-extension 2p1, Zicsr/Zifencei) **without a single `#ifdef`** — the C11
atomics the protocol is written against map cleanly onto the A-extension,
and `posix_memalign(64)` satisfies the layout contract on glibc/RV64. The
portability work was therefore *verification*, not modification; this
document is the audit trail.

| Gate | Result | Evidence |
| :--- | :--- | :--- |
| Kernel litmus **L1–L8** under `qemu-riscv64` | **PASS (8/8)** | `evidence/D-21/riscv/litmus-L*.log` |
| Fan-out **F-series** (fenced acq/rel regime) | **PASS (396 checks)** | `evidence/D-21/riscv/fanout-f.log` |
| Fan-out **F-series** (all-seq_cst regime) | **PASS** | `evidence/D-21/riscv/fanout-f-seq.log` |
| Atomics/fence audit (code emission) | **PASS** (see §2) | `evidence/D-21/riscv/audit-rv.s` |
| Claim-copy dispatch on RV64 | **scalar** (honest: no RVV path — see §4) | dispatcher reports `scalar` |

CI: `ci/scripts/run_riscv_litmus_shard.sh` (workflow `riscv-port.yml`) runs
all of the above on every push/PR — the toolchain installs via
`gcc-riscv64-linux-gnu` + `qemu-user` on the runner.

## 2. The atomics/fence audit (why the proofs hold on RV64GC)

The memory-ordering proofs in `weft.h` (the kernel constitution) and
`fanout.h` (P1/P2 brackets) assume the C11 mapping emits *at least* the
ordered primitives the proofs name. The audit source
(`ci/riscv/audit_rv.c`) compiles each primitive and the emitted assembly
(`gcc 14.2, -O2`) was inspected:

| C11 primitive (the protocol's actual usage) | RV64GC emission | Verdict |
| :--- | :--- | :--- |
| `atomic_exchange_explicit(.., acq_rel)` — the kernel's ONE atomic, and `weft_fanout_claim`'s stamps | **`amoswap.d.aqrl`** — single instruction, both ordering bits | exactly the required RMW ordering |
| `atomic_store_explicit(.., release)` — publish stamps | plain `sd` preceded by `fence rw,w` | release = predecessor fence ✓ |
| `atomic_load_explicit(.., acquire)` — claim-side stamps | plain `ld` followed by `fence r,rw` | acquire = successor fence ✓ |
| `atomic_thread_fence(seq_cst)` — the P1/P2 brackets | `fence rw,rw` | full barrier ✓ |
| relaxed 32-bit payload words (fill/copy) | plain `lw`/`sw` | zero cost, as on x86/ARM ✓ |
| cache-line discipline | `posix_memalign(64)`; RV64 `DminLine` is 64 B on the application-class cores (CTR02); the slot-line alignment pass (#17-2) lands on 64 B here too | ✓ |

The all-seq_cst A/B regime (`-DWEFT_FANOUT_SEQ_CST=1`) passes the same
battery, so the cheaper fenced-acq/rel default ships with both-regime
evidence on this architecture, matching the discipline of every other
supported target.

## 3. Reproducing (root and rootless)

**With root (CI, ubuntu-latest):**
```sh
apt-get install -y gcc-riscv64-linux-gnu qemu-user
bash ci/scripts/run_riscv_litmus_shard.sh
```

**Without root (evidence sandboxes)** — the recipe this port was validated
with: `apt-get download` the eleven debs (`gcc-riscv64-linux-gnu`,
`cpp-14-riscv64-linux-gnu`, `gcc-14-riscv64-linux-gnu{,-base}`,
`libgcc-14-dev-riscv64-cross`, `libgcc-s1-riscv64-cross`,
`binutils-riscv64-linux-gnu`, `libc6-dev-riscv64-cross`,
`libc6-riscv64-cross`, `linux-libc-dev-riscv64-cross`, `qemu-user`),
`dpkg-deb -x` each into a prefix (verify every deb by fully reading
`data.tar` — a truncated download produces binaries that SIGSEGV at exec
with zero diagnostics), then:

```sh
export PATH=$PFX/usr/bin:$PATH
export LD_LIBRARY_PATH=$PFX/usr/lib/x86_64-linux-gnu   # cross-binutils libs
# the sysroot's libc.so linker scripts hardcode /usr/riscv64-linux-gnu —
# sed them sysroot-relative:
sed -i 's|/usr/riscv64-linux-gnu/|/|g' $PFX/usr/riscv64-linux-gnu/lib/*.so
CC="riscv64-linux-gnu-gcc --sysroot=$PFX/usr/riscv64-linux-gnu"
RUN="qemu-riscv64 -L $PFX/usr/riscv64-linux-gnu"
$CC -O2 -std=c11 -pthread -D_GNU_SOURCE -o spike core/c/weft.c core/c/litmus_runner.c
$RUN ./spike L1-tear   # ... L2..L8
```

## 4. Honest boundaries

- **QEMU is not silicon.** User-mode emulation validates the instruction
  mapping, the C11 semantics, and the full protocol behavior — it does NOT
  validate RV64 memory-model subtleties at the hardware level (store
  buffers, fence timing costs). Real-hardware validation (e.g. Vision
  Five 2 / HiFive Unmatched / Jupiter class boards) is **declared
  follow-up**, exactly like the Apple-Silicon device-rate legs elsewhere
  in the fleet.
- **No RVV (vector) claim-copy path.** `fanout_simd` resolves **scalar**
  on RV64 — the RVV 1.0 intrinsics in gcc 14 are usable but the
  portability matrix (vlenb discovery, LMUL tuning, QEMU's RVV support
  maturity) makes an honest, evidence-backed RVV path a separate piece of
  work. The dispatch seam (issue #17-1) is where it would land.
- **32 B-line embedded parts**: the slot-alignment knob drops to 32
  (`WEFT_FANOUT_SLOT_ALIGN_BYTES=32`) — see the fanout.h guidance table.

## 5. Issue #18-2 acceptance mapping

| Criterion | Status |
| :--- | :--- |
| "All litmus tests pass on QEMU RISC-V" | **PASS** — L1–L8, plus F-series ×2 regimes beyond the ask |
| "No regressions on existing architectures" | PASS — zero shared-code changes; all x86_64 gates green (D-21 zero-regression log) |
| "Validate atomic operations (acq_rel fences)" | PASS — §2's audit is a CI gate, not a one-off note |
| "64-byte alignment works on RISC-V cache lines" | PASS — posix_memalign(64) + 64 B `DminLine` on app-class cores; FL-series alignment gate |
| "Document RISC-V-specific considerations" | **This document** (+ the rootless reproduction recipe) |
