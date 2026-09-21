// backend_registry.c — the weft-spectrum driver execution table (Pillar 5).
//
// WHY EXISTS: this is the module's governor-facing core. It probes and
// admits backends, builds the immutable priority table, owns the module
// heap discipline (Law 1), the throttled honest log ring, and the dispatch
// walk that guarantees deterministic graceful degradation (Law 3) — the
// < 1 us fallback from a refused engine to the SIMD vector engine.
//
// Table order is a TOTAL deterministic order: engine class ascending
// (NPU -> GPU -> DSP -> CPU_VECTOR), score descending, vendor_id ascending,
// registration order last. After the table is built it is IMMUTABLE for
// the context's lifetime — dispatch reads it lock-free, TSan-clean.
//
// THREADING CONTRACT (documented, not enforced): a context is
// single-writer — the governor creates one per thread (or serializes).
// Creation brackets the module heap lock (allocation is legal only inside
// init phases); dispatch never allocates.

#include "weft_backend.h"
#include "weft_dma.h"
#include "weft_driver_core.h"
#include "../simd/weft_simd.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ===========================================================================
// §1 Module heap discipline (Law 1)
// ===========================================================================

static _Atomic uint64_t g_heap_bytes;
static _Atomic int      g_heap_locked;

uint64_t weft_backend_heap_bytes(void) {
    return atomic_load_explicit(&g_heap_bytes, memory_order_relaxed);
}
int weft_backend_heap_locked(void) {
    return atomic_load_explicit(&g_heap_locked, memory_order_relaxed);
}
void weft_backend_heap_lock(void) {
    atomic_store_explicit(&g_heap_locked, 1, memory_order_release);
}
void weft_backend_heap_unlock(void) {
    atomic_store_explicit(&g_heap_locked, 0, memory_order_release);
}

/// The ONLY allocator in this module. Init-phase legal; post-lock FATAL.
/// (Fail-closed in the loudest way: a Law-1 violation is a programming
/// error, and silent allocation would be the dishonest alternative.)
static void* wb_alloc(uint64_t bytes, uint64_t align) {
    if (atomic_load_explicit(&g_heap_locked, memory_order_acquire)) {
        fprintf(stderr,
                "weft-spectrum: LAW 1 VIOLATION — %" PRIu64
                "-byte heap allocation attempted on the locked hot path. "
                "Aborting (fail-closed).\n",
                bytes);
        fflush(stderr);
        abort();
    }
    if (align < 64u) {
        align = 64u;
    }
    void* p = NULL;
    if (posix_memalign(&p, (size_t)align, (size_t)bytes) != 0) {
        return NULL;
    }
    atomic_fetch_add_explicit(&g_heap_bytes, bytes, memory_order_relaxed);
    return p;
}

static void wb_free(void* p, uint64_t bytes) {
    if (p != NULL) {
        atomic_fetch_sub_explicit(&g_heap_bytes, bytes, memory_order_relaxed);
        free(p);
    }
}

uint64_t weft_backend_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ===========================================================================
// §2 The honest log ring (fixed 32B records, throttled, never blocking)
// ===========================================================================

#define WB_LOG_RING 128u

typedef struct {
    uint64_t t_ns;
    uint64_t seq;
    uint32_t code;
    uint32_t aux;
    uint16_t backend_idx;
    uint8_t  level;
    uint8_t  pad0;
} wb_log_rec_t;   // 32B

_Static_assert(sizeof(wb_log_rec_t) == 32, "log record must be 32B");

static uint8_t wb_log_level_of(uint32_t code) {
    switch (code) {
    case WEFT_LOG_INIT_FAILED:
    case WEFT_LOG_DEVICE_GONE:
    case WEFT_LOG_TORN_READ:
    case WEFT_LOG_HEAP_VIOLATION:
        return WEFT_CTX_LOG_ERROR;
    case WEFT_LOG_PROBE_REFUSED:
    case WEFT_LOG_FALLBACK_HOP:
    case WEFT_LOG_BUS_SATURATED:
        return WEFT_CTX_LOG_INFO;
    default:
        return WEFT_CTX_LOG_DEBUG;
    }
}

// ===========================================================================
// §3 The context (public via opaque handle; one aligned allocation)
// ===========================================================================

#define WB_CTX_MAGIC 0x53504543u   // 'SPEC'

