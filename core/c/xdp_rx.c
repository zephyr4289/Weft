// xdp_rx.c — AF_XDP UMEM-as-ring-slot ingestion (RFC-0016 §4).
//
// Hand-rolled if_xdp.h uapi (kernel 4.18+, values stable; the uring_rx
// no-lib dependency discipline). The state machine (begin_fill /
// publish_desc) is universal C — it runs and is gate-tested on every host;
// the xsk plumbing runs wherever CONFIG_XDP_SOCKETS exists.

#include "xdp_rx.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// if_xdp.h uapi subset (local re-declaration — no libc/kernel-header
// version dependency; values identical to include/uapi/linux/if_xdp.h)
// ---------------------------------------------------------------------------

#define WEFT_AF_XDP 44
#define WEFT_XDP_MMAP_OFFSETS        1
#define WEFT_XDP_RX_RING             7
#define WEFT_XDP_UMEM_REG            4
#define WEFT_XDP_UMEM_FILL_RING      5
#define WEFT_XDP_UMEM_COMPLETION_RING 6
#define WEFT_XDP_USE_NEED_WAKEUP     (1u << 3)
// pgoffs + masks: EXACTLY the system's include/uapi/linux/if_xdp.h (the
// authoritative values — RX shares pgoff 0 with the socket's own mapping,
// TX is 0x80000000; unaligned addresses live in the low 48 bits)
#define WEFT_XDP_PGOFF_RX_RING          0ULL
#define WEFT_XDP_PGOFF_TX_RING          0x80000000ULL
#define WEFT_XDP_UMEM_PGOFF_FILL_RING   0x100000000ULL
#define WEFT_XDP_UMEM_PGOFF_COMPLETION_RING 0x180000000ULL
#define WEFT_XSK_UNALIGNED_BUF_OFFSET_SHIFT 48
#define WEFT_XSK_UNALIGNED_BUF_ADDR_MASK \
    ((1ULL << WEFT_XSK_UNALIGNED_BUF_OFFSET_SHIFT) - 1)

struct weft_xdp_umem_reg_ {
    uint64_t addr;
    uint64_t len;
    uint32_t chunk_size;
    uint32_t headroom;
    uint32_t flags;
};
struct weft_xdp_ring_offset_ {
    uint64_t producer;
    uint64_t consumer;
    uint64_t desc;
    uint32_t flags;
};
struct weft_xdp_mmap_offsets_ {
    struct weft_xdp_ring_offset_ rx, tx, fr, cr;
};
struct weft_sockaddr_xdp_ {
    uint16_t sxdp_family;
    uint16_t sxdp_flags;
    uint32_t sxdp_ifindex;
    uint32_t sxdp_queue_id;
    uint32_t sxdp_shared_umem_fd;
};

// WFSH constants (the shared dialect — see weft_dmabuf.c / shm_ring.c)
#define WEFT_XDP_HEADER_BYTES 64u
#define WEFT_XDP_MAGIC        0x48534657u

