// weft_spectrum.h — WSP5 (Weft Spectrum v1) universal hardware descriptor,
// zero-alloc micro-architecture probing surface, and dynamic tier governor.
//
// Scope (Pillar 5, Senior Systems Engineer 1 — core protocols, compiler &
// memory architecture; weft-spectrum Core Architecture & Hardware Governor):
//   - A byte-frozen, strictly little-endian, cache-line aligned hardware
//     profile (weft_hw_profile_t, exactly 256 bytes, 128-byte aligned) that
//     describes the silicon Weft is running on: CPU vector/matrix ISA,
//     cache-line geometry, core topology (P/E split, SMT, NUMA), GPU memory
//     architecture, NPU/DSP acceleration channels, RAM tier, thermal class
//     and frequency headroom.
//   - A frozen tier plan (weft_tier_plan_t, exactly 128 bytes, 64-byte
//     aligned) published by the runtime governor: ring lanes, slot strides,
//     frame deadlines, arena budgets and scheduler policy for Tier 1
//     (flagship 240 Hz), Tier 2 (mid-range 120 Hz) and Tier 3 (budget
//     60 Hz drop-not-queue).
//   - A lock-free governor (weft_governor_t) that recomputes the plan under
//     pressure transitions and publishes it through an atomic double-buffer
//     with a version sandwich: readers NEVER lock, NEVER block, NEVER
//     allocate, and NEVER observe a torn plan (bounded retries, refusal
//     with WEFT_SPECTRUM_EPLAN_BUSY otherwise — Law 4).
//
// Weft Core Laws (enforced here):
//   Law 1 (zero heap on hot path): probing, tier governance and all query
//        paths perform ZERO dynamic allocation — no malloc, no calloc, no
//        mmap churn, no GC-visible wrappers. Every descriptor lives on the
//        caller stack or inside statically aligned caller-owned arenas.
//        weft_spectrum_warmup() optionally pre-touches lazy libc caches
//        (sysconf/getauxval/stdio) ONCE at init-time so that the measured
//        runtime paths are provably allocation-free (CI ledger: 0 calls).
//   Law 2 (bounded, deterministic): the profile and plan are byte-frozen
//        with static-asserted offsets at every field; the tier score is
//        pure integer arithmetic on verified evidence only; every retry
//        loop has a compile-time-visible bound (SNAPSHOT_RETRIES = 64).
//   Law 3 (byte-frozen kernel): core/c/weft.{c,h} is untouched — the
//        spectrum engine is a sibling of the kernel, never a modification.
//   Law 4 (honest boundaries): capabilities are set ONLY on positive
//        evidence (OS-verifiable primitives or documented platform facts);
//        anything unverifiable stays CLEAR and is left to the driver layer
//        to promote via weft_hw_profile_promote_feature(). Nothing fails
//        silently: every refusal returns a unique named code from
//        weft_spectrum_status_t.
//
// Performance bar (normative, mission directive Pillar 5):
//   - weft_hw_has_feature()  : direct bitmask bit-test, <= 5 ns (inline).
//   - weft_get_active_tier() : single relaxed atomic load, <= 5 ns.
//   - weft_governor_snapshot(): 16 relaxed atomic word loads + version
//     sandwich; zero lock contention, zero blocking, zero allocation.
//   - Governor budget adjustments NEVER trigger blocking sweeps or torn
//     state reads during active 240 Hz render/ingestion loops: the writer
//     builds the new plan off to the side and publishes with two ordered
//     atomic stores; readers either see the old plan or the new plan.
//
// Endianness: WSP5 is frozen little-endian (all Pillar 5 silicon targets
// are LE). A big-endian host fails CLOSED at compile time (see below) —
// a BE wire port is deliberately out of scope for ABI v1.
//
// Threading / ownership contract (normative):
//   - weft_hw_profile_t is IMMUTABLE after seal: probe once at startup
//     (or mock-inject in tests), then share freely across threads. Driver
//     promotion (weft_hw_profile_promote_feature) MUST complete before the
//     profile is published to reader threads.
//   - weft_governor_t supports exactly ONE writer thread (the governance
//     thread calling weft_governor_retarget/force/set_refresh) and UNLIMITED
//     lock-free readers. Multiple writers are a contract violation.
//   - Probe source selection (weft_probe_set_source, internal surface) is
//     init-time only: set before the first weft_hw_probe() call, before the
//     process goes multithreaded.
//
// Handoff (mission directive §4):
//   - Engineer 2 (Native Drivers): weft_hw_profile_t + the inline feature
//     queries are the exact ABI surface for Qualcomm FastRPC, MediaTek
//     Neuropilot, Metal 3, CUDA and RISC-V RVV backends. Promote
//     loader-verifiable bits (Vulkan timeline semaphores, async compute,
//     timestamp queries) through weft_hw_profile_promote_feature().
//   - Engineer 3 (Managed Bindings): the C-ABI exports below
//     (weft_hw_probe / weft_governor_* / inline queries mirrored as
//     functions in weft_governor.c) are the binding surface for TypeScript,
//     Swift, Dart and Python telemetry.

#ifndef WEFT_SPECTRUM_H
#define WEFT_SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* ABI identity                                                        */
/* ------------------------------------------------------------------ */