typedef enum {
    WB_ENTRY_LIVE = 1,
    WB_ENTRY_DEAD_PROBE = 0,        // probe refused (honest absence)
    WB_ENTRY_DEAD_INIT = -1,        // probe OK but init failed
    WB_ENTRY_DEAD_RUNTIME = -2      // hot-unplug / EDEVICE at dispatch
} wb_entry_state_t;

typedef struct {
    const weft_backend_ops_t* ops;
    void*                     self;
    weft_backend_caps_t       caps;
    uint32_t                  state;         ///< wb_entry_state_t
    int32_t                   death_reason;  ///< status that killed it
    uint32_t                  reg_order;
} wb_entry_t;

struct weft_backend_ctx_s {
    uint32_t magic;
    uint32_t log_level;
    uint32_t flags;
    uint32_t engine_mask;

    uint32_t n_entries;      ///< total slots probed (live + dead, honest)
    uint32_t n_live;
    uint32_t last_submit_idx;
    uint32_t reserved0;

    wb_entry_t entries[WEFT_BACKEND_MAX_BACKENDS];

    // Honest ledger
    _Atomic uint64_t dispatches;
    _Atomic uint64_t fallback_hops;
    _Atomic uint64_t refusals;
    _Atomic uint64_t busy_propagated;
    _Atomic uint64_t device_gone;
    _Atomic uint64_t bytes_submitted;
    _Atomic uint64_t torn_detected;
    _Atomic uint64_t log_records_dropped;

    // Log ring (single-writer: the owning thread)
    wb_log_rec_t log_ring[WB_LOG_RING];
    uint64_t     log_head;         ///< total records ever attempted
    _Atomic uint32_t supp[16];     ///< per-code suppression counters

    // Scratch arena (bump; init-time only)
    unsigned char* arena;
    uint64_t       arena_bytes;
    uint64_t       arena_off;
};

static weft_backend_status_t wb_ctx_arena_alloc(void* owner, uint64_t bytes,
                                                uint64_t align, void** out) {
    weft_backend_ctx_t* ctx = (weft_backend_ctx_t*)owner;
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC || out == NULL || bytes == 0) {
        return WEFT_BACKEND_EINVAL;
    }
    if (align < 64u) {
        align = 64u;
    }
    const uint64_t off = (ctx->arena_off + align - 1u) & ~(align - 1u);
    if (off + bytes > ctx->arena_bytes) {
        return WEFT_BACKEND_ENOMEM;   // init-time exhaustion: fail-closed
    }
    ctx->arena_off = off + bytes;
    *out = ctx->arena + off;
    return WEFT_BACKEND_OK;
}

static void wb_ctx_log(void* owner, uint32_t code, uint32_t backend_idx,
                       uint32_t aux) {
    weft_backend_ctx_t* ctx = (weft_backend_ctx_t*)owner;
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC) {
        return;
    }
    const uint8_t level = wb_log_level_of(code);
    if (level > ctx->log_level) {
        return;   // gated: zero work below the configured level
    }
    // Throttle: first occurrence + every 65536th afterwards. Honest and
    // NEVER blocking: a suppressed record costs one relaxed atomic add.
    const uint32_t n = atomic_fetch_add_explicit(&ctx->supp[code & 15u], 1u,
                                                 memory_order_relaxed);
    if (n != 0u && (n & 0xFFFFu) != 0xFFFFu) {
        return;
    }
    const uint64_t seq = ++ctx->log_head;
    wb_log_rec_t* rec = &ctx->log_ring[seq % WB_LOG_RING];
    if (seq > WB_LOG_RING) {
        // This write displaces record seq-128, still unread: count it —
        // dropped records are reported, never hidden.
        atomic_fetch_add_explicit(&ctx->log_records_dropped, 1u,
                                   memory_order_relaxed);
    }
    rec->t_ns        = weft_backend_now_ns();
    rec->seq         = seq;
    rec->code        = code;
    rec->aux         = aux;
    rec->backend_idx = (uint16_t)backend_idx;
    rec->level       = level;
    rec->pad0        = 0;
}

