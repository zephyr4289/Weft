// weft_dmabuf.c — Linux DMA-BUF direct binding (RFC-0016 §3, driver layer).
//
// Implementation notes:
//   * The DMA-BUF heap uAPI (<linux/dma-heap.h>, stable since kernel 5.6)
//     and the sync uAPI (<linux/dma-buf.h>) are re-declared locally —
//     the same discipline uring_rx applies to io_uring structs: no libc
//     version dependency, byte-identical to the kernel's own definitions.
//   * The WFSH header writer mirrors shm_ring.c's dialect field-for-field
//     (the DB4 gate proves the two writers agree on every deterministic
//     byte; creator_pid/created_unix_ns are diagnostics and differ by
//     design — AXIOM T).
//   * Non-Linux builds compile the refusal stubs (the uring_rx pattern):
//     probe reports UNSUPPORTED, every entry point refuses with
//     EOPNOTSUPP, nothing crashes, nothing pretends.

#include "weft_dmabuf.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)

#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// uAPI subsets (kernel-stable; local re-declaration, no libc dependency)
// ---------------------------------------------------------------------------

// <linux/dma-heap.h> — allocation uAPI (kernel >= 5.6)
#define WEFT_DMA_HEAP_IOC_MAGIC 'H'
struct weft_dma_heap_allocation {
    uint64_t len;        // IN: requested bytes
    uint32_t fd;         // OUT: the dma-buf fd
    uint32_t fd_flags;   // IN: O_CLOEXEC etc.
};
#define WEFT_DMA_HEAP_IOCTL_ALLOC \
    _IOWR(WEFT_DMA_HEAP_IOC_MAGIC, 0x0, struct weft_dma_heap_allocation)

// <linux/dma-buf.h> — CPU-cache sync uAPI (foreign uncached buffers)
struct weft_dma_buf_sync { uint64_t flags; };
#define WEFT_DMA_BUF_SYNC_READ   (1ULL << 0)
#define WEFT_DMA_BUF_SYNC_WRITE  (2ULL << 0)
#define WEFT_DMA_BUF_SYNC_START  (0ULL << 2)
#define WEFT_DMA_BUF_SYNC_END    (1ULL << 2)
#define WEFT_DMA_BUF_BASE        'b'
#define WEFT_DMA_BUF_IOCTL_SYNC \
    _IOW(WEFT_DMA_BUF_BASE, 0, struct weft_dma_buf_sync)

// ---------------------------------------------------------------------------
// WFSH header dialect (byte-identical to shm_ring.c's writer/validator)
// ---------------------------------------------------------------------------

#define WEFT_DMABUF_HEADER_BYTES 64u
#define WEFT_DMABUF_MAGIC        0x48534657u  // "WFSH" little-endian
#define WEFT_DMABUF_VERSION      1u