#define WEFT_SPECTRUM_ABI_VERSION 1u

/* Memory bytes on a little-endian host: 'W','S','P','5'. */
#define WEFT_SPECTRUM_MAGIC       0x35505357u
#define WEFT_SPECTRUM_TAIL_MAGIC  (WEFT_SPECTRUM_MAGIC ^ 0xA5A5A5A5u)
/* Memory bytes: 'P','L','P','1' (tier plan). */
#define WEFT_SPECTRUM_PLAN_MAGIC  0x31504C50u
/* Memory bytes: 'G','W','S','1' (governor stats). */
#define WEFT_GOV_STATS_MAGIC      0x31535747u

/* FNV-1a 64 over the canonical layout string
 * "weft_spectrum:hw_profile:v1:le:frozen" — frozen ABI signature. */
#define WEFT_SPECTRUM_SCHEMA_HASH      0x449bfabaae13267cULL
/* FNV-1a 64 over "weft_spectrum:tier_plan:v1:le:frozen". */
#define WEFT_SPECTRUM_PLAN_SCHEMA_HASH 0xa0721bc5e692bdf9ULL

/* Big-endian hosts fail closed at compile time: WSP5 v1 is LE-frozen. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "weft-spectrum ABI v1 is little-endian frozen; BE port is v2 scope"
#endif

/* ------------------------------------------------------------------ */
/* Status ladder (Law 4: unique named refusal codes, nothing silent)   */
/* ------------------------------------------------------------------ */

typedef enum weft_spectrum_status {
    WEFT_SPECTRUM_OK = 0,        /* success                                  */
    WEFT_SPECTRUM_EINVAL,        /* null / out-of-range argument             */
    WEFT_SPECTRUM_ECHECKSUM,     /* profile CRC / tail magic failure         */
    WEFT_SPECTRUM_EABI,          /* magic / version / schema mismatch        */
    WEFT_SPECTRUM_EPLAN_BUSY,    /* snapshot retry bound exhausted — a       */
                                 /* REFUSAL, never a torn plan               */
    WEFT_SPECTRUM_ENOARCHETYPE   /* unknown mock archetype id (test surface) */
} weft_spectrum_status_t;

/* ------------------------------------------------------------------ */
/* Feature identifiers                                                 */
/*                                                                     */
/* A feature id indexes a 64-bit capability word:                       */
/*   word = id >> 6   (0..4),  bit = id & 63                            */
/* weft_hw_has_feature() is two shifts and one AND — no pointer         */
/* chasing, no locks, no branches beyond the bounds check.              */
/*                                                                     */
/* Capability discipline (fail-closed): a bit is SET only on positive   */
/* evidence (auxv hwcaps, cpuinfo intersection, sysfs/sysctl reads,     */
/* device nodes, or documented platform facts). A bit that CANNOT be    */
/* verified from pure C (Vulkan timeline semaphores, async compute      */
/* queues, timestamp queries, high-refresh panels) stays CLEAR until    */
/* the driver layer promotes it.                                       */
/* ------------------------------------------------------------------ */

#define WEFT_SPECTRUM_CAP_WORDS 5

/* Word 0 — CPU vector / matrix ISA (ids 0..63). */
enum weft_feature_cpu_isa {
    WEFT_F_CPU_NEON          = 0,  /* 128-bit NEON / ASIMD                   */
    WEFT_F_CPU_AVX2          = 1,  /* 256-bit AVX2                           */
    WEFT_F_CPU_AVX512F       = 2,  /* 512-bit AVX-512 foundation             */
    WEFT_F_CPU_SVE2          = 3,  /* SVE2 scalable vectors (VLEN >= 128b)   */
    WEFT_F_CPU_RVV_1_0       = 4,  /* RISC-V Vector 1.0 (VLEN >= 128b)       */
    WEFT_F_CPU_AMX           = 5,  /* Apple AMX / Intel AMX matrix engine    */
    WEFT_F_CPU_AVX_VNNI      = 6,  /* x86 VNNI int8 dot-product              */
    WEFT_F_CPU_AVX512_BF16   = 7,  /* AVX-512 BF16 dot-product               */
    WEFT_F_CPU_SVE2_BF16     = 8,  /* SVE2 BF16 (SVEBF16)                    */
    WEFT_F_CPU_RVV_VLEN_256  = 9,  /* RVV with VLEN >= 256 bits              */
    WEFT_F_CPU_LSE_ATOMICS   = 10, /* ARM LSE atomic instructions            */
    WEFT_F_CPU_CRC32         = 11, /* hw CRC32 (SSE4.2 / ARM CRC32)          */
    WEFT_F_CPU_CLMUL         = 12, /* carry-less multiply (PCLMULQDQ/PMULL)  */
    WEFT_F_CPU_AES           = 13  /* AES instructions (AESNI / FEAT_AES)    */
};

