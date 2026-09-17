// turbo.c — RFC 0012 Tail-Latency Eradication Layer (see turbo.h for the
// contract). Nothing in this file writes a stamp, allocates on a hot path,
// or modifies a frozen byte: every service composes the seams the frozen
// layers expose (attach_writer, advisory ctrl reads, writer-private reads).

#define _GNU_SOURCE
#include "turbo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#if WEFT_TURBO_LINUX
#include <sys/syscall.h>
#endif

// MADV_HUGEPAGE — glibc guarded it behind __USE_MISC for years; the raw
// kernel value keeps every -D_GNU_SOURCE build (and older glibcs) working.
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

// ---------------------------------------------------------------------------
// Capability probe (cold; pthread_once-guarded, idempotent)
// ---------------------------------------------------------------------------

#if WEFT_TURBO_LINUX

#include <pthread.h>

static weft_turbo_caps_t g_caps;
static cpu_set_t g_node_cpus[WEFT_TURBO_MAX_NODES];
static pthread_once_t g_caps_once = PTHREAD_ONCE_INIT;

static long read_hugepage_size(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Hugepagesize:", 13) == 0) {
            kb = strtol(line + 13, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb > 0 ? kb * 1024 : 0;
}

static weft_turbo_thp_mode_t read_thp_mode(char* raw, size_t rawlen) {
    FILE* f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (!f) return WEFT_TURBO_THP_UNKNOWN;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = 0;
        size_t n = strlen(buf);
        if (n >= rawlen) n = rawlen - 1;
        memcpy(raw, buf, n);
        raw[n] = 0;
        if (strstr(buf, "[always]"))  { fclose(f); return WEFT_TURBO_THP_ALWAYS; }
        if (strstr(buf, "[madvise]")) { fclose(f); return WEFT_TURBO_THP_MADVISE; }
        if (strstr(buf, "[never]"))   { fclose(f); return WEFT_TURBO_THP_NEVER; }
    }
    fclose(f);
    return WEFT_TURBO_THP_UNKNOWN;
}

// Parse "0-1" / "0,2-3" / "0-3,8-11" node cpulists into a cpu_set_t.
static int parse_cpulist(const char* path, cpu_set_t* set) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    CPU_ZERO(set);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned a, b;
        if (sscanf(tok, "%u-%u", &a, &b) == 2) {
            for (unsigned c = a; c <= b && c < CPU_SETSIZE; c++) CPU_SET(c, set);
        } else if (sscanf(tok, "%u", &a) == 1) {
            if (a < CPU_SETSIZE) CPU_SET(a, set);
        }
    }
    return 0;
}

static void caps_probe_once(void) {
    weft_turbo_caps_t c;
    memset(&c, 0, sizeof(c));
    c.page_size = sysconf(_SC_PAGESIZE);
    if (c.page_size <= 0) c.page_size = 4096;
    c.hugepage_size = read_hugepage_size();
    c.ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (c.ncpu <= 0) c.ncpu = 1;
    c.thp_mode = read_thp_mode(c.thp_mode_str, sizeof(c.thp_mode_str));

    // NUMA topology: /sys/devices/system/node/node%d/cpulist
    c.node_count = 0;
    for (int n = 0; n < WEFT_TURBO_MAX_NODES; n++) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", n);
        cpu_set_t set;
        if (parse_cpulist(path, &set) == 0) {
            g_node_cpus[n] = set;
            c.node_cpu_count[n] = CPU_COUNT(&set);
            c.node_count = n + 1;
        } else {
            break;
        }
    }
    if (c.node_count == 0) {
        // No /sys topology (container stripped?) — node 0 = all CPUs we have.
        CPU_ZERO(&g_node_cpus[0]);
        for (int i = 0; i < c.ncpu && i < CPU_SETSIZE; i++) CPU_SET(i, &g_node_cpus[0]);
        c.node_cpu_count[0] = c.ncpu > CPU_SETSIZE ? CPU_SETSIZE : c.ncpu;
        c.node_count = 1;
    }

    // Affinity probe: get works if set works on this kernel/cgroup.
    cpu_set_t cur;
    c.affinity_ok = (sched_getaffinity(0, sizeof(cur), &cur) == 0);

    // mlock probe: lock+unlock one page, record the refusal errno verbatim.
    static char probe_page[4096] __attribute__((aligned(4096)));
    c.mlock_errno = 0;
    if (mlock(probe_page, sizeof(probe_page)) == 0) {
        munlock(probe_page, sizeof(probe_page));
        c.mlock_ok = 1;
    } else {
        c.mlock_ok = 0;
        c.mlock_errno = errno ? errno : 1;
    }

    // SCHED_FIFO probe: try prio 1, ALWAYS restore the old policy.
    c.sched_fifo_errno = 0;
    struct sched_param dummy;
    memset(&dummy, 0, sizeof(dummy));
    int old_sched = sched_getscheduler(0);
    if (old_sched >= 0) {
        struct sched_param old_param;
        memset(&old_param, 0, sizeof(old_param));
        sched_getparam(0, &old_param);
        dummy.sched_priority = 1;
        if (sched_setscheduler(0, SCHED_FIFO, &dummy) == 0) {
            sched_setscheduler(0, old_sched, &old_param);  // restore
            c.sched_fifo_ok = 1;
        } else {
            c.sched_fifo_ok = 0;
            c.sched_fifo_errno = errno ? errno : 1;
        }
    } else {
        c.sched_fifo_ok = 0;
        c.sched_fifo_errno = errno ? errno : 1;
    }

    c.probed = 1;
    g_caps = c;
}