void weft_backend_flush_log(weft_backend_ctx_t* ctx) {
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC) {
        return;
    }
    const uint64_t head = ctx->log_head;
    const uint64_t start = (head > WB_LOG_RING) ? head - WB_LOG_RING : 0;
    for (uint64_t s = start; s < head; s++) {
        const wb_log_rec_t* rec = &ctx->log_ring[(s + 1u) % WB_LOG_RING];
        if (rec->seq != s + 1u) {
            continue;   // displaced/never-written slot
        }
        fprintf(stderr,
                "weft-spectrum[backend=%u] code=%u aux=%u level=%u t=%" PRIu64
                "ns\n",
                (unsigned)rec->backend_idx, rec->code, rec->aux,
                (unsigned)rec->level, rec->t_ns);
    }
    fflush(stderr);
}

// ===========================================================================
// §4 The terminal CPU engine (guaranteed; wraps the SIMD core)
// ===========================================================================

typedef struct {
    uint32_t seq;
    uint32_t pad[15];
} wb_cpu_state_t;   // 64B state block

static weft_backend_status_t wb_cpu_probe(weft_backend_caps_t* caps) {
    if (caps == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    memset(caps, 0, sizeof(*caps));
    caps->abi_version = WEFT_BACKEND_ABI_VERSION;
    caps->vendor_id   = WEFT_VENDOR_CPU_SIMD;
    caps->engine_class = WEFT_ENGINE_CPU_VECTOR;
    caps->flags       = WEFT_CAPS_ZERO_COPY;
    caps->op_affinity = WEFT_OP_AFFINITY(WEFT_OP_NORMALIZE_F32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_ENCODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DELTA_DECODE_U32) |
                        WEFT_OP_AFFINITY(WEFT_OP_DOT_F32) |
                        WEFT_OP_AFFINITY(WEFT_OP_SEQLOCK_CHECKSUM);
    caps->device_memory_bytes = 0;
    caps->score = 1;   // terminal: lowest priority, always admitted
    // Honest identity: names the SIMD implementation actually resolved.
    const char* impl = weft_simd_active_impl_name();
    size_t i = 0;
    for (; i + 1 < sizeof(caps->impl_name) && impl[i] != '\0'; i++) {
        caps->impl_name[i] = impl[i];
    }
    if (i + 1 < sizeof(caps->impl_name)) {
        caps->impl_name[i] = '\0';
    } else {
        caps->impl_name[sizeof(caps->impl_name) - 1] = '\0';
    }
    return WEFT_BACKEND_OK;
}

static weft_backend_status_t wb_cpu_init(void* self,
                                         const weft_backend_init_cfg_t* cfg) {
    (void)cfg;
    wb_cpu_state_t* st = (wb_cpu_state_t*)self;
    if (st == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    st->seq = 0;
    return WEFT_BACKEND_OK;
}

static weft_backend_status_t wb_cpu_execute(void* self, const weft_op_desc_t* op,
                                            weft_dispatch_result_t* out) {
    wb_cpu_state_t* st = (wb_cpu_state_t*)self;
    if (st == NULL) {
        return WEFT_BACKEND_ESTATE;
    }
    return weft_driver_host_execute(op, out, &st->seq);
}

static weft_backend_status_t wb_cpu_submit(void* self, const weft_op_desc_t* ops,
                                           uint32_t count) {
    wb_cpu_state_t* st = (wb_cpu_state_t*)self;
    if (st == NULL || ops == NULL || count == 0) {
        return WEFT_BACKEND_EINVAL;
    }
    const uint32_t kind = ops[0].kind;
    for (uint32_t i = 0; i < count; i++) {
        if (ops[i].kind != kind) {
            return WEFT_BACKEND_EINVAL;
        }
        weft_backend_status_t st_code = weft_driver_host_execute(&ops[i], NULL, &st->seq);
        if (st_code != WEFT_BACKEND_OK) {
            return st_code;
        }
    }
    return WEFT_BACKEND_OK;
}

static weft_backend_status_t wb_cpu_sync(void* self, uint64_t completion_seq,
                                         uint64_t timeout_ns) {
    (void)self;
    (void)completion_seq;
    (void)timeout_ns;
    return WEFT_BACKEND_OK;   // host engine: submit completed inline
}

static void wb_cpu_shutdown(void* self) { (void)self; }

const weft_backend_ops_t weft_cpu_simd_ops = {
    .name         = "weft-cpu-simd",
    .abi_version  = WEFT_BACKEND_ABI_VERSION,
    .vendor_id    = WEFT_VENDOR_CPU_SIMD,
    .engine_class = WEFT_ENGINE_CPU_VECTOR,
    .state_bytes  = sizeof(wb_cpu_state_t),
    .reserved0    = 0,
    .probe        = wb_cpu_probe,
    .init         = wb_cpu_init,
    .execute      = wb_cpu_execute,
    .submit       = wb_cpu_submit,
    .sync         = wb_cpu_sync,
    .shutdown     = wb_cpu_shutdown,
};

// ===========================================================================
// §5 Registry: builtins + runtime slots (Engineer 3's SDK fallbacks)
// ===========================================================================

extern const weft_backend_ops_t weft_driver_qualcomm_ops;
extern const weft_backend_ops_t weft_driver_mediatek_ops;
extern const weft_backend_ops_t weft_driver_apple_ops;
extern const weft_backend_ops_t weft_driver_nvidia_pc_ops;
extern const weft_backend_ops_t weft_driver_riscv_arm_ops;

static const weft_backend_ops_t* const g_builtin_ops[] = {
    &weft_driver_qualcomm_ops,
    &weft_driver_mediatek_ops,
    &weft_driver_apple_ops,
    &weft_driver_nvidia_pc_ops,
    &weft_driver_riscv_arm_ops,
    &weft_cpu_simd_ops,
};
#define WB_BUILTIN_COUNT \
    ((uint32_t)(sizeof(g_builtin_ops) / sizeof(g_builtin_ops[0])))

#define WB_RUNTIME_MAX 24u
typedef const weft_backend_ops_t* wb_ops_ptr_t;
static _Atomic wb_ops_ptr_t g_runtime_ops[WB_RUNTIME_MAX];
static _Atomic uint32_t g_runtime_count;
static _Atomic uint32_t g_live_ctxs;   // heap-lock lives while any ctx lives

const weft_backend_ops_t* const* weft_backend_registry_ops(uint32_t* count_out) {
    if (count_out != NULL) {
        *count_out = WB_BUILTIN_COUNT;
    }
    return g_builtin_ops;
}

weft_backend_status_t weft_backend_register(const weft_backend_ops_t* ops) {
    if (ops == NULL || ops->probe == NULL || ops->name == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    if (ops->abi_version != WEFT_BACKEND_ABI_VERSION) {
        return WEFT_BACKEND_EABI;
    }
    if (atomic_load_explicit(&g_live_ctxs, memory_order_acquire) > 0u) {
        return WEFT_BACKEND_ESTATE;   // tables immutable once built (frozen hdr)
    }
    if (atomic_load_explicit(&g_runtime_count, memory_order_acquire) >=
        WB_RUNTIME_MAX) {
        return WEFT_BACKEND_EBUSY;
    }
    for (uint32_t i = 0; i < WB_RUNTIME_MAX; i++) {
        if (atomic_load_explicit(&g_runtime_ops[i], memory_order_acquire) == ops) {
            return WEFT_BACKEND_ESTATE;   // duplicate registration: fail-closed
        }
    }
    for (uint32_t i = 0; i < WB_RUNTIME_MAX; i++) {
        wb_ops_ptr_t expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&g_runtime_ops[i], &expected,
                                                    ops, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            atomic_fetch_add_explicit(&g_runtime_count, 1, memory_order_release);
            return WEFT_BACKEND_OK;
        }
    }
    return WEFT_BACKEND_EBUSY;
}

weft_backend_status_t weft_backend_unregister(const weft_backend_ops_t* ops) {
    if (ops == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    for (uint32_t i = 0; i < WB_BUILTIN_COUNT; i++) {
        if (g_builtin_ops[i] == ops) {
            return WEFT_BACKEND_ESTATE;   // builtins are permanent
        }
    }
    for (uint32_t i = 0; i < WB_RUNTIME_MAX; i++) {
        wb_ops_ptr_t expected = ops;
        if (atomic_compare_exchange_strong_explicit(&g_runtime_ops[i], &expected,
                                                    NULL, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            atomic_fetch_sub_explicit(&g_runtime_count, 1, memory_order_release);
            return WEFT_BACKEND_OK;
        }
    }
    return WEFT_BACKEND_EINVAL;
}

// ===========================================================================
// §6 Table build: probe -> admit -> deterministic total order -> init
// ===========================================================================

static int wb_entry_before(const wb_entry_t* a, const wb_entry_t* b) {
    if (a->caps.engine_class != b->caps.engine_class) {
        return a->caps.engine_class < b->caps.engine_class;
    }
    if (a->caps.score != b->caps.score) {
        return a->caps.score > b->caps.score;
    }
    if (a->caps.vendor_id != b->caps.vendor_id) {
        return a->caps.vendor_id < b->caps.vendor_id;
    }
    return a->reg_order < b->reg_order;
}

weft_backend_ctx_t* weft_backend_ctx_create(const weft_backend_cfg_t* cfg_in) {
    weft_backend_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (cfg_in != NULL) {
        cfg = *cfg_in;
    }
    if (cfg.engine_mask == 0) {
        cfg.engine_mask = WEFT_ENGINE_MASK_ALL;
    }
    if (cfg.log_level == 0) {
        cfg.log_level = WEFT_CTX_LOG_INFO;
    }
    if (cfg.cmd_ring_slots == 0) {
        cfg.cmd_ring_slots = WEFT_BACKEND_DEFAULT_CMD_RING;
    }
    if (cfg.dma_map_slots == 0) {
        cfg.dma_map_slots = WEFT_BACKEND_DEFAULT_DMA_MAPS;
    }
    if (cfg.arena_bytes == 0) {
        cfg.arena_bytes = WEFT_BACKEND_ARENA_DEFAULT;
    }

    // Init-phase bracket around the module heap lock (single-writer ctx
    // contract): allocation is legal exactly here.
    const int was_locked = weft_backend_heap_locked();
    if (was_locked) {
        weft_backend_heap_unlock();
    }

    const uint64_t total = sizeof(weft_backend_ctx_t) + cfg.arena_bytes;
    weft_backend_ctx_t* ctx = (weft_backend_ctx_t*)wb_alloc(total, 64);
    if (ctx == NULL) {
        if (was_locked) {
            weft_backend_heap_lock();
        }
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic       = WB_CTX_MAGIC;
    ctx->log_level   = cfg.log_level;
    ctx->flags       = cfg.flags;
    ctx->engine_mask = cfg.engine_mask;
    ctx->arena       = (unsigned char*)ctx + sizeof(weft_backend_ctx_t);
    ctx->arena_bytes = cfg.arena_bytes;
    ctx->arena_off   = 0;
    ctx->last_submit_idx = 0xFFFFFFFFu;

    // ---- Phase 1: probe every candidate -------------------------------
    // Candidates: builtins first (stable reg_order), then runtime slots.
    uint32_t reg = 0;
    for (uint32_t i = 0; i < WB_BUILTIN_COUNT; i++) {
        const weft_backend_ops_t* ops = g_builtin_ops[i];
        wb_entry_t* e = &ctx->entries[ctx->n_entries];
        memset(e, 0, sizeof(*e));
        e->ops = ops;
        e->reg_order = reg++;
        e->state = WB_ENTRY_DEAD_PROBE;
        e->death_reason = WEFT_BACKEND_EREFUSED;
        weft_backend_caps_t caps;
        memset(&caps, 0, sizeof(caps));
        if (ops->probe(&caps) == WEFT_BACKEND_OK &&
            caps.abi_version == WEFT_BACKEND_ABI_VERSION) {
            const int mock_ok =
                (caps.vendor_id != WEFT_VENDOR_MOCK) ||
                ((cfg.flags & WEFT_CTX_FLAG_ALLOW_MOCK) != 0u);
            const int class_ok =
                (caps.engine_class < 32u) &&
                ((cfg.engine_mask & (1u << caps.engine_class)) != 0u);
            if (mock_ok && class_ok) {
                e->caps = caps;
                e->state = WB_ENTRY_LIVE;
                e->death_reason = 0;
            } else {
                e->caps = caps;
                e->death_reason = WEFT_BACKEND_EREFUSED;
            }
        } else {
            // Probe refused / ABI mismatch: the driver's caps still carry
            // its HONEST identity (drivers fill impl_name before refusing)
            // — keep it so dead entries stay attributable and sort in their
            // true engine class instead of collapsing to a zeroed ghost.
            e->caps = caps;
        }
        ctx->n_entries++;
    }
    for (uint32_t i = 0; i < WB_RUNTIME_MAX && ctx->n_entries < WEFT_BACKEND_MAX_BACKENDS; i++) {
        const weft_backend_ops_t* ops = atomic_load_explicit(&g_runtime_ops[i], memory_order_acquire);
        if (ops == NULL) {
            continue;
        }
        wb_entry_t* e = &ctx->entries[ctx->n_entries];
        memset(e, 0, sizeof(*e));
        e->ops = ops;
        e->reg_order = reg++;
        e->state = WB_ENTRY_DEAD_PROBE;
        e->death_reason = WEFT_BACKEND_EREFUSED;
        weft_backend_caps_t caps;
        memset(&caps, 0, sizeof(caps));
        if (ops->probe(&caps) == WEFT_BACKEND_OK &&
            caps.abi_version == WEFT_BACKEND_ABI_VERSION) {
            const int mock_ok =
                (caps.vendor_id != WEFT_VENDOR_MOCK) ||
                ((cfg.flags & WEFT_CTX_FLAG_ALLOW_MOCK) != 0u);
            const int class_ok =
                (caps.engine_class < 32u) &&
                ((cfg.engine_mask & (1u << caps.engine_class)) != 0u);
            if (mock_ok && class_ok) {
                e->caps = caps;
                e->state = WB_ENTRY_LIVE;
                e->death_reason = 0;
            } else {
                e->caps = caps;
                e->death_reason = WEFT_BACKEND_EREFUSED;
            }
        } else {
            e->caps = caps;
        }
        ctx->n_entries++;
    }
    // ---- Phase 2: deterministic total order (stable insertion sort) ----
    for (uint32_t i = 1; i < ctx->n_entries; i++) {
        wb_entry_t key = ctx->entries[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && wb_entry_before(&key, &ctx->entries[j])) {
            ctx->entries[j + 1] = ctx->entries[j];
            j--;
        }
        ctx->entries[j + 1] = key;
    }

    // ---- Phase 3: init admitted entries (final indices; arena state) --
    for (uint32_t i = 0; i < ctx->n_entries; i++) {
        wb_entry_t* e = &ctx->entries[i];
        if (e->state != WB_ENTRY_LIVE) {
            wb_ctx_log(ctx, WEFT_LOG_PROBE_REFUSED, 0xFFFFu, e->caps.vendor_id);
            continue;
        }
        void* self = NULL;
        if (wb_ctx_arena_alloc(ctx, e->ops->state_bytes, 64, &self) !=
            WEFT_BACKEND_OK) {
            e->state = WB_ENTRY_DEAD_INIT;
            e->death_reason = WEFT_BACKEND_ENOMEM;
            wb_ctx_log(ctx, WEFT_LOG_INIT_FAILED, i, WEFT_BACKEND_ENOMEM);
            continue;
        }
        memset(self, 0, (size_t)e->ops->state_bytes);
        weft_backend_init_cfg_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        icfg.owner = ctx;
        // The transport seam: the vendor's override, else the driver
        // constructs its own real transport inside init (and refuses
        // honestly on headless hosts when it cannot).
        icfg.transport = NULL;
        if (e->caps.vendor_id >= 1u && e->caps.vendor_id <= WEFT_VENDOR_COUNT) {
            icfg.transport = cfg.transport_overrides[e->caps.vendor_id - 1u];
        }
        icfg.arena_alloc    = wb_ctx_arena_alloc;
        icfg.log            = wb_ctx_log;
        icfg.flags          = cfg.flags;
        icfg.engine_mask    = cfg.engine_mask;
        icfg.cmd_ring_slots = cfg.cmd_ring_slots;
        icfg.dma_map_slots  = cfg.dma_map_slots;
        e->self = self;
        weft_backend_status_t st = e->ops->init(self, &icfg);
        if (st != WEFT_BACKEND_OK) {
            e->state = WB_ENTRY_DEAD_INIT;
            e->death_reason = st;
            e->self = NULL;
            wb_ctx_log(ctx, WEFT_LOG_INIT_FAILED, i, (uint32_t)st);
        } else {
            ctx->n_live++;
        }
    }

    // ---- Phase 4: Law 1 engages ---------------------------------------
    atomic_fetch_add_explicit(&g_live_ctxs, 1u, memory_order_acq_rel);
    weft_backend_heap_lock();
    return ctx;
}

void weft_backend_ctx_destroy(weft_backend_ctx_t* ctx) {
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC) {
        return;
    }
    for (uint32_t i = 0; i < ctx->n_entries; i++) {
        wb_entry_t* e = &ctx->entries[i];
        if (e->state == WB_ENTRY_LIVE && e->ops->shutdown != NULL) {
            e->ops->shutdown(e->self);
        }
    }
    weft_backend_flush_log(ctx);
    const uint64_t total = sizeof(weft_backend_ctx_t) + ctx->arena_bytes;
    ctx->magic = 0;
    // The heap lock tracks live contexts: the LAST destroy opens it again.
    // (wb_free never checks the lock — only allocation on the hot path is
    // the Law-1 abort; destruction is an init-phase operation.)
    if (atomic_fetch_sub_explicit(&g_live_ctxs, 1u, memory_order_acq_rel) == 1u) {
        weft_backend_heap_unlock();
    }
    wb_free(ctx, total);
}

// ===========================================================================
// §7 Dispatch — the hot walk (zero heap; refusal hops; transient propagates)
// ====================================================================================

static int wb_is_refusal(weft_backend_status_t st) {
    return st == WEFT_BACKEND_ENOTSUP || st == WEFT_BACKEND_EDEVICE ||
           st == WEFT_BACKEND_EREFUSED;
}

weft_backend_status_t weft_backend_dispatch(weft_backend_ctx_t* ctx,
                                             const weft_op_desc_t* op,
                                             weft_dispatch_result_t* out) {
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC || op == NULL || out == NULL) {
        return WEFT_BACKEND_EINVAL;
    }
    if (op->kind < WEFT_OP_NORMALIZE_F32 || op->kind > WEFT_OP_SEQLOCK_CHECKSUM) {
        return WEFT_BACKEND_EINVAL;   // fail-closed: unknown kinds never walk
    }

    const uint64_t affinity = 1ull << op->kind;
    uint32_t hops = 0;
    weft_backend_status_t last_refusal = WEFT_BACKEND_OK;

    for (uint32_t i = 0; i < ctx->n_entries; i++) {
        wb_entry_t* e = &ctx->entries[i];
        if (e->state != WB_ENTRY_LIVE) {
            continue;   // honest dead entries stay visible but unreachable
        }
        if ((e->caps.op_affinity & affinity) == 0u) {
            continue;   // not targetable for this op kind
        }
        weft_backend_status_t st = e->ops->execute(e->self, op, out);
        if (st == WEFT_BACKEND_OK) {
            atomic_fetch_add_explicit(&ctx->dispatches, 1u,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(
                &ctx->bytes_submitted,
                op->bufs[0].bytes + op->bufs[1].bytes + op->bufs[2].bytes,
                memory_order_relaxed);
            out->backend_index = i;
            out->engine_class  = e->caps.engine_class;
            out->fallback_hops = hops;
            out->status        = WEFT_BACKEND_OK;
            return WEFT_BACKEND_OK;
        }
        if (st == WEFT_BACKEND_EDEVICE) {
            e->state = WB_ENTRY_DEAD_INIT;   // hot-unplug: deterministic death
            e->death_reason = WEFT_BACKEND_EDEVICE;
            atomic_fetch_add_explicit(&ctx->device_gone, 1u,
                                      memory_order_relaxed);
            wb_ctx_log(ctx, WEFT_LOG_DEVICE_GONE, i, (uint32_t)st);
            hops++;
            last_refusal = st;
            continue;
        }
        if (wb_is_refusal(st)) {
            atomic_fetch_add_explicit(&ctx->refusals, 1u,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&ctx->fallback_hops, 1u,
                                      memory_order_relaxed);
            wb_ctx_log(ctx, WEFT_LOG_FALLBACK_HOP, i, (uint32_t)st);
            hops++;
            last_refusal = st;
            continue;
        }
        if (st == WEFT_BACKEND_EBUSY) {
            atomic_fetch_add_explicit(&ctx->busy_propagated, 1u,
                                      memory_order_relaxed);
            wb_ctx_log(ctx, WEFT_LOG_BUS_SATURATED, i, (uint32_t)st);
            return st;   // transient: caller owns retry policy (Law 4)
        }
        // EINVAL/ERANGE/ECHECKSUM/ESTATE/ETIMEOUT: honest propagate.
        return st;
    }

    // Every entry refused (or the mask excluded the terminal engine — a
    // governor choice). Fail closed with the last honest refusal.
    return (last_refusal != WEFT_BACKEND_OK) ? last_refusal
                                             : WEFT_BACKEND_ESTATE;
}

weft_backend_status_t weft_backend_submit(weft_backend_ctx_t* ctx,
                                          const weft_op_desc_t* ops,
                                          uint32_t count) {
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC || ops == NULL || count == 0) {
        return WEFT_BACKEND_EINVAL;
    }
    const uint32_t kind = ops[0].kind;
    if (kind < WEFT_OP_NORMALIZE_F32 || kind > WEFT_OP_SEQLOCK_CHECKSUM) {
        return WEFT_BACKEND_EINVAL;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (ops[i].kind != kind) {
            return WEFT_BACKEND_EINVAL;   // homogeneous batches only
        }
    }
    const uint64_t affinity = 1ull << kind;
    for (uint32_t i = 0; i < ctx->n_entries; i++) {
        wb_entry_t* e = &ctx->entries[i];
        if (e->state != WB_ENTRY_LIVE || (e->caps.op_affinity & affinity) == 0u) {
            continue;
        }
        ctx->last_submit_idx = i;
        return e->ops->submit(e->self, ops, count);
    }
    return WEFT_BACKEND_ESTATE;
}

weft_backend_status_t weft_backend_sync(weft_backend_ctx_t* ctx,
                                        uint64_t completion_seq,
                                        uint64_t timeout_ns) {
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC) {
        return WEFT_BACKEND_EINVAL;
    }
    if (ctx->last_submit_idx == 0xFFFFFFFFu ||
        ctx->last_submit_idx >= ctx->n_entries) {
        return WEFT_BACKEND_ESTATE;
    }
    wb_entry_t* e = &ctx->entries[ctx->last_submit_idx];
    if (e->state != WB_ENTRY_LIVE || e->ops->sync == NULL) {
        return WEFT_BACKEND_ESTATE;
    }
    return e->ops->sync(e->self, completion_seq, timeout_ns);
}

// ===========================================================================
// §8 Introspection + stats
// ===========================================================================

uint32_t weft_backend_table_info(const weft_backend_ctx_t* ctx,
                                 weft_backend_info_t* out, uint32_t cap) {
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC || out == NULL) {
        return 0;
    }
    const uint32_t n = (ctx->n_entries < cap) ? ctx->n_entries : cap;
    for (uint32_t i = 0; i < n; i++) {
        const wb_entry_t* e = &ctx->entries[i];
        weft_backend_info_t* d = &out[i];
        memset(d, 0, sizeof(*d));
        d->vendor_id  = e->caps.vendor_id;
        d->engine_class = e->caps.engine_class;
        d->score      = e->caps.score;
        d->state      = (e->state == WB_ENTRY_LIVE)
                            ? 1
                            : ((int32_t)e->state == WB_ENTRY_DEAD_INIT ? -1 : 0);
        d->flags      = e->caps.flags;
        d->op_affinity = e->caps.op_affinity;
        d->device_memory_bytes = e->caps.device_memory_bytes;
        memcpy(d->impl_name, e->caps.impl_name, sizeof(d->impl_name));
        d->death_reason = e->death_reason;
    }
    return ctx->n_entries;
}

void weft_backend_stats(const weft_backend_ctx_t* ctx,
                        weft_backend_stats_t* out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (ctx == NULL || ctx->magic != WB_CTX_MAGIC) {
        return;
    }
    out->dispatches   = atomic_load_explicit(&ctx->dispatches, memory_order_relaxed);
    out->fallback_hops = atomic_load_explicit(&ctx->fallback_hops, memory_order_relaxed);
    out->refusals     = atomic_load_explicit(&ctx->refusals, memory_order_relaxed);
    out->busy_propagated = atomic_load_explicit(&ctx->busy_propagated, memory_order_relaxed);
    out->device_gone  = atomic_load_explicit(&ctx->device_gone, memory_order_relaxed);
    out->bytes_submitted = atomic_load_explicit(&ctx->bytes_submitted, memory_order_relaxed);
    out->torn_detected = atomic_load_explicit(&ctx->torn_detected, memory_order_relaxed);
    out->log_records_dropped =
        atomic_load_explicit(&ctx->log_records_dropped, memory_order_relaxed);
}