static uint32_t xget_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t xget_u16le(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint64_t xget_u64le(const uint8_t* p) {
    return (uint64_t)xget_u32le(p) | ((uint64_t)xget_u32le(p + 4) << 32);
}

// ---------------------------------------------------------------------------
// Capability probe
// ---------------------------------------------------------------------------

static weft_xdp_mode_t g_probe_mode = WEFT_XDP_MODE_NONE;
static int g_probe_done = 0;
static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;

static void probe_run(void) {
    int s = socket(WEFT_AF_XDP, SOCK_RAW, 0);
    if (s < 0) {
        g_probe_mode = WEFT_XDP_MODE_NONE;  // EAFNOSUPPORT: not in kernel
        g_probe_done = 1;
        return;
    }
    // The family exists. SETUP-vs-LIVE is attach-time business (bind,
    // privileges, traffic) — the probe reports SETUP as the ceiling.
    close(s);
    g_probe_mode = WEFT_XDP_MODE_SETUP;
    g_probe_done = 1;
}

weft_xdp_mode_t weft_xdp_probe(void) {
    pthread_once(&g_probe_once, probe_run);
    return g_probe_mode;
}

const char* weft_xdp_mode_str(weft_xdp_mode_t m) {
    switch (m) {
        case WEFT_XDP_MODE_URING: return "uring";
        case WEFT_XDP_MODE_SETUP: return "setup";
        case WEFT_XDP_MODE_LIVE:  return "live";
        default: return "none";
    }
}

size_t weft_xdp_report(char* buf, size_t buflen) {
    int n = snprintf(buf, buflen, "xdp: %s (af_xdp socket %s)",
                     weft_xdp_mode_str(weft_xdp_probe()),
                     weft_xdp_probe() >= WEFT_XDP_MODE_SETUP
                         ? "present" : "absent (EAFNOSUPPORT)");
    return (n < 0) ? 0 : (size_t)n;
}

// ---------------------------------------------------------------------------
// Session geometry helpers
// ---------------------------------------------------------------------------

/// The slot payload's offset within the session (== its UMEM offset).
static uint64_t slot_umem_offset(const weft_xdp_rx_t* rx, uint64_t slot) {
    const uint64_t ctrl = 16ull + 8ull * rx->slot_count;
    return (uint64_t)WEFT_XDP_HEADER_BYTES + ctrl + slot * rx->payload_bytes;
}

/// Validate the session header under the ring (page-tolerant >= semantics).
static int session_at_ring_valid(const uint8_t* ring, size_t payload_bytes,
                                 unsigned slot_count) {
    const uint8_t* base = ring - WEFT_XDP_HEADER_BYTES;
    if (xget_u32le(base + 0) != WEFT_XDP_MAGIC) return -1;
    if (xget_u16le(base + 4) != 1) return -1;
    if (xget_u16le(base + 6) != (uint16_t)WEFT_XDP_HEADER_BYTES) return -1;
    if (xget_u32le(base + 12) != payload_bytes) return -1;
    if (xget_u32le(base + 16) != slot_count) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// xsk plumbing (SETUP/LIVE)
// ---------------------------------------------------------------------------

static void xsk_teardown(weft_xdp_rx_t* rx) {
    if (rx->fill_map && rx->fill_map != MAP_FAILED) {
        munmap(rx->fill_map, rx->fill_map_bytes);
    }
    if (rx->rx_map && rx->rx_map != MAP_FAILED) {
        munmap(rx->rx_map, rx->rx_map_bytes);
    }
    rx->fill_map = NULL;
    rx->rx_map = NULL;
    rx->fill_prod = rx->fill_cons = NULL;
    rx->rx_prod = rx->rx_cons = NULL;
    rx->fill_desc = NULL;
    rx->rx_desc = NULL;
    if (rx->xsk_fd >= 0) close(rx->xsk_fd);
    rx->xsk_fd = -1;
}

/// Build the xsk over the ring's own session pages. Every step checked;
/// the caller treats any failure as "delegate to uring" (Law 4).
static int xsk_setup(weft_xdp_rx_t* rx, weft_fanout_t* f) {
    const size_t rb = weft_fanout_ring_bytes(rx->payload_bytes, rx->slot_count);
    uint8_t* ring = (uint8_t*)weft_fanout_ring(f);
    if (ring == NULL) return -1;

    // The ring must live in page-aligned shared memory (shm/dmabuf/memfd).
    // A malloc'd ring's session base is not page-aligned and cannot be
    // registered — the documented boundary.
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    rx->session_base = ring - WEFT_XDP_HEADER_BYTES;
    rx->session_span = WEFT_XDP_HEADER_BYTES + rb;
    if (((uintptr_t)rx->session_base & ((uintptr_t)page - 1)) != 0) return -1;
    if (session_at_ring_valid(ring, rx->payload_bytes, rx->slot_count) != 0) {
        rx->session_base = NULL;
        return -1;  // not a WFSH session — malloc'd or foreign layout
    }
    rx->umem_bytes = ((rx->session_span + (size_t)page - 1) / (size_t)page) *
                     (size_t)page;

    int s = socket(WEFT_AF_XDP, SOCK_RAW, 0);
    if (s < 0) {
        rx->stats.last_errno = errno;
        rx->session_base = NULL;
        return -1;
    }
    rx->xsk_fd = s;

    // UMEM over the session's own pages, unaligned chunks (>= 5.7) sized
    // to the payload so any slot payload is a legal chunk base.
    struct weft_xdp_umem_reg_ reg;
    memset(&reg, 0, sizeof(reg));
    reg.addr = (uint64_t)(uintptr_t)rx->session_base;
    reg.len = (uint64_t)rx->umem_bytes;
    reg.chunk_size = (uint32_t)rx->payload_bytes;
    if (setsockopt(s, SOL_SOCKET, WEFT_XDP_UMEM_REG, &reg, sizeof(reg)) != 0) {
        rx->stats.last_errno = errno;
        xsk_teardown(rx);
        rx->session_base = NULL;
        return -1;
    }

    static const uint32_t k_ring_entries = 64;
    if (setsockopt(s, SOL_SOCKET, WEFT_XDP_UMEM_FILL_RING,
                   &(uint32_t){ k_ring_entries }, sizeof(uint32_t)) != 0 ||
        setsockopt(s, SOL_SOCKET, WEFT_XDP_RX_RING,
                   &(uint32_t){ k_ring_entries }, sizeof(uint32_t)) != 0) {
        rx->stats.last_errno = errno;
        xsk_teardown(rx);
        rx->session_base = NULL;
        return -1;
    }

    struct weft_xdp_mmap_offsets_ off;
    socklen_t offlen = sizeof(off);
    if (getsockopt(s, SOL_SOCKET, WEFT_XDP_MMAP_OFFSETS, &off, &offlen) != 0) {
        rx->stats.last_errno = errno;
        xsk_teardown(rx);
        rx->session_base = NULL;
        return -1;
    }

    // Map the fill + rx rings (generously sized; the masks bound access).
    const size_t map_len = (size_t)page * 4;
    rx->fill_map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, s,
                        WEFT_XDP_UMEM_PGOFF_FILL_RING);
    rx->rx_map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, s,
                      WEFT_XDP_PGOFF_RX_RING);
    if (rx->fill_map == MAP_FAILED || rx->rx_map == MAP_FAILED) {
        rx->stats.last_errno = errno;
        xsk_teardown(rx);
        rx->session_base = NULL;
        return -1;
    }
    rx->fill_map_bytes = map_len;
    rx->rx_map_bytes = map_len;
    rx->fill_size = k_ring_entries;
    rx->rx_size = k_ring_entries;
    rx->fill_mask = k_ring_entries - 1;
    rx->rx_mask = k_ring_entries - 1;
    rx->fill_prod = (volatile uint32_t*)((char*)rx->fill_map + off.fr.producer);
    rx->fill_cons = (volatile uint32_t*)((char*)rx->fill_map + off.fr.consumer);
    rx->fill_desc = (uint64_t*)((char*)rx->fill_map + off.fr.desc);
    rx->rx_prod = (volatile uint32_t*)((char*)rx->rx_map + off.rx.producer);
    rx->rx_cons = (volatile uint32_t*)((char*)rx->rx_map + off.rx.consumer);
    rx->rx_desc = (struct weft_xdp_desc*)((char*)rx->rx_map + off.rx.desc);

    // NOTE: bind() is deliberately NOT attempted at attach. Without an XDP
    // program on a target interface there is no traffic to receive, and
    // binding under a BPF-attached interface is a deployment decision
    // (ifindex/queue) the CALLER owns. The session is SETUP-functional;
    // the LIVE rung is the privileged proof rig's business (Law 4).
    (void)f;
    return 0;
}