const weft_turbo_caps_t* weft_turbo_caps(void) {
    pthread_once(&g_caps_once, caps_probe_once);
    return &g_caps;
}

size_t weft_turbo_caps_report(char* buf, size_t buflen) {
    const weft_turbo_caps_t* c = weft_turbo_caps();
    int n = snprintf(buf, buflen,
        "turbo-capability-report\n"
        "  page_size=%ld hugepage_size=%ld ncpu=%d\n"
        "  thp_mode=%s (%d)\n"
        "  numa_nodes=%d node0_cpus=%d%s\n"
        "  affinity=%s\n"
        "  mlock=%s (errno %d)\n"
        "  sched_fifo=%s (errno %d)\n",
        c->page_size, c->hugepage_size, c->ncpu,
        c->thp_mode_str[0] ? c->thp_mode_str : "unknown", (int)c->thp_mode,
        c->node_count, c->node_count > 0 ? c->node_cpu_count[0] : 0,
        c->node_count > 1 ? " [multi-node: first-touch placement live]" : " [single-node: ladder verified, cross-node deferred]",
        c->affinity_ok ? "available" : "refused",
        c->mlock_ok ? "available" : "REFUSED", c->mlock_errno,
        c->sched_fifo_ok ? "available" : "REFUSED", c->sched_fifo_errno);
    return n > 0 ? (size_t)n : 0;
}

#else  // !WEFT_TURBO_LINUX — portable stubs, every service refuses loudly

static weft_turbo_caps_t g_caps;

const weft_turbo_caps_t* weft_turbo_caps(void) {
    if (!g_caps.probed) {
        memset(&g_caps, 0, sizeof(g_caps));
        g_caps.page_size = 4096;
        g_caps.node_count = 1;
        g_caps.thp_mode = WEFT_TURBO_THP_UNKNOWN;
        snprintf(g_caps.thp_mode_str, sizeof(g_caps.thp_mode_str), "non-linux");
        g_caps.probed = 1;
    }
    return &g_caps;
}

size_t weft_turbo_caps_report(char* buf, size_t buflen) {
    return snprintf(buf, buflen,
        "turbo-capability-report\n  platform=non-linux (all services refuse)\n");
}

#endif // WEFT_TURBO_LINUX

// ---------------------------------------------------------------------------
// Tail-latency ring allocation
// ---------------------------------------------------------------------------

void weft_turbo_ring_opts_default(weft_turbo_ring_opts_t* o) {
    memset(o, 0, sizeof(*o));
    o->slot_count = 8;
    o->payload_bytes = 4096;
    o->want_thp = 1;
    o->want_prefault = 1;
    o->want_mlock = 1;
    o->numa_node = -1;
}

#if WEFT_TURBO_LINUX