/* Word 1 — CPU system / topology (ids 64..127). */
enum weft_feature_cpu_sys {
    WEFT_F_CPU_SMT            = 64, /* hyper-threading / thread siblings      */
    WEFT_F_CPU_HETERO_CORES   = 65, /* big.LITTLE / P+E hybrid verified       */
    WEFT_F_CPU_128B_CACHELINE = 66, /* verified 128-byte coherency line       */
    WEFT_F_CPU_32B_CACHELINE  = 67, /* verified 32-byte coherency line        */
    WEFT_F_CPU_MULTI_NUMA     = 68, /* > 1 NUMA node verified                 */
    WEFT_F_CPU_FREQ_BOOST     = 69, /* verified boost headroom > 0            */
    WEFT_F_CPU_TSO            = 70  /* x86-style TSO memory model (fact)      */
};

/* Word 2 — GPU acceleration tier (ids 128..191). */
enum weft_feature_gpu {
    WEFT_F_GPU_PRESENT              = 128, /* any verified GPU               */
    WEFT_F_GPU_UNIFIED_MEMORY       = 129, /* unified SoC memory (fact/evid.)*/
    WEFT_F_GPU_DISCRETE_VRAM        = 130, /* discrete VRAM verified         */
    WEFT_F_GPU_DMABUF_IMPORT        = 131, /* dma-buf import path (Linux)    */
    WEFT_F_GPU_VULKAN_TIMELINE_SEM  = 132, /* driver-promoted only           */
    WEFT_F_GPU_METAL3_ARG_BUFFERS   = 133, /* Metal 3 (Darwin >= 22, arm64)  */
    WEFT_F_GPU_COMPUTE_SHADER       = 134, /* compute verified (Metal fact)  */
    WEFT_F_GPU_ASYNC_COMPUTE        = 135, /* driver-promoted only           */
    WEFT_F_GPU_TIMESTAMP_QUERIES    = 136  /* driver-promoted only           */
};

/* Word 3 — NPU / DSP acceleration (ids 192..255). */
enum weft_feature_npu {
    WEFT_F_NPU_PRESENT          = 192, /* any verified NPU/DSP               */
    WEFT_F_NPU_HEXAGON_FASTRPC  = 193, /* /dev/fastrpc-* channels            */
    WEFT_F_NPU_MTK_NEUROPILOT   = 194, /* MediaTek Neuropilot runtime        */
    WEFT_F_NPU_APPLE_ANE        = 195, /* Apple Neural Engine (platform fact)*/
    WEFT_F_NPU_ZERO_COPY_ARENA  = 196, /* zero-copy NPU arena verified       */
    WEFT_F_NPU_MULTI_CHANNEL    = 197  /* > 1 concurrent accelerator channel */
};

/* Word 4 — system constraints (ids 256..319). */
enum weft_feature_sys {
    WEFT_F_SYS_64BIT          = 256, /* LP64 host (compile-time fact)         */
    WEFT_F_SYS_LITTLE_ENDIAN  = 257, /* LE host (compile-time fact)           */
    WEFT_F_SYS_LOW_RAM        = 258, /* verified RAM < 6 GiB                  */
    WEFT_F_SYS_THERMAL_CAPPED = 259, /* battery / passive dissipation class   */
    WEFT_F_SYS_CONTAINERIZED  = 260, /* cgroup memory ceiling detected        */
    WEFT_F_SYS_HIGH_REFRESH   = 261, /* >= 120 Hz panel (driver-promoted)     */
    WEFT_F_SYS_ANDROID        = 262, /* Android OS (system property evidence) */
    WEFT_F_SYS_APPLE_OS       = 263, /* Darwin (sysctl evidence)              */
    WEFT_F_SYS_BARE_FALLBACK  = 264  /* zero rich sources answered            */
};

/* One past the last defined feature id. */
#define WEFT_FEATURE_COUNT 265

/* ------------------------------------------------------------------ */
/* Probe source acknowledgement bits (weft_hw_profile_t::probe_sources_ok) */
/* ------------------------------------------------------------------ */

enum weft_probe_source_bit {
    WEFT_SRC_AUXV       = 1u << 0,  /* auxv AT_HWCAP/AT_HWCAP2 read         */
    WEFT_SRC_CPUINFO    = 1u << 1,  /* /proc/cpuinfo parsed                 */
    WEFT_SRC_SYSCONF    = 1u << 2,  /* sysconf(_SC_NPROCESSORS_ONLN)        */
    WEFT_SRC_CACHELINE  = 1u << 3,  /* sysfs/sysctl cache line size         */
    WEFT_SRC_CPUFREQ    = 1u << 4,  /* sysfs cpufreq min/max read           */
    WEFT_SRC_MEMINFO    = 1u << 5,  /* /proc/meminfo parsed                 */
    WEFT_SRC_CGROUP_MEM = 1u << 6,  /* cgroup memory ceiling parsed         */
    WEFT_SRC_NUMA       = 1u << 7,  /* sysfs node topology parsed           */
    WEFT_SRC_DRM        = 1u << 8,  /* DRM render node / PCI probed         */
    WEFT_SRC_SYSCTL     = 1u << 9,  /* Darwin sysctlbyname answered         */
    WEFT_SRC_SYSPROP    = 1u << 10, /* Android system property answered     */
    WEFT_SRC_THERMAL    = 1u << 11, /* power-supply / battery evidence      */
    WEFT_SRC_FASTRPC    = 1u << 12, /* /dev/fastrpc-* device nodes          */
    WEFT_SRC_NEUROPILOT = 1u << 13, /* Neuropilot library evidence          */
    WEFT_SRC_KERNEL_REL = 1u << 14, /* kernel release parsed                */
    WEFT_SRC_TOPOLOGY   = 1u << 15  /* SMT / P-E topology evidence           */
};