// ---------------------------------------------------------------------------
// The state machine (universal — gate-tested everywhere)
// ---------------------------------------------------------------------------

uint64_t weft_xdp_begin_fill(weft_xdp_rx_t* rx) {
    if (rx == NULL || rx->mode < WEFT_XDP_MODE_SETUP || rx->fan == NULL)
        return 0;
    if (rx->in_flight_umem != UINT64_MAX) return rx->in_flight_umem;  // one open bracket

    uint8_t* cursor = weft_fanout_begin(rx->fan);  // invalidate: SeqCst + fence (P1)
    if (cursor == NULL) return 0;
    const uint64_t slot = ((uint64_t)rx->fan->w_seq - 1ull) % rx->slot_count;
    const uint64_t off = slot_umem_offset(rx, slot);
    rx->in_flight_umem = off;
    rx->in_flight_slot = slot;

    // Post the slot's payload offset to the fill ring (unaligned-mode: the
    // umem address is the low 48 bits; the ring is small so the offset IS
    // the address, no high offset bits needed).
    // One entry, release-ordered after the bracket opened.
    if (rx->fill_prod != NULL && rx->fill_desc != NULL) {
        const uint32_t idx = (*rx->fill_prod & rx->fill_mask);
        if ((uint32_t)(*rx->fill_prod - *rx->fill_cons) < rx->fill_size) {
            rx->fill_desc[idx] = off & WEFT_XSK_UNALIGNED_BUF_ADDR_MASK;
            __atomic_store_n(rx->fill_prod, *rx->fill_prod + 1, __ATOMIC_RELEASE);
            rx->stats.fill_posts++;
        } else {
            // fill ring full: the bracket stays open, the post is retried
            // at the next begin_fill — counted, never silent
            rx->stats.last_errno = EBUSY;
        }
    } else {
        rx->stats.fill_posts++;  // test-injection path (no kernel rings)
    }
    return off;
}