int weft_turbo_fanout_create(weft_fanout_t* f, weft_turbo_ring_t* tr,
                             const weft_turbo_ring_opts_t* opts) {
    if (!f || !tr) return -1;
    memset(tr, 0, sizeof(*tr));
    weft_turbo_ring_opts_t o;
    if (opts) o = *opts; else weft_turbo_ring_opts_default(&o);

    const size_t ring_bytes = weft_fanout_ring_bytes(o.payload_bytes, o.slot_count);
    if (ring_bytes == 0) return -1;
    if (o.numa_node >= WEFT_TURBO_MAX_NODES) return -1;

    const weft_turbo_caps_t* caps = weft_turbo_caps();
    const size_t align = (o.want_thp && caps->hugepage_size >= caps->page_size)
                       ? (size_t)caps->hugepage_size : (size_t)caps->page_size;

    // NUMA bind scope BEFORE the mapping exists — first-touch then lands
    // the pages on the bound node (the memset below is the touch).
    weft_turbo_affinity_t saved;
    int bound = 0;
    if (o.numa_node >= 0) {
        if (weft_turbo_numa_bind(o.numa_node, &saved) == 0) bound = 1;
    }

    // Over-allocate by one alignment unit, then trim both edges to the
    // hugepage boundary — the classic guaranteed-aligned-mapping recipe.
    const size_t map_len = ring_bytes + align;
    void* p = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        if (bound) weft_turbo_restore_affinity(&saved);
        return -1;
    }
    uintptr_t base = (uintptr_t)p;
    uintptr_t ring = (base + align - 1) & ~(uintptr_t)(align - 1);
    if (ring != base) {
        munmap((void*)base, ring - base);
    }
    const size_t tail = (size_t)((base + map_len) - (ring + ring_bytes));
    if (tail > 0) {
        munmap((void*)(ring + ring_bytes), tail);
    }

    // THP policy BEFORE any touch, so the faults below can be huge faults
    // on madvise-mode hosts. Best-effort: the errno is the honesty record.
    int thp_rc = 0;
    if (o.want_thp) {
        thp_rc = madvise((void*)ring, ring_bytes, MADV_HUGEPAGE);
        if (thp_rc != 0) thp_rc = errno;
    }

    // First-touch + prefault + the attach contract's zeroed ctrl, in one
    // memset. want_prefault=0 relies on anon-mmap zero pages (lazy) — the
    // ctrl words still read as 0 for attach_writer; the tail-latency cost
    // of laziness is then the CALLER's stated choice.
    if (o.want_prefault) {
        memset((void*)ring, 0, ring_bytes);
    }

    if (bound) weft_turbo_restore_affinity(&saved);

    // mlock AFTER touching (locking faulted pages). The 64 KiB
    // RLIMIT_MEMLOCK container cap refuses multi-MiB rings — recorded.
    int lock_rc = 0;
    if (o.want_mlock) {
        if (mlock((void*)ring, ring_bytes) != 0) lock_rc = errno;
    }

    // The FROZEN attach path validates geometry and adopts the mapping.
    memset(f, 0, sizeof(*f));
    if (weft_fanout_attach_writer(f, (void*)ring, ring_bytes,
                                  o.payload_bytes, o.slot_count) != 0) {
        munmap((void*)ring, ring_bytes);
        memset(tr, 0, sizeof(*tr));
        return -1;
    }

    tr->ring = (void*)ring;
    tr->ring_bytes = ring_bytes;
    tr->map_base = (void*)ring;
    tr->map_len = ring_bytes;
    tr->thp_advised = thp_rc;
    tr->prefaulted = o.want_prefault ? 1 : 0;
    tr->locked = lock_rc;
    tr->numa_node = bound ? o.numa_node : -1;
    tr->numa_bound = bound;
    return 0;
}

void weft_turbo_fanout_destroy(weft_fanout_t* f, weft_turbo_ring_t* tr) {
    if (!tr) return;
    if (tr->map_base && tr->map_len) {
        munmap(tr->map_base, tr->map_len);
    }
    if (f) weft_fanout_destroy(f);  // owns_ring == 0: frees nothing
    memset(tr, 0, sizeof(*tr));
}

#else  // non-Linux: refuse ring creation (no mmap contract to ride)

int weft_turbo_fanout_create(weft_fanout_t* f, weft_turbo_ring_t* tr,
                             const weft_turbo_ring_opts_t* opts) {
    (void)f; (void)tr; (void)opts;
    return -1;
}

void weft_turbo_fanout_destroy(weft_fanout_t* f, weft_turbo_ring_t* tr) {
    if (f) weft_fanout_destroy(f);
    if (tr) memset(tr, 0, sizeof(*tr));
}

#endif // WEFT_TURBO_LINUX

// ---------------------------------------------------------------------------
// Non-temporal streaming fill
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>  // SSE2: _mm_stream_si128 + _mm_sfence (baseline ISA —
                        // no -mavx flag, no runtime dispatch, no target attr)
#define WEFT_TURBO_NT 1
#else
#define WEFT_TURBO_NT 0
#endif