/* ------------------------------------------------------------------ */
/* weft_hw_profile_t — the universal hardware descriptor               */
/*                                                                     */
/* Exactly 256 bytes (two full 128B cache lines), 128-byte aligned,     */
/* strictly little-endian, byte-frozen at ABI v1. Every offset below    */
/* is static-asserted in this header and re-verified at runtime by the */
/* test ladder (L-series) and the golden ABI fixtures (P-series).      */
/*                                                                     */
/*   0x000  u32   magic                = WEFT_SPECTRUM_MAGIC           */
/*   0x004  u32   abi_version          = 1                             */
/*   0x008  u64   schema_hash          = FNV-1a64(layout string)       */
/*   0x010  u64   caps[5]              capability words:               */
/*                                    [0] CPU vector/matrix ISA        */
/*                                    [1] CPU system / topology        */
/*                                    [2] GPU acceleration tier        */
/*                                    [3] NPU / DSP acceleration       */
/*                                    [4] system constraints           */
/*   0x038  u32   cache_line_size      32 | 64 | 128 (bytes)           */
/*   0x03c  u32   simd_max_bits        0|128|256|512 vector reg width  */
/*   0x040  u16   cores_total          online logical cores            */
/*   0x042  u16   cores_performance    performance-cluster cores        */
/*   0x044  u16   cores_efficiency     efficiency-cluster cores         */
/*   0x046  u16   numa_node_count      1..16                           */
/*   0x048  u16   npu_channels         verified accelerator channels   */
/*   0x04a  u16   gpu_engines         verified render/compute nodes    */
/*   0x04c  u16   reserved0            = 0                             */
/*   0x04e  u16   probe_sources_ok     WEFT_SRC_* bitmask              */
/*   0x050  u64   ram_total_bytes      verified physical RAM            */
/*   0x058  u64   ram_available_bytes  min(meminfo, cgroup ceiling)    */
/*   0x060  u64   gpu_memory_bytes     verified discrete VRAM (else 0) */
/*   0x068  u32   thermal_limit_mw     0=unknown; 8000=battery class   */
/*   0x06c  u32   boost_headroom_mhz   verified max-min frequency      */
/*   0x070  u32   display_max_hz       0=unknown (driver promotes)     */
/*   0x074  u32   probe_cost_ns        measured full-probe duration    */
/*   0x078  u32[16] numa_cpu_mask      per-node low-32 core mask       */
/*   0x0b8  u64[8] reserved1           = 0 (ABI v2 growth space)       */
/*   0x0f8  u32   crc32c               CRC-32C over [0x000, 0x0f8)     */
/*   0x0fc  u32   tail_magic           = WEFT_SPECTRUM_TAIL_MAGIC      */
/* ------------------------------------------------------------------ */

#define WEFT_SPECTRUM_MAX_NUMA_NODES 16

typedef struct {
    uint32_t magic;
    uint32_t abi_version;
    uint64_t schema_hash;
    uint64_t caps[WEFT_SPECTRUM_CAP_WORDS];
    uint32_t cache_line_size;
    uint32_t simd_max_bits;
    uint16_t cores_total;
    uint16_t cores_performance;
    uint16_t cores_efficiency;
    uint16_t numa_node_count;
    uint16_t npu_channels;
    uint16_t gpu_engines;
    uint16_t reserved0;
    uint16_t probe_sources_ok;
    uint64_t ram_total_bytes;
    uint64_t ram_available_bytes;
    uint64_t gpu_memory_bytes;
    uint32_t thermal_limit_mw;
    uint32_t boost_headroom_mhz;
    uint32_t display_max_hz;
    uint32_t probe_cost_ns;
    uint32_t numa_cpu_mask[WEFT_SPECTRUM_MAX_NUMA_NODES];
    uint64_t reserved1[8];
    uint32_t crc32c;
    uint32_t tail_magic;
} __attribute__((aligned(128))) weft_hw_profile_t;