uint64_t weft_xdp_publish_desc(weft_xdp_rx_t* rx, uint64_t umem_addr, uint32_t len) {
    if (rx == NULL || rx->mode < WEFT_XDP_MODE_SETUP || rx->fan == NULL) return 0;
    if (rx->in_flight_umem == UINT64_MAX) {
        rx->stats.misordered++;  // descriptor with no open bracket
        rx->stats.last_errno = EINVAL;
        return 0;
    }
    // Unaligned-mode decode: the umem address is the descriptor's low 48
    // bits (any high offset bits a driver adds are stripped — the encoding
    // the system's if_xdp.h defines and the privileged rig exercises).
    const uint64_t addr = umem_addr & WEFT_XSK_UNALIGNED_BUF_ADDR_MASK;
    if (addr != rx->in_flight_umem) {
        rx->stats.misordered++;  // not the in-flight slot — refused (Law 4)
        rx->stats.last_errno = EFAULT;
        return 0;
    }

    // The NIC wrote `len` bytes at the slot payload while the bracket was
    // open. Pad the tail (short packets) so readers see a whole frame;
    // oversize packets keep the head (truncated++, the uring_rx accounting).
    uint8_t* payload = rx->session_base + rx->in_flight_umem;
    if (len < rx->payload_bytes) {
        memset(payload + len, 0, rx->payload_bytes - len);
        rx->stats.padded++;
        rx->stats.bytes += len;
    } else if (len > rx->payload_bytes) {
        rx->stats.truncated++;
        rx->stats.bytes += rx->payload_bytes;
    } else {
        rx->stats.bytes += len;
    }

    const uint64_t seq = weft_fanout_publish(rx->fan);  // Release: the frame closes
    rx->in_flight_umem = UINT64_MAX;
    rx->stats.publishes++;
    rx->stats.frames++;
    return seq;
}

// ---------------------------------------------------------------------------
// Attach / next / detach
// ---------------------------------------------------------------------------

int weft_xdp_attach(weft_xdp_rx_t* rx, weft_fanout_t* f, int fd) {
    if (rx == NULL || f == NULL) { errno = EINVAL; return -1; }
    memset(rx, 0, sizeof(*rx));
    rx->xsk_fd = -1;
    rx->in_flight_umem = UINT64_MAX;
    rx->fan = f;
    rx->payload_bytes = f->payload_bytes;
    rx->slot_count = f->slot_count;
    if (weft_fanout_ring_bytes(rx->payload_bytes, rx->slot_count) == 0) {
        rx->stats.last_errno = EINVAL;
        errno = EINVAL;
        return -1;
    }

    // The XDP road first (kernel family present + page-aligned session).
    if (weft_xdp_probe() >= WEFT_XDP_MODE_SETUP) {
        if (xsk_setup(rx, f) == 0) {
            rx->mode = WEFT_XDP_MODE_SETUP;
            weft_xdp_begin_fill(rx);  // the first pre-bracketed slot
            return 0;
        }
        // fall through to the delegation — the refusal is counted
    } else {
        rx->stats.last_errno = EAFNOSUPPORT;
    }

    // The uring delegation (the never-a-regression road).
    if (fd < 0) {
        errno = rx->stats.last_errno ? rx->stats.last_errno : ENOTSUP;
        return -1;
    }
    if (weft_uring_attach(&rx->uring, f, fd) != 0) {
        rx->stats.last_errno = rx->uring.stats.last_errno;
        errno = rx->stats.last_errno ? rx->stats.last_errno : EINVAL;
        return -1;
    }
    rx->mode = WEFT_XDP_MODE_URING;
    return 0;
}