int weft_turbo_fill(weft_fanout_t* f, const void* src, size_t len) {
    if (!f->w_cursor) return -1;
    if (len > f->payload_bytes || (len % 4) != 0) return -1;

#if WEFT_TURBO_NT
    uint8_t* dst = f->w_cursor;
    const uint8_t* s = (const uint8_t*)src;
    size_t off = 0;

    // Prologue: relaxed-atomic word stores until 16-byte aligned (matches
    // weft_fanout_fill's race-free discipline for the ragged edge).
    while (off < len && (((uintptr_t)(dst + off)) & 15u) != 0) {
        uint32_t w;
        memcpy(&w, s + off, 4);
        atomic_store_explicit((_Atomic uint32_t*)(void*)(dst + off), w,
                              memory_order_relaxed);
        off += 4;
    }

    // Body: 128-bit non-temporal stores — no RFO, no cache pollution; the
    // reader's copy pulls the frame once from memory, exactly its access
    // pattern, while the writer's cache stays reserved for the hot ctrl.
    // body_end is measured from the ALIGNED `off` (a ragged prologue of 8
    // bytes with body_end = len & ~15 would overrun the slot by exactly the
    // prologue length — T5's M=3 geometry exists to pin this).
    const size_t body_end = off + ((len - off) & ~(size_t)15);
    for (; off < body_end; off += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(s + off));
        _mm_stream_si128((__m128i*)(dst + off), v);
    }

    // Epilogue: ragged tail via the relaxed-atomic word path.
    while (off < len) {
        uint32_t w;
        memcpy(&w, s + off, 4);
        atomic_store_explicit((_Atomic uint32_t*)(void*)(dst + off), w,
                              memory_order_relaxed);
        off += 4;
    }

    // THE ordering obligation (turbo.h contract): WC stores must retire
    // before the publish() release stamp — SFENCE here, so the frozen
    // stamp can never outrun a payload word.
    _mm_sfence();
    return (int)(len / 4);
#else
    // Non-x86: byte-identical fallback (the frozen race-free fill).
    return weft_fanout_fill(f, src, len);
#endif
}

// ---------------------------------------------------------------------------
// Thread services
// ---------------------------------------------------------------------------

#if WEFT_TURBO_LINUX

int weft_turbo_save_affinity(weft_turbo_affinity_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (sched_getaffinity(0, sizeof(cpu_set_t), (cpu_set_t*)out->raw) != 0) {
        return -(errno ? errno : 1);
    }
    out->valid = 1;
    return 0;
}

int weft_turbo_restore_affinity(const weft_turbo_affinity_t* saved) {
    if (!saved || !saved->valid) return -1;
    if (sched_setaffinity(0, sizeof(cpu_set_t), (const cpu_set_t*)saved->raw) != 0) {
        return -(errno ? errno : 1);
    }
    return 0;
}

int weft_turbo_pin_cpu(int cpu) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) return -EINVAL;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return -(errno ? errno : 1);
    }
    return 0;
}

int weft_turbo_rt(int prio) {
    struct sched_param p;
    memset(&p, 0, sizeof(p));
    p.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &p) != 0) {
        return -(errno ? errno : 1);
    }
    return 0;
}

int weft_turbo_mlockall(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        return -(errno ? errno : 1);
    }
    return 0;
}

int weft_turbo_numa_bind(int node, weft_turbo_affinity_t* saved) {
    (void)weft_turbo_caps();  // ensure g_node_cpus is populated
    if (node < 0 || node >= WEFT_TURBO_MAX_NODES || g_caps.node_count <= 0) {
        return -EINVAL;
    }
    if (node >= g_caps.node_count || g_caps.node_cpu_count[node] == 0) {
        return -EINVAL;  // node does not exist on this host — refuse, don't guess
    }
    if (saved && weft_turbo_save_affinity(saved) != 0) {
        return -1;
    }
    if (sched_setaffinity(0, sizeof(cpu_set_t), &g_node_cpus[node]) != 0) {
        return -(errno ? errno : 1);
    }
    return 0;
}

#else  // non-Linux stubs

int weft_turbo_save_affinity(weft_turbo_affinity_t* out) { (void)out; return -ENOSYS; }
int weft_turbo_restore_affinity(const weft_turbo_affinity_t* s) { (void)s; return -ENOSYS; }
int weft_turbo_pin_cpu(int cpu) { (void)cpu; return -ENOSYS; }
int weft_turbo_rt(int prio) { (void)prio; return -ENOSYS; }
int weft_turbo_mlockall(void) { return -ENOSYS; }
int weft_turbo_numa_bind(int node, weft_turbo_affinity_t* s) { (void)node; (void)s; return -ENOSYS; }

#endif // WEFT_TURBO_LINUX