/* ------------------------------------------------------------------ */
/* weft_tier_plan_t — the published governor plan                      */
/*                                                                     */
/* Exactly 128 bytes (one full 64B-aligned pair of cache lines),       */
/* byte-frozen at ABI v1. Published only through the governor's        */
/* atomic double-buffer; readers obtain consistent copies via          */
/* weft_governor_snapshot().                                           */
/*                                                                     */
/*   0x000  u32  magic            = WEFT_SPECTRUM_PLAN_MAGIC           */
/*   0x004  u32  abi_version      = 1                                  */
/*   0x008  u64  schema_hash      = FNV-1a64(plan layout string)       */
/*   0x010  u16  tier             1 | 2 | 3                            */
/*   0x012  u16  generation       monotonically increasing             */
/*   0x014  u16  policy_flags     WEFT_PLAN_POLICY_*                   */
/*   0x016  u16  ring_lanes       16 | 8 | 4 concurrent seqlock lanes  */
/*   0x018  u16  ring_slots       slots per lane                       */
/*   0x01a  u16  hdr_stride       128 | 64 (header alignment target)   */
/*   0x01c  u32  slot_stride      1024 | 512 | 256 bytes                */
/*   0x020  u32  frame_deadline_us  floor(1e6 / refresh_hz)            */
/*   0x024  u32  jitter_guard_us   deadline / 8 (render safety margin) */
/*   0x028  u32  refresh_hz        240 | 120 | 60                      */
/*   0x02c  u32  simd_path         WEFT_SIMD_* dispatch path           */
/*   0x030  u32  ingest_workers    bounded by cores_total              */
/*   0x034  u32  batch_max_msgs    ingest batching ceiling             */
/*   0x038  u64  arena_budget_bytes lanes*(hdr+slots*slot_stride)      */
/*   0x040  u64  lane_bytes        hdr_stride + slots*slot_stride      */
/*   0x048  u32  pressure_level    WEFT_PRESSURE_* at publish time     */
/*   0x04c  u32  reserved0         = 0                                 */
/*   0x050  u64[5] reserved1       = 0 (ABI v2 growth space)           */
/*   0x078  u32  reserved2         = 0                                 */
/*   0x07c  u32  crc32c            CRC-32C over [0x000, 0x07c)         */
/* ------------------------------------------------------------------ */

enum weft_tier {
    WEFT_SPECTRUM_TIER_INVALID = 0,
    WEFT_SPECTRUM_TIER_1       = 1, /* flagship / workstation, 240 Hz     */
    WEFT_SPECTRUM_TIER_2       = 2, /* mid-range, 120 Hz                  */
    WEFT_SPECTRUM_TIER_3       = 3  /* budget / low-RAM, 60 Hz            */
};

enum weft_pressure_level {
    WEFT_PRESSURE_NOMINAL  = 0, /* hardware tier governs                  */
    WEFT_PRESSURE_ELEVATED = 1, /* cap at Tier 2                          */
    WEFT_PRESSURE_CRITICAL = 2  /* drop to Tier 3 (drop-not-queue)        */
};
typedef enum weft_pressure_level weft_pressure_level_t;

enum weft_simd_path {
    WEFT_SIMD_SCALAR = 0,
    WEFT_SIMD_NEON   = 1,
    WEFT_SIMD_AVX2   = 2,
    WEFT_SIMD_AVX512 = 3,
    WEFT_SIMD_SVE2   = 4,
    WEFT_SIMD_RVV    = 5,
    WEFT_SIMD_AMX    = 6
};

enum weft_plan_policy {
    WEFT_PLAN_POLICY_SEQLOCK_LANES = 1u << 0, /* always set (engine core)  */
    WEFT_PLAN_POLICY_DROP_NOT_QUEUE = 1u << 1, /* Tier 3                   */
    WEFT_PLAN_POLICY_ASYNC_DMA      = 1u << 2, /* T1 + zero-copy accel     */
    WEFT_PLAN_POLICY_HETERO_CORES   = 1u << 3  /* verified P/E topology    */
};

typedef struct {
    uint32_t magic;
    uint32_t abi_version;
    uint64_t schema_hash;
    uint16_t tier;
    uint16_t generation;
    uint16_t policy_flags;
    uint16_t ring_lanes;
    uint16_t ring_slots;
    uint16_t hdr_stride;
    uint32_t slot_stride;
    uint32_t frame_deadline_us;
    uint32_t jitter_guard_us;
    uint32_t refresh_hz;
    uint32_t simd_path;
    uint32_t ingest_workers;
    uint32_t batch_max_msgs;
    uint64_t arena_budget_bytes;
    uint64_t lane_bytes;
    uint32_t pressure_level;
    uint32_t reserved0;
    uint64_t reserved1[5];
    uint32_t reserved2;
    uint32_t crc32c;
} __attribute__((aligned(64))) weft_tier_plan_t;

/* ------------------------------------------------------------------ */
/* weft_governor_t — dynamic budget governor                           */
/*                                                                     */
/* 384 bytes, 64-byte aligned, caller-owned (stack or static arena —   */
/* zero heap). Layout:                                                 */
/*   0x000  u64  version        atomic; bumps on every plan publish    */
/*   0x008  u64  rsvd0                                              */
/*   0x010  u32  active_tier    atomic; fast-path tier (<= 5 ns)      */
/*   0x014  u32  plan_index     atomic; 0 | 1 (published slot)        */
/*   0x018  u32  generation    atomic; total publishes               */
/*   0x01c  u32  pressure      atomic; current pressure level         */
/*   0x020  u32  forced_tier   atomic; 0 = none, else 1|2|3           */
/*   0x024  u32  flags         reserved                               */
/*   0x028  u64[5] rsvd1                                            */
/*   0x050  const weft_hw_profile_t *profile  borrowed, immutable     */
/*   0x058  u64[5] rsvd2                                            */
/*   0x080  u64  plan_words[2][16]  the two 128B plan slots, all      */
/*          accesses atomic (relaxed) — TSan-clean by construction    */
/*                                                                     */
/* Publication protocol (single writer):                               */
/*   1. build the new plan into a stack-local weft_tier_plan_t        */
/*   2. store its 16 words into the INACTIVE slot (relaxed atomics)   */
/*   3. release fence; publish plan_index (release store)             */
/*   4. store active_tier, pressure, generation                        */
/*   5. bump version (release store) — publication is complete        */
/* Reader protocol (unlimited, lock-free):                             */
/*   v1 := version(acquire); idx := plan_index(acquire);              */
/*   copy 16 words (relaxed); v2 := version(acquire);                 */
/*   accept iff v1 == v2 — the copied slot was stable throughout.     */
/* ABA across two rapid publishes is caught by the version sandwich;  */
/* the fast-path tier read is a single relaxed atomic load.           */
/* ------------------------------------------------------------------ */