static void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32le(uint8_t* p, uint32_t v) {
    put_u16le(p, (uint16_t)(v & 0xFFFFu));
    put_u16le(p + 2, (uint16_t)(v >> 16));
}
static void put_u64le(uint8_t* p, uint64_t v) {
    put_u32le(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32le(p + 4, (uint32_t)(v >> 32));
}
static uint16_t get_u16le(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64le(const uint8_t* p) {
    return (uint64_t)get_u32le(p) | ((uint64_t)get_u32le(p + 4) << 32);
}

static uint64_t unix_ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/// Write the session header (shm_ring dialect; see DB4).
static int session_header_write(uint8_t* base, size_t payload_bytes,
                                unsigned slot_count) {
    memset(base, 0, WEFT_DMABUF_HEADER_BYTES);
    put_u32le(base + 0, WEFT_DMABUF_MAGIC);
    put_u16le(base + 4, (uint16_t)WEFT_DMABUF_VERSION);
    put_u16le(base + 6, (uint16_t)WEFT_DMABUF_HEADER_BYTES);
    put_u32le(base + 8, 0);  // flags
    put_u32le(base + 12, (uint32_t)payload_bytes);
    put_u32le(base + 16, (uint32_t)slot_count);
    put_u64le(base + 20, (uint64_t)weft_fanout_ring_bytes(payload_bytes, slot_count));
    put_u32le(base + 28, (uint32_t)getpid());
    put_u64le(base + 32, unix_ns_now());
    return 0;
}

/// Validate a mapped header with >= size semantics (page-rounded
/// allocations exceed the exact span — the layering note in the header).
/// Returns 0/-1; never guesses.
static int session_header_validate(const uint8_t* base, size_t map_bytes,
                                   size_t* payload_bytes, unsigned* slot_count) {
    if (map_bytes < WEFT_DMABUF_HEADER_BYTES) return -1;
    if (get_u32le(base + 0) != WEFT_DMABUF_MAGIC) return -1;
    if (get_u16le(base + 4) != WEFT_DMABUF_VERSION) return -1;
    if (get_u16le(base + 6) != WEFT_DMABUF_HEADER_BYTES) return -1;
    if (get_u32le(base + 8) != 0) return -1;  // unknown flag bits = reject
    const uint32_t pb = get_u32le(base + 12);
    const uint32_t sc = get_u32le(base + 16);
    const uint64_t rb = get_u64le(base + 20);
    if (rb != weft_fanout_ring_bytes(pb, sc)) return -1;
    if (pb == 0 || (pb & 3u) || sc < 2 || sc > WEFT_FANOUT_MAX_SLOTS) return -1;
    for (size_t i = 40; i < WEFT_DMABUF_HEADER_BYTES; i++) {
        if (base[i] != 0) return -1;  // reserved must be zero (version discipline)
    }
    if ((uint64_t)map_bytes < (uint64_t)WEFT_DMABUF_HEADER_BYTES + rb) return -1;
    if (payload_bytes) *payload_bytes = pb;
    if (slot_count) *slot_count = (unsigned)sc;
    return 0;
}

// ---------------------------------------------------------------------------
// Capability probe
// ---------------------------------------------------------------------------

static weft_dmabuf_probe_t g_probe;
static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;

/// Try to open + allocation-prove one heap node. Returns an owned fd on
/// success (the probe's throwaway allocation), -1 with the heap skipped.
static int heap_try(const char* dir, const char* name) {
    char path[128];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return -1;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct weft_dma_heap_allocation alloc;
    memset(&alloc, 0, sizeof(alloc));
    alloc.len = 4096;  // one throwaway page proves the ioctl actually works
    alloc.fd = 0;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(fd, WEFT_DMA_HEAP_IOCTL_ALLOC, &alloc) != 0 ||
        alloc.fd == 0 || alloc.fd == ~(uint32_t)0) {
        close(fd);
        return -1;
    }
    close(alloc.fd);  // the proof allocation; the real one comes at ring_alloc
    return fd;
}

static void probe_run(void) {
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.page_size = sysconf(_SC_PAGESIZE);
    if (g_probe.page_size <= 0) g_probe.page_size = 4096;

    static const char* k_dirs[] = { "/dev/dma_heap", "/dev/dma_heap/system@by-uid" };
    for (size_t d = 0; d < sizeof(k_dirs) / sizeof(k_dirs[0]); d++) {
        DIR* dir = opendir(k_dirs[d]);
        if (!dir) continue;
        struct dirent* e;
        while ((e = readdir(dir)) != NULL) {
            if (e->d_name[0] == '.') continue;
            // record every heap node found (diagnostics — the honest map)
            if (g_probe.heap_list[0] != '\0')
                strncat(g_probe.heap_list, ",", sizeof(g_probe.heap_list) - strlen(g_probe.heap_list) - 1);
            strncat(g_probe.heap_list, e->d_name, sizeof(g_probe.heap_list) - strlen(g_probe.heap_list) - 1);
        }
        closedir(dir);
    }

    // Coherent system heap candidates, in preference order. The uncached
    // variants are deliberately absent — see the header's WHY SYSTEM ONLY.
    static const char* k_preferred[] = { "system", "system-dma32" };
    for (size_t i = 0; i < sizeof(k_preferred) / sizeof(k_preferred[0]); i++) {
        char path[96];
        if (snprintf(path, sizeof(path), "/dev/dma_heap/%s", k_preferred[i]) >= (int)sizeof(path))
            continue;
        int fd = heap_try("/dev/dma_heap", k_preferred[i]);
        if (fd >= 0) {
            g_probe.caps = WEFT_DMABUF_HEAP_SYSTEM;
            snprintf(g_probe.heap_path, sizeof(g_probe.heap_path), "%s", path);
            close(fd);  // path recorded; ring_alloc reopens (thread-simplicity)
            break;
        }
    }
    g_probe.probed = 1;
}

const weft_dmabuf_probe_t* weft_dmabuf_probe(void) {
    pthread_once(&g_probe_once, probe_run);
    return &g_probe;
}

size_t weft_dmabuf_report(char* buf, size_t buflen) {
    const weft_dmabuf_probe_t* p = weft_dmabuf_probe();
    const char* rung = (p->caps == WEFT_DMABUF_HEAP_SYSTEM) ? "heap-system" : "unsupported";
    int n = snprintf(buf, buflen, "dmabuf: %s heap='%s' heaps=[%s] page=%ld",
                     rung, p->heap_path, p->heap_list, p->page_size);
    return (n < 0) ? 0 : (size_t)n;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

size_t weft_dmabuf_span_bytes(size_t payload_bytes, unsigned slot_count) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) return 0;
    return (size_t)WEFT_DMABUF_HEADER_BYTES + rb;
}