uint64_t weft_xdp_next(weft_xdp_rx_t* rx) {
    if (rx == NULL || rx->fan == NULL) return 0;
    if (rx->mode == WEFT_XDP_MODE_URING) {
        const uint64_t seq = weft_uring_next(&rx->uring);
        if (seq != 0) rx->stats.delegated++;
        return seq;
    }
    if (rx->mode < WEFT_XDP_MODE_SETUP) return 0;

    // Consume one rx descriptor from the shared ring (no syscall; the
    // steady state is shared-memory polling — need_wakeup is the only
    // syscall gate, attempted below only when the driver asks).
    if (rx->rx_prod != NULL && rx->rx_cons != NULL && rx->rx_desc != NULL) {
        const uint32_t prod = __atomic_load_n(rx->rx_prod, __ATOMIC_ACQUIRE);
        if (prod != *rx->rx_cons) {
            const struct weft_xdp_desc* d =
                &rx->rx_desc[*rx->rx_cons & rx->rx_mask];
            const uint64_t seq = weft_xdp_publish_desc(rx, d->addr, d->len);
            rx->stats.rx_descs++;
            __atomic_store_n(rx->rx_cons, *rx->rx_cons + 1, __ATOMIC_RELEASE);
            if (seq != 0) {
                weft_xdp_begin_fill(rx);  // the next pre-bracketed slot
                rx->mode = WEFT_XDP_MODE_LIVE;  // descriptors flow
            }
            return seq;
        }
        // Empty ring with a posted fill: one bounded wakeup attempt (the
        // documented busy-poll discipline — recvfrom(NULL,0,MSG_DONTWAIT)
        // kicks the driver without copying; counted, never spun; Law 1).
        if (rx->in_flight_umem != UINT64_MAX && rx->xsk_fd >= 0) {
            rx->stats.wakeups++;
            (void)recvfrom(rx->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);
        }
    }
    return 0;
}

void weft_xdp_detach(weft_xdp_rx_t* rx) {
    if (rx == NULL) return;
    if (rx->mode == WEFT_XDP_MODE_URING) {
        weft_uring_detach(&rx->uring);
    } else if (rx->mode >= WEFT_XDP_MODE_SETUP) {
        xsk_teardown(rx);
    }
    memset(rx, 0, sizeof(*rx));
    rx->xsk_fd = -1;
    rx->in_flight_umem = UINT64_MAX;
}

void weft_xdp_stats(const weft_xdp_rx_t* rx, weft_xdp_stats_t* out) {
    if (out == NULL) return;
    if (rx == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = rx->stats;
}

#else  // !__linux__ — the uring_rx pattern: refusal stubs

#include <errno.h>

weft_xdp_mode_t weft_xdp_probe(void) { return WEFT_XDP_MODE_NONE; }
const char* weft_xdp_mode_str(weft_xdp_mode_t m) {
    (void)m;
    return "none";
}
size_t weft_xdp_report(char* buf, size_t buflen) {
    int n = snprintf(buf, buflen, "xdp: none (non-linux)");
    return (n < 0) ? 0 : (size_t)n;
}
int weft_xdp_attach(weft_xdp_rx_t* rx, weft_fanout_t* f, int fd) {
    (void)rx; (void)f; (void)fd;
    errno = EOPNOTSUPP;
    return -1;
}
uint64_t weft_xdp_begin_fill(weft_xdp_rx_t* rx) { (void)rx; return 0; }
uint64_t weft_xdp_publish_desc(weft_xdp_rx_t* rx, uint64_t a, uint32_t l) {
    (void)rx; (void)a; (void)l;
    return 0;
}
uint64_t weft_xdp_next(weft_xdp_rx_t* rx) { (void)rx; return 0; }
void weft_xdp_detach(weft_xdp_rx_t* rx) { (void)rx; }
void weft_xdp_stats(const weft_xdp_rx_t* rx, weft_xdp_stats_t* out) {
    (void)rx;
    if (out) memset(out, 0, sizeof(*out));
}

#endif  // __linux__