#define WEFT_SPECTRUM_PLAN_WORDS 16          /* 128 bytes / 8            */
#define WEFT_SPECTRUM_SNAPSHOT_RETRIES 64    /* Law 4: bounded retries   */

typedef struct {
    uint64_t version;
    uint64_t rsvd0;
    uint32_t active_tier;
    uint32_t plan_index;
    uint32_t generation;
    uint32_t pressure;
    uint32_t forced_tier;
    uint32_t flags;
    uint64_t rsvd1[5];
    const weft_hw_profile_t *profile;
    uint64_t rsvd2[5];
    uint64_t plan_words[2][WEFT_SPECTRUM_PLAN_WORDS];
} __attribute__((aligned(64))) weft_governor_t;

/* ------------------------------------------------------------------ */
/* Byte-frozen layout proofs (compile-time; re-verified at runtime by  */
/* the L-series test ladder against the identical offset table).       */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(weft_hw_profile_t) == 256,
               "WSP5 profile must be exactly 256 bytes (two 128B lines)");
_Static_assert(_Alignof(weft_hw_profile_t) == 128,
               "WSP5 profile must be 128-byte cache-line aligned");
_Static_assert(offsetof(weft_hw_profile_t, magic) == 0x000, "frozen 0x000");
_Static_assert(offsetof(weft_hw_profile_t, abi_version) == 0x004, "frozen 0x004");
_Static_assert(offsetof(weft_hw_profile_t, schema_hash) == 0x008, "frozen 0x008");
_Static_assert(offsetof(weft_hw_profile_t, caps) == 0x010, "frozen 0x010");
_Static_assert(offsetof(weft_hw_profile_t, cache_line_size) == 0x038, "frozen 0x038");
_Static_assert(offsetof(weft_hw_profile_t, simd_max_bits) == 0x03c, "frozen 0x03c");
_Static_assert(offsetof(weft_hw_profile_t, cores_total) == 0x040, "frozen 0x040");
_Static_assert(offsetof(weft_hw_profile_t, cores_performance) == 0x042, "frozen 0x042");
_Static_assert(offsetof(weft_hw_profile_t, cores_efficiency) == 0x044, "frozen 0x044");
_Static_assert(offsetof(weft_hw_profile_t, numa_node_count) == 0x046, "frozen 0x046");
_Static_assert(offsetof(weft_hw_profile_t, npu_channels) == 0x048, "frozen 0x048");
_Static_assert(offsetof(weft_hw_profile_t, gpu_engines) == 0x04a, "frozen 0x04a");
_Static_assert(offsetof(weft_hw_profile_t, reserved0) == 0x04c, "frozen 0x04c");
_Static_assert(offsetof(weft_hw_profile_t, probe_sources_ok) == 0x04e, "frozen 0x04e");
_Static_assert(offsetof(weft_hw_profile_t, ram_total_bytes) == 0x050, "frozen 0x050");
_Static_assert(offsetof(weft_hw_profile_t, ram_available_bytes) == 0x058, "frozen 0x058");
_Static_assert(offsetof(weft_hw_profile_t, gpu_memory_bytes) == 0x060, "frozen 0x060");
_Static_assert(offsetof(weft_hw_profile_t, thermal_limit_mw) == 0x068, "frozen 0x068");
_Static_assert(offsetof(weft_hw_profile_t, boost_headroom_mhz) == 0x06c, "frozen 0x06c");
_Static_assert(offsetof(weft_hw_profile_t, display_max_hz) == 0x070, "frozen 0x070");
_Static_assert(offsetof(weft_hw_profile_t, probe_cost_ns) == 0x074, "frozen 0x074");
_Static_assert(offsetof(weft_hw_profile_t, numa_cpu_mask) == 0x078, "frozen 0x078");
_Static_assert(offsetof(weft_hw_profile_t, reserved1) == 0x0b8, "frozen 0x0b8");
_Static_assert(offsetof(weft_hw_profile_t, crc32c) == 0x0f8, "frozen 0x0f8");
_Static_assert(offsetof(weft_hw_profile_t, tail_magic) == 0x0fc, "frozen 0x0fc");

_Static_assert(sizeof(weft_tier_plan_t) == 128,
               "WSP5 plan must be exactly 128 bytes");
_Static_assert(_Alignof(weft_tier_plan_t) == 64,
               "WSP5 plan must be 64-byte cache-line aligned");