// ---------------------------------------------------------------------------
// Ring allocation / fd binding
// ---------------------------------------------------------------------------

static int ring_map_and_init(weft_dmabuf_ring_t* r, int fd,
                             size_t payload_bytes, unsigned slot_count,
                             int bound_fd) {
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        errno = (st.st_size <= 0 && errno == 0) ? EINVAL : errno;
        return -1;
    }
    const size_t span = weft_dmabuf_span_bytes(payload_bytes, slot_count);
    if (span == 0 || (size_t)st.st_size < span) {
        errno = EINVAL;  // the buffer cannot cover the session
        return -1;
    }
    void* p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return -1;

    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->base = (uint8_t*)p;
    r->map_bytes = (size_t)st.st_size;
    r->span_bytes = span;
    r->payload_bytes = payload_bytes;
    r->slot_count = slot_count;
    r->bound_fd = bound_fd;
    r->ring = r->base + WEFT_DMABUF_HEADER_BYTES;

    // The session header + fresh-ring ctrl invariants. For a fresh heap
    // allocation the pages are already zero; an adopted (possibly reused)
    // fd is re-initialized explicitly — both roads end identical.
    session_header_write(r->base, payload_bytes, slot_count);
    memset(r->ring, 0, weft_fanout_ring_bytes(payload_bytes, slot_count));
    return 0;
}

int weft_dmabuf_ring_alloc(weft_dmabuf_ring_t* out, size_t payload_bytes,
                           unsigned slot_count) {
    if (out == NULL) { errno = EINVAL; return -1; }
    // geometry pre-check (mirror fanout's rules; ring_bytes returns 0 on bad)
    if (weft_dmabuf_span_bytes(payload_bytes, slot_count) == 0) {
        errno = EINVAL;
        return -1;
    }
    const weft_dmabuf_probe_t* p = weft_dmabuf_probe();
    if (p->caps != WEFT_DMABUF_HEAP_SYSTEM || p->heap_path[0] == '\0') {
        errno = ENOTSUP;  // honest refusal: no coherent system heap
        return -1;
    }
    int heap_fd = open(p->heap_path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) return -1;

    const size_t span = weft_dmabuf_span_bytes(payload_bytes, slot_count);
    const long page = p->page_size > 0 ? p->page_size : 4096;
    const size_t rounded = ((span + (size_t)page - 1) / (size_t)page) * (size_t)page;

    struct weft_dma_heap_allocation alloc;
    memset(&alloc, 0, sizeof(alloc));
    alloc.len = (uint64_t)rounded;
    alloc.fd = 0;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, WEFT_DMA_HEAP_IOCTL_ALLOC, &alloc) != 0 ||
        alloc.fd == 0 || alloc.fd == ~(uint32_t)0) {
        const int saved = errno;
        close(heap_fd);
        errno = saved;
        return -1;
    }
    close(heap_fd);

    if (ring_map_and_init(out, (int)alloc.fd, payload_bytes, slot_count, 0) != 0) {
        const int saved = errno;
        close((int)alloc.fd);
        errno = saved;
        return -1;
    }
    return 0;
}

int weft_dmabuf_ring_bind_fd(weft_dmabuf_ring_t* out, int fd,
                             size_t payload_bytes, unsigned slot_count) {
    if (out == NULL || fd < 0) { errno = EINVAL; return -1; }
    if (weft_dmabuf_span_bytes(payload_bytes, slot_count) == 0) {
        errno = EINVAL;
        return -1;
    }
    return ring_map_and_init(out, fd, payload_bytes, slot_count, 1);
}