_Static_assert(offsetof(weft_tier_plan_t, magic) == 0x00, "frozen plan 0x00");
_Static_assert(offsetof(weft_tier_plan_t, abi_version) == 0x04, "frozen plan 0x04");
_Static_assert(offsetof(weft_tier_plan_t, schema_hash) == 0x08, "frozen plan 0x08");
_Static_assert(offsetof(weft_tier_plan_t, tier) == 0x10, "frozen plan 0x10");
_Static_assert(offsetof(weft_tier_plan_t, generation) == 0x12, "frozen plan 0x12");
_Static_assert(offsetof(weft_tier_plan_t, policy_flags) == 0x14, "frozen plan 0x14");
_Static_assert(offsetof(weft_tier_plan_t, ring_lanes) == 0x16, "frozen plan 0x16");
_Static_assert(offsetof(weft_tier_plan_t, ring_slots) == 0x18, "frozen plan 0x18");
_Static_assert(offsetof(weft_tier_plan_t, hdr_stride) == 0x1a, "frozen plan 0x1a");
_Static_assert(offsetof(weft_tier_plan_t, slot_stride) == 0x1c, "frozen plan 0x1c");
_Static_assert(offsetof(weft_tier_plan_t, frame_deadline_us) == 0x20, "frozen plan 0x20");
_Static_assert(offsetof(weft_tier_plan_t, jitter_guard_us) == 0x24, "frozen plan 0x24");
_Static_assert(offsetof(weft_tier_plan_t, refresh_hz) == 0x28, "frozen plan 0x28");
_Static_assert(offsetof(weft_tier_plan_t, simd_path) == 0x2c, "frozen plan 0x2c");
_Static_assert(offsetof(weft_tier_plan_t, ingest_workers) == 0x30, "frozen plan 0x30");
_Static_assert(offsetof(weft_tier_plan_t, batch_max_msgs) == 0x34, "frozen plan 0x34");
_Static_assert(offsetof(weft_tier_plan_t, arena_budget_bytes) == 0x38, "frozen plan 0x38");
_Static_assert(offsetof(weft_tier_plan_t, lane_bytes) == 0x40, "frozen plan 0x40");
_Static_assert(offsetof(weft_tier_plan_t, pressure_level) == 0x48, "frozen plan 0x48");
_Static_assert(offsetof(weft_tier_plan_t, crc32c) == 0x7c, "frozen plan 0x7c");

_Static_assert(sizeof(weft_governor_t) == 384,
               "WSP5 governor must be exactly 384 bytes");
_Static_assert(_Alignof(weft_governor_t) == 64,
               "WSP5 governor must be 64-byte aligned");
_Static_assert(offsetof(weft_governor_t, version) == 0x00, "frozen gov 0x00");
_Static_assert(offsetof(weft_governor_t, active_tier) == 0x10, "frozen gov 0x10");
_Static_assert(offsetof(weft_governor_t, plan_index) == 0x14, "frozen gov 0x14");
_Static_assert(offsetof(weft_governor_t, generation) == 0x18, "frozen gov 0x18");
_Static_assert(offsetof(weft_governor_t, pressure) == 0x1c, "frozen gov 0x1c");
_Static_assert(offsetof(weft_governor_t, forced_tier) == 0x20, "frozen gov 0x20");
_Static_assert(offsetof(weft_governor_t, profile) == 0x50, "frozen gov 0x50");
_Static_assert(offsetof(weft_governor_t, plan_words) == 0x80, "frozen gov 0x80");
_Static_assert((WEFT_FEATURE_COUNT) <= 320,
               "five capability words hold at most 320 feature ids");

/* ------------------------------------------------------------------ */
/* weft_governor_stats_t — binding-facing telemetry snapshot           */
/*                                                                     */
/* 64 bytes. Individual fields are atomic loads; cross-field           */
/* consistency is NOT implied (indicative telemetry — see D-51 §10).   */
/*   0x00 u32 magic  0x04 u32 abi  0x08 u64 version  0x10 u32 tier     */
/*   0x14 u32 pressure  0x18 u32 generation  0x1c u32 forced_tier      */
/*   0x20 u32 score  0x24 u32 transitions  0x28 u32 refresh_hz         */
/*   0x2c u32 frame_deadline_us  0x30 u64 arena_budget_bytes           */
/*   0x38 u32 profile_crc32c  0x3c u32 reserved                        */
/* ------------------------------------------------------------------ */

typedef struct weft_governor_stats {
    uint32_t magic;
    uint32_t abi_version;
    uint64_t version;
    uint32_t tier;
    uint32_t pressure;
    uint32_t generation;
    uint32_t forced_tier;
    uint32_t score;
    uint32_t transitions;
    uint32_t refresh_hz;
    uint32_t frame_deadline_us;
    uint64_t arena_budget_bytes;
    uint32_t profile_crc32c;
    uint32_t reserved;
} weft_governor_stats_t;

_Static_assert(sizeof(weft_governor_stats_t) == 64,
               "WSP5 governor stats must be exactly 64 bytes");

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Probe the host into caller-owned storage. Zero allocation, one-shot
 * (call at startup). Fail-closed per source: missing sources degrade
 * the profile conservatively and clear their WEFT_SRC_* bits; the call
 * itself only fails on contract violations (null out). */
weft_spectrum_status_t weft_hw_probe(weft_hw_profile_t *out_profile);