void weft_dmabuf_ring_free(weft_dmabuf_ring_t* r) {
    if (r == NULL || r->base == NULL) return;
    munmap(r->base, r->map_bytes);
    if (r->fd >= 0 && !r->bound_fd) close(r->fd);  // heap-allocated: owned
    memset(r, 0, sizeof(*r));
    r->fd = -1;  // the documented "unbound" state
}

// ---------------------------------------------------------------------------
// Foreign import
// ---------------------------------------------------------------------------

int weft_dmabuf_import_fd(weft_dmabuf_import_t* out, int fd, size_t map_bytes) {
    if (out == NULL || fd < 0) { errno = EINVAL; return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        errno = (st.st_size <= 0 && errno == 0) ? EINVAL : errno;
        return -1;
    }
    size_t want = (size_t)st.st_size;
    if (map_bytes != 0 && map_bytes < want) want = map_bytes;  // caller clamp
    if (want < WEFT_DMABUF_HEADER_BYTES) { errno = EINVAL; return -1; }

    void* p = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return -1;

    memset(out, 0, sizeof(*out));
    out->fd = fd;
    out->base = (uint8_t*)p;
    out->map_bytes = want;
    size_t pb = 0;
    unsigned sc = 0;
    if (session_header_validate(out->base, want, &pb, &sc) == 0) {
        out->has_session = 1;
        out->ring = out->base + WEFT_DMABUF_HEADER_BYTES;
        out->payload_bytes = pb;
        out->slot_count = sc;
    }
    // has_session == 0 is the RAW road (a camera frame) — not an error.
    return 0;
}

void weft_dmabuf_import_free(weft_dmabuf_import_t* i) {
    if (i == NULL || i->base == NULL) return;
    munmap(i->base, i->map_bytes);
    memset(i, 0, sizeof(*i));
}

// ---------------------------------------------------------------------------
// Foreign-buffer cache sync
// ---------------------------------------------------------------------------

int weft_dmabuf_sync(int fd, int start, int read_only) {
    struct weft_dma_buf_sync s;
    memset(&s, 0, sizeof(s));
    s.flags = (start ? WEFT_DMA_BUF_SYNC_START : WEFT_DMA_BUF_SYNC_END) |
              (read_only ? WEFT_DMA_BUF_SYNC_READ : (WEFT_DMA_BUF_SYNC_READ | WEFT_DMA_BUF_SYNC_WRITE));
    return ioctl(fd, WEFT_DMA_BUF_IOCTL_SYNC, &s) == 0 ? 0 : -1;
}

#else  // !__linux__ — refusal stubs (the uring_rx pattern)

#include <errno.h>

static weft_dmabuf_probe_t g_probe;  // all-zero: probed=0, UNSUPPORTED

const weft_dmabuf_probe_t* weft_dmabuf_probe(void) {
    g_probe.probed = 1;
    g_probe.caps = WEFT_DMABUF_UNSUPPORTED;
    g_probe.page_size = 4096;
    return &g_probe;
}

size_t weft_dmabuf_report(char* buf, size_t buflen) {
    int n = snprintf(buf, buflen, "dmabuf: unsupported (non-linux) page=4096");
    return (n < 0) ? 0 : (size_t)n;
}

size_t weft_dmabuf_span_bytes(size_t payload_bytes, unsigned slot_count) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    return rb == 0 ? 0 : 64u + rb;
}

int weft_dmabuf_ring_alloc(weft_dmabuf_ring_t* out, size_t payload_bytes,
                           unsigned slot_count) {
    (void)out; (void)payload_bytes; (void)slot_count;
    errno = EOPNOTSUPP;
    return -1;
}

int weft_dmabuf_ring_bind_fd(weft_dmabuf_ring_t* out, int fd,
                             size_t payload_bytes, unsigned slot_count) {
    (void)out; (void)fd; (void)payload_bytes; (void)slot_count;
    errno = EOPNOTSUPP;
    return -1;
}

void weft_dmabuf_ring_free(weft_dmabuf_ring_t* r) { (void)r; }

int weft_dmabuf_import_fd(weft_dmabuf_import_t* out, int fd, size_t map_bytes) {
    (void)out; (void)fd; (void)map_bytes;
    errno = EOPNOTSUPP;
    return -1;
}

void weft_dmabuf_import_free(weft_dmabuf_import_t* i) { (void)i; }

int weft_dmabuf_sync(int fd, int start, int read_only) {
    (void)fd; (void)start; (void)read_only;
    errno = EOPNOTSUPP;
    return -1;
}

#endif  // __linux__