/* One-time warmup of lazy libc caches (sysconf / getauxval / stdio).
 * Optional but recommended before zero-alloc measurement windows. */
void weft_spectrum_warmup(void);

/* Full integrity ladder: magic, abi version, schema hash, tail magic,
 * CRC-32C. Refuses with the matching named code — never silent. */
weft_spectrum_status_t weft_hw_profile_validate(const weft_hw_profile_t *profile);

/* Driver-layer promotion (Engineer 2): set a loader-verifiable feature
 * bit on an already-sealed profile, re-sealing the CRC. MUST complete
 * before the profile is shared with reader threads. Returns false and
 * touches nothing when feature_id is out of range. */
bool weft_hw_profile_promote_feature(weft_hw_profile_t *profile,
                                     uint32_t feature_id);

/* Recompute crc32c + tail magic identity (builder helper; the probe and
 * the promotion path both seal through here). */
void weft_hw_profile_seal(weft_hw_profile_t *profile);

/* Governor lifecycle. init validates the profile (fail-closed: a
 * corrupted profile refuses with ECHECKSUM/EABI) and publishes the
 * Tier-1/2/3 plan for the supplied pressure level (default NOMINAL
 * when in doubt — callers pass WEFT_PRESSURE_NOMINAL explicitly). */
weft_spectrum_status_t weft_governor_init(weft_governor_t *gov,
                                          const weft_hw_profile_t *profile);

/* Single-writer pressure transition: recompute + republish the plan.
 * Never blocks readers; no sweeps; arena budgets change as integers
 * consumed by arena owners at their own cadence. */
weft_spectrum_status_t weft_governor_retarget(weft_governor_t *gov,
                                              weft_pressure_level_t pressure);

/* Operator override: force a tier (1|2|3) until cleared. Applied at the
 * next retarget; weft_get_active_tier reflects it immediately after. */
weft_spectrum_status_t weft_governor_force_tier(weft_governor_t *gov,
                                                uint32_t tier);
weft_spectrum_status_t weft_governor_clear_force(weft_governor_t *gov);

/* Lock-free consistent plan copy (version sandwich, bounded retries).
 * Refusal code EPLAN_BUSY — a torn plan is NEVER returned as data. */
weft_spectrum_status_t weft_governor_snapshot(const weft_governor_t *gov,
                                              weft_tier_plan_t *out_plan);

/* Indicative telemetry snapshot for managed bindings (Engineer 3). */
weft_spectrum_status_t weft_governor_stats(const weft_governor_t *gov,
                                           weft_governor_stats_t *out_stats);

/* Override the display cadence (24..480 Hz); recomputes deadlines and
 * republishes. Display-side evidence comes from Engineer 2's driver. */
weft_spectrum_status_t weft_governor_set_refresh(weft_governor_t *gov,
                                                 uint32_t hz);

/* Auditable pure functions: exact integer tier score and resulting tier
 * for a validated profile (the full scoring table is normative in
 * D-51 §5 — the tests pin every archetype's exact score). */
uint32_t weft_governor_score(const weft_hw_profile_t *profile);
uint32_t weft_governor_tier_for_profile(const weft_hw_profile_t *profile);

/* ------------------------------------------------------------------ */
/* Inline queries — the <= 5 ns hot paths                              */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#  define WEFT_SPECTRUM_INLINE static inline __attribute__((always_inline))
#else
#  define WEFT_SPECTRUM_INLINE static inline
#endif

/* Direct bitmask bit-test: two shifts + one AND. Bounds-checked so an
 * out-of-range id can never read foreign memory (fail-closed false). */
WEFT_SPECTRUM_INLINE bool weft_hw_has_feature(const weft_hw_profile_t *p,
                                              uint32_t feature_id)
{
    if (p == NULL || feature_id >= (uint32_t)WEFT_FEATURE_COUNT) {
        return false;
    }
    return ((p->caps[feature_id >> 6] >> (feature_id & 63u)) & 1u) != 0u;
}

/* Raw capability word (0..4); out-of-range yields 0. */
WEFT_SPECTRUM_INLINE uint64_t weft_hw_caps_word(const weft_hw_profile_t *p,
                                                uint32_t word)
{
    if (p == NULL || word >= (uint32_t)WEFT_SPECTRUM_CAP_WORDS) {
        return 0u;
    }
    return p->caps[word];
}

/* Single relaxed atomic load of the published tier — the fastest
 * governance query in the engine (sub-nanosecond on every target). */
WEFT_SPECTRUM_INLINE uint32_t weft_get_active_tier(const weft_governor_t *g)
{
    if (g == NULL) {
        return (uint32_t)WEFT_SPECTRUM_TIER_INVALID;
    }
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&g->active_tier, __ATOMIC_RELAXED);
#else
    return *(const volatile uint32_t *)&g->active_tier;
#endif
}

/* Monotonic publish counter (diagnostics / binding heartbeat). */
WEFT_SPECTRUM_INLINE uint64_t weft_governor_plan_version(const weft_governor_t *g)
{
    if (g == NULL) {
        return 0u;
    }
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&g->version, __ATOMIC_ACQUIRE);
#else
    return *(const volatile uint64_t *)&g->version;
#endif
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEFT_SPECTRUM_H */
