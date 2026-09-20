// weft_ipc.c — Zero-Copy IPC Mesh layer 2: registry, tokens, healing
// (RFC-0016).
//
// Implementation notes:
//   - the registry is ONE fixed-size MAP_SHARED object; every mutation is
//     a single-word CAS or a release store — there is no lock anywhere;
//   - the RESERVE -> fill -> ACTIVATE protocol makes ACTIVE snapshots
//     consistent for the immutable field set (the activation release
//     store orders the field writes; scanners load the state word with
//     acquire), while heartbeats and consumer counters stay advisory;
//   - the same-name dedup is one-sided and total: the LATER activation's
//     re-scan always sees the earlier one (the earlier stays ACTIVE
//     unless crashed), so exactly one of two racing registrants yields;
//   - kill(pid, 0) is only ever a HEURISTIC leg — every crash-out needs
//     heartbeat staleness AND a dead pid (a clock jump or a PID-reuse
//     alone cannot evict a live producer);
//   - tokens are the in-tree pure-C HMAC-SHA256 over a canonical 32-byte
//     claims encoding — zero new dependencies, constant-time compare.
//
// Registry entry layout (160 bytes, little-endian; offsets from entry):
//     0   state_gen  _Atomic u64  (low u32 state, high u32 generation)
//     8   session_id      u64 (immutable after activation)
//     16  epoch           u32 (immutable; 0 never appears — fresh = 1)
//     20  transport       u32 (immutable)
//     24  producer_pid    _Atomic u32 (written first after reservation)
//     28  n_consumers     _Atomic u32 (advisory)
//     32  heartbeat_seq   _Atomic u64 (advisory; monotonic)
//     40  heartbeat_ns    _Atomic u64 (advisory; wall stamp)
//     48  payload_bytes   u32 (immutable)
//     52  slot_count      u32 (immutable)
//     56  latest_seq_pub  _Atomic u64 (advisory mirror)
//     64  name            char[40] (immutable; 5 relaxed-atomic u64 words)
//     104 token_key_id    u32 (advisory cross-check plane)
//     112 token_tag       u8[32]  (advisory; 4 relaxed-atomic u64 words)
//     144 pad[16]

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "weft_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "hmac.h"
#include "weft_shm.h"  // perm mirror asserts + verifier adapter contract

// The permission mirrors must stay in lockstep (one definition per header,
// equal values — pinned here so a drift is a compile error).
_Static_assert(WEFT_IPC_PERM_READ == WEFT_SHM_PERM_READ, "perm mirror READ");
_Static_assert(WEFT_IPC_PERM_WRITE == WEFT_SHM_PERM_WRITE, "perm mirror WRITE");
_Static_assert(WEFT_IPC_PERM_CLAIM == WEFT_SHM_PERM_CLAIM, "perm mirror CLAIM");
_Static_assert(WEFT_IPC_PERM_ADMIN == WEFT_SHM_PERM_ADMIN, "perm mirror ADMIN");
_Static_assert(WEFT_IPC_TOKEN_BYTES == WEFT_SHM_TOKEN_BYTES, "token size mirror");

// ---------------------------------------------------------------------------
// Layout constants (offsets; see the file header for the rationale)
// ---------------------------------------------------------------------------

#define E_STATE_GEN 0
#define E_SESSION 8
#define E_EPOCH 16
#define E_TRANSPORT 20
#define E_PID 24
#define E_CONSUMERS 28
#define E_HB_SEQ 32
#define E_HB_NS 40
#define E_PB 48
#define E_SLOTS 52
#define E_LATEST 56
#define E_NAME 64
#define E_KEYID 104
#define E_TAG 112

#define H_MAGIC 0
#define H_VERSION 4
#define H_HSIZE 6
#define H_FLAGS 8
#define H_COUNT 12
#define H_EBYTES 16
#define H_RBYTES 20
#define H_PID 28
#define H_CREATED 32

/// Registry open retry bound (create/validate race; cold path, bounded).
#define WEFT_IPC_OPEN_ATTEMPTS 100
/// Registration retry bound (mid-fill reaped / successor race).
#define WEFT_IPC_REG_ATTEMPTS 3

// ---------------------------------------------------------------------------
// Small helpers (byte-wise LE; static here, no TU clashes)
// ---------------------------------------------------------------------------

static void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t get_u16le(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t get_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_u64le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get_u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t unix_ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/// kill(pid, 0) heuristic: 1 = provably alive or undecidable, 0 = ESRCH.
/// EPERM (exists, other user) counts as alive — we only reap on PROOF of
/// death, never on failure to probe.
static int pid_alive(uint32_t pid) {
    if (pid == 0) return 0;
    errno = 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno != ESRCH;
}

static int name_valid(const char* name) {
    if (name == NULL) return 0;
    size_t n = 0;
    while (name[n] != '\0') {
        const char c = name[n];
        const int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
        n++;
        if (n > 80) return 0;
    }
    return n != 0 && name[0] != '-';
}

// ---------------------------------------------------------------------------
// Registry entry accessors
// ---------------------------------------------------------------------------

static uint8_t* entry_at(const weft_ipc_registry_t* reg, uint32_t slot) {
    return reg->base + WEFT_IPC_REGISTRY_HEADER_BYTES +
           (size_t)slot * WEFT_IPC_REGISTRY_ENTRY_BYTES;
}

static _Atomic uint64_t* e_a64(const weft_ipc_registry_t* reg, uint32_t slot,
                               size_t off) {
    return (_Atomic uint64_t*)(entry_at(reg, slot) + off);
}

static _Atomic uint32_t* e_a32(const weft_ipc_registry_t* reg, uint32_t slot,
                               size_t off) {
    return (_Atomic uint32_t*)(entry_at(reg, slot) + off);
}

static uint64_t sg_pack(uint32_t state, uint32_t gen) {
    return (uint64_t)state | ((uint64_t)gen << 32);
}
static uint32_t sg_state(uint64_t v) { return (uint32_t)(v & 0xFFFFFFFFu); }
static uint32_t sg_gen(uint64_t v) { return (uint32_t)(v >> 32); }

// Immutable-entry-field access: RELAXED ATOMIC word access on BOTH sides
// (the fanout.h payload-word discipline). A registrant overwrites a
// DEAD/RESERVED slot's fields with no synchronizing edge against another
// thread's scan of that slot — plain bytes would be a strict-C11 data
// race there (TSan proves it; the CAS on the state word keeps the
// OUTCOME safe, but the house bar is zero plain races, and relaxed
// atomics are free on x86). Fields published by the RESERVE->ACTIVATE
// release store still gain their happens-before from that store; the
// relaxed discipline here is about RACING writers, not ordering.
static uint64_t e_get_u64(const weft_ipc_registry_t* reg, uint32_t slot,
                          size_t off) {
    return atomic_load_explicit(e_a64(reg, slot, off), memory_order_relaxed);
}
static uint32_t e_get_u32(const weft_ipc_registry_t* reg, uint32_t slot,
                          size_t off) {
    return atomic_load_explicit(e_a32(reg, slot, off), memory_order_relaxed);
}
static void e_put_u64(weft_ipc_registry_t* reg, uint32_t slot, size_t off,
                      uint64_t v) {
    atomic_store_explicit(e_a64(reg, slot, off), v, memory_order_relaxed);
}
static void e_put_u32(weft_ipc_registry_t* reg, uint32_t slot, size_t off,
                      uint32_t v) {
    atomic_store_explicit(e_a32(reg, slot, off), v, memory_order_relaxed);
}
/// The 40-byte name and 32-byte token tag as relaxed-atomic u64 words.
static void e_get_words(const weft_ipc_registry_t* reg, uint32_t slot,
                        size_t off, uint8_t* dst, size_t bytes) {
    _Static_assert(WEFT_IPC_NAME_MAX % 8 == 0, "name words");
    for (size_t w = 0; w < bytes / 8; w++) {
        const uint64_t v = atomic_load_explicit(
            e_a64(reg, slot, off + w * 8), memory_order_relaxed);
        put_u64le(dst + w * 8, v);
    }
}
static void e_put_words(weft_ipc_registry_t* reg, uint32_t slot, size_t off,
                        const uint8_t* src, size_t bytes) {
    for (size_t w = 0; w < bytes / 8; w++) {
        const uint64_t v = get_u64le(src + w * 8);
        atomic_store_explicit(e_a64(reg, slot, off + w * 8), v,
                              memory_order_relaxed);
    }
}

/// Millisecond sleep for the open retry loop (cold path only).
static void nap_ms(long ms) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = ms * 1000000L };
    nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// Registry lifecycle
// ---------------------------------------------------------------------------

static int registry_header_valid(const uint8_t* base) {
    if (get_u32le(base + H_MAGIC) != WEFT_IPC_REGISTRY_MAGIC) return 0;
    if (get_u16le(base + H_VERSION) != WEFT_IPC_REGISTRY_VERSION) return 0;
    if (get_u16le(base + H_HSIZE) != WEFT_IPC_REGISTRY_HEADER_BYTES) return 0;
    if (get_u32le(base + H_FLAGS) != 0) return 0;
    if (get_u32le(base + H_COUNT) != WEFT_IPC_REGISTRY_ENTRY_COUNT) return 0;
    if (get_u32le(base + H_EBYTES) != WEFT_IPC_REGISTRY_ENTRY_BYTES) return 0;
    if (get_u64le(base + H_RBYTES) != WEFT_IPC_REGISTRY_BYTES) return 0;
    for (size_t i = H_PID + 8 + 8; i < WEFT_IPC_REGISTRY_HEADER_BYTES; i++) {
        if (base[i] != 0) return 0;  // reserved bytes nonzero = reject
    }
    return 1;
}

int weft_ipc_registry_open(weft_ipc_registry_t* reg, int create_if_absent,
                           int read_only) {
    if (reg == NULL) return WEFT_IPC_ERR_INVALID;
    memset(reg, 0, sizeof *reg);

    for (int attempt = 0; attempt < WEFT_IPC_OPEN_ATTEMPTS; attempt++) {
        if (!read_only && create_if_absent) {
            const int fd = open(WEFT_IPC_REGISTRY_PATH,
                                O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0666);
            if (fd >= 0) {
                if (ftruncate(fd, (off_t)WEFT_IPC_REGISTRY_BYTES) != 0) {
                    close(fd);
                    unlink(WEFT_IPC_REGISTRY_PATH);  // our botched create
                    return WEFT_IPC_ERR_SYS;
                }
                // Header via pwrite BEFORE mmap: attachers validate a
                // complete object, never a half-written one (they still
                // retry — belt and braces for the pwrite race).
                uint8_t hdr[WEFT_IPC_REGISTRY_HEADER_BYTES];
                memset(hdr, 0, sizeof hdr);
                put_u32le(hdr + H_MAGIC, WEFT_IPC_REGISTRY_MAGIC);
                put_u16le(hdr + H_VERSION, (uint16_t)WEFT_IPC_REGISTRY_VERSION);
                put_u16le(hdr + H_HSIZE, (uint16_t)WEFT_IPC_REGISTRY_HEADER_BYTES);
                put_u32le(hdr + H_FLAGS, 0);
                put_u32le(hdr + H_COUNT, WEFT_IPC_REGISTRY_ENTRY_COUNT);
                put_u32le(hdr + H_EBYTES, WEFT_IPC_REGISTRY_ENTRY_BYTES);
                put_u64le(hdr + H_RBYTES, WEFT_IPC_REGISTRY_BYTES);
                put_u32le(hdr + H_PID, (uint32_t)getpid());
                put_u64le(hdr + H_CREATED, unix_ns_now());
                size_t off = 0;
                while (off < sizeof hdr) {
                    ssize_t n = pwrite(fd, hdr + off, sizeof hdr - off, (off_t)off);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        close(fd);
                        unlink(WEFT_IPC_REGISTRY_PATH);
                        return WEFT_IPC_ERR_SYS;
                    }
                    off += (size_t)n;
                }
                void* p = mmap(NULL, WEFT_IPC_REGISTRY_BYTES, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
                if (p == MAP_FAILED) {
                    close(fd);
                    return WEFT_IPC_ERR_SYS;
                }
                reg->base = (uint8_t*)p;
                reg->bytes = WEFT_IPC_REGISTRY_BYTES;
                reg->fd = fd;
                reg->read_only = 0;
                return 0;
            }
            if (errno != EEXIST) return WEFT_IPC_ERR_SYS;
        }

        // Attach (or re-attach after losing the create race).
        const int fd = open(WEFT_IPC_REGISTRY_PATH,
                            (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC, 0);
        if (fd < 0) {
            if (errno == ENOENT && !create_if_absent) return WEFT_IPC_ERR_SYS;
            if (errno == ENOENT) {
                nap_ms(2);
                continue;  // creator between create and header — bounded retry
            }
            return WEFT_IPC_ERR_SYS;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size != (off_t)WEFT_IPC_REGISTRY_BYTES) {
            close(fd);
            if (create_if_absent) {
                nap_ms(2);
                continue;  // mid-creation size — retry
            }
            return WEFT_IPC_ERR_INVALID;
        }
        void* p = mmap(NULL, WEFT_IPC_REGISTRY_BYTES,
                       read_only ? PROT_READ : (PROT_READ | PROT_WRITE),
                       MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            close(fd);
            return WEFT_IPC_ERR_SYS;
        }
        if (!registry_header_valid((const uint8_t*)p)) {
            munmap(p, WEFT_IPC_REGISTRY_BYTES);
            close(fd);
            if (create_if_absent) {
                nap_ms(2);
                continue;
            }
            return WEFT_IPC_ERR_INVALID;  // a decoy object — refused, not guessed
        }
        reg->base = (uint8_t*)p;
        reg->bytes = WEFT_IPC_REGISTRY_BYTES;
        reg->fd = fd;
        reg->read_only = read_only ? 1 : 0;
        return 0;
    }
    return WEFT_IPC_ERR_INVALID;  // create/validate race never settled
}

void weft_ipc_registry_close(weft_ipc_registry_t* reg) {
    if (reg == NULL || reg->base == NULL) return;
    munmap(reg->base, reg->bytes);
    if (reg->fd >= 0) close(reg->fd);
    memset(reg, 0, sizeof *reg);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

/// Scan for an entry whose name matches; report slot + state + gen +
/// epoch + pid + heartbeat age. Bounded by the fixed slot count.
static int scan_name(const weft_ipc_registry_t* reg, const char* name,
                     uint32_t* out_slot, uint64_t* out_sg, uint32_t* out_epoch,
                     uint32_t* out_pid, uint64_t* out_hb_ns) {
    for (uint32_t i = 0; i < WEFT_IPC_REGISTRY_ENTRY_COUNT; i++) {
        const uint64_t sg = atomic_load_explicit(e_a64(reg, i, E_STATE_GEN),
                                                 memory_order_acquire);
        const uint32_t st = sg_state(sg);
        if (st != WEFT_IPC_SLOT_ACTIVE && st != WEFT_IPC_SLOT_DEAD) continue;
        char nm[WEFT_IPC_NAME_MAX + 1];
        e_get_words(reg, i, E_NAME, (uint8_t*)nm, WEFT_IPC_NAME_MAX);
        nm[WEFT_IPC_NAME_MAX] = '\0';
        if (strcmp(nm, name) != 0) continue;
        *out_slot = i;
        *out_sg = sg;
        if (out_epoch) *out_epoch = e_get_u32(reg, i, E_EPOCH);
        if (out_pid) *out_pid = atomic_load_explicit(e_a32(reg, i, E_PID),
                                                     memory_order_relaxed);
        if (out_hb_ns) *out_hb_ns = atomic_load_explicit(e_a64(reg, i, E_HB_NS),
                                                         memory_order_relaxed);
        return (int)st;
    }
    return -1;  // no such name
}

/// Fill the mutable/advisory fields immediately after a won reservation.
/// PID FIRST — it is the heal pass's only reliable leg during RESERVE.
static void reservation_stamp(weft_ipc_registry_t* reg, uint32_t slot) {
    atomic_store_explicit(e_a32(reg, slot, E_PID), (uint32_t)getpid(),
                          memory_order_relaxed);
    atomic_store_explicit(e_a64(reg, slot, E_HB_NS), unix_ns_now(),
                          memory_order_relaxed);
    atomic_store_explicit(e_a64(reg, slot, E_HB_SEQ), 0, memory_order_relaxed);
    atomic_store_explicit(e_a32(reg, slot, E_CONSUMERS), 0, memory_order_relaxed);
    atomic_store_explicit(e_a64(reg, slot, E_LATEST), 0, memory_order_relaxed);
}

int weft_ipc_register(weft_ipc_registry_t* reg, const char* name,
                      uint32_t transport, size_t payload_bytes,
                      unsigned slot_count, uint64_t latest_seq_now,
                      weft_ipc_session_t* out) {
    if (reg == NULL || reg->read_only || !name_valid(name) || out == NULL) {
        return WEFT_IPC_ERR_INVALID;
    }
    if (weft_fanout_ring_bytes(payload_bytes, slot_count) == 0 ||
        (transport != WEFT_IPC_TRANSPORT_NAMED &&
         transport != WEFT_IPC_TRANSPORT_MEMFD)) {
        return WEFT_IPC_ERR_INVALID;
    }
    if (strlen(name) > WEFT_IPC_NAME_MAX) return WEFT_IPC_ERR_INVALID;
    memset(out, 0, sizeof *out);

    uint64_t session_id = 0;
    if (weft_ipc_random_bytes(&session_id, sizeof session_id) != 0 ||
        session_id == 0) {
        session_id = unix_ns_now() ^ ((uint64_t)getpid() << 32);
        if (session_id == 0) session_id = 1;
    }

    for (int attempt = 0; attempt < WEFT_IPC_REG_ATTEMPTS; attempt++) {
        // ---- 1. successor / duplicate adjudication for this name ----
        uint32_t c_slot = 0;
        uint64_t c_sg = 0;
        uint32_t c_epoch = 0, c_pid = 0;
        uint64_t c_hb = 0;
        const int found = scan_name(reg, name, &c_slot, &c_sg, &c_epoch, &c_pid,
                                    &c_hb);
        uint32_t epoch = 1;
        int takeover_slot = -1;
        uint32_t takeover_gen = 0;
        if (found == (int)WEFT_IPC_SLOT_ACTIVE) {
            const uint64_t age_ns = unix_ns_now() - c_hb;
            const int stale = age_ns > 2000000000ull;  // 2s staleness floor
            if (!stale || pid_alive(c_pid)) {
                return WEFT_IPC_ERR_DUPLICATE;  // live incumbent — refuse
            }
            // Crashed incumbent: claim its slot; epoch continues (+1).
            uint64_t expected = c_sg;
            const uint64_t desired = sg_pack(WEFT_IPC_SLOT_RESERVED, sg_gen(c_sg));
            if (!atomic_compare_exchange_strong_explicit(
                    e_a64(reg, c_slot, E_STATE_GEN), &expected, desired,
                    memory_order_acq_rel, memory_order_acquire)) {
                continue;  // someone else took it — rescan
            }
            epoch = c_epoch + 1;
            takeover_slot = (int)c_slot;
            takeover_gen = sg_gen(c_sg);
        } else if (found == (int)WEFT_IPC_SLOT_DEAD) {
            // Cleanly-detached predecessor: reuse the slot, continue epoch.
            uint64_t expected = c_sg;
            const uint64_t desired = sg_pack(WEFT_IPC_SLOT_RESERVED, sg_gen(c_sg));
            if (!atomic_compare_exchange_strong_explicit(
                    e_a64(reg, c_slot, E_STATE_GEN), &expected, desired,
                    memory_order_acq_rel, memory_order_acquire)) {
                continue;
            }
            epoch = c_epoch + 1;
            takeover_slot = (int)c_slot;
            takeover_gen = sg_gen(c_sg);
        }

        // ---- 2. claim a slot (fresh registration path) ----
        uint32_t slot = 0;
        uint32_t gen = 0;
        if (takeover_slot < 0) {
            int claimed = 0;
            for (uint32_t i = 0; i < WEFT_IPC_REGISTRY_ENTRY_COUNT && !claimed;
                 i++) {
                uint64_t expected = atomic_load_explicit(
                    e_a64(reg, i, E_STATE_GEN), memory_order_acquire);
                const uint32_t st = sg_state(expected);
                if (st != WEFT_IPC_SLOT_FREE && st != WEFT_IPC_SLOT_DEAD) continue;
                expected = sg_pack(st, sg_gen(expected));
                const uint64_t desired = sg_pack(WEFT_IPC_SLOT_RESERVED,
                                                 sg_gen(expected));
                if (atomic_compare_exchange_strong_explicit(
                        e_a64(reg, i, E_STATE_GEN), &expected, desired,
                        memory_order_acq_rel, memory_order_acquire)) {
                    // Continuity: a DEAD same-name... handled above; a DEAD
                    // OTHER-name slot just starts a fresh epoch-1 session.
                    slot = i;
                    gen = sg_gen(expected);
                    claimed = 1;
                }
            }
            if (!claimed) return WEFT_IPC_ERR_FULL;
        } else {
            slot = (uint32_t)takeover_slot;
            gen = takeover_gen;
        }

        // ---- 3. fill ----
        reservation_stamp(reg, slot);
        e_put_u64(reg, slot, E_SESSION, session_id);
        e_put_u32(reg, slot, E_EPOCH, epoch);
        e_put_u32(reg, slot, E_TRANSPORT, transport);
        e_put_u32(reg, slot, E_PB, (uint32_t)payload_bytes);
        e_put_u32(reg, slot, E_SLOTS, (uint32_t)slot_count);
        uint8_t namebuf[WEFT_IPC_NAME_MAX];
        memset(namebuf, 0, sizeof namebuf);
        strncpy((char*)namebuf, name, sizeof namebuf - 1);
        e_put_words(reg, slot, E_NAME, namebuf, WEFT_IPC_NAME_MAX);
        e_put_u32(reg, slot, E_KEYID, 0);
        uint8_t zerotag[32] = {0};
        e_put_words(reg, slot, E_TAG, zerotag, 32);
        atomic_store_explicit(e_a64(reg, slot, E_LATEST), latest_seq_now,
                              memory_order_relaxed);

        // ---- 4. activate (CAS: a heal pass may have reaped us mid-fill) --
        uint64_t expected = sg_pack(WEFT_IPC_SLOT_RESERVED, gen);
        const uint64_t desired = sg_pack(WEFT_IPC_SLOT_ACTIVE, gen);
        if (!atomic_compare_exchange_strong_explicit(
                e_a64(reg, slot, E_STATE_GEN), &expected, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;  // reaped mid-fill — bounded retry
        }

        // ---- 5. one-sided same-name dedup (the later activation yields) -
        uint32_t o_slot = 0;
        uint64_t o_sg = 0;
        uint32_t o_epoch = 0, o_pid = 0;
        uint64_t o_hb = 0;
        const int other = scan_name(reg, name, &o_slot, &o_sg, &o_epoch, &o_pid,
                                    &o_hb);
        if (other == (int)WEFT_IPC_SLOT_ACTIVE && o_slot != slot) {
            const uint64_t other_session = e_get_u64(reg, o_slot, E_SESSION);
            if (other_session < session_id) {
                // The earlier activation wins; we are the duplicate.
                uint64_t exp2 = desired;
                const uint64_t des2 = sg_pack(WEFT_IPC_SLOT_DEAD, gen);
                atomic_compare_exchange_strong_explicit(
                    e_a64(reg, slot, E_STATE_GEN), &exp2, des2,
                    memory_order_acq_rel, memory_order_acquire);
                return WEFT_IPC_ERR_DUPLICATE;
            }
            // Else: ours is lower — the OTHER side's rescan yields. (The
            // later activator always sees the earlier; if it has not run
            // yet, it will before it returns.)
        }

        out->reg = reg;
        out->slot = slot;
        out->generation = gen;
        out->session_id = session_id;
        out->epoch = epoch;
        out->active = 1;
        return 0;
    }
    return WEFT_IPC_ERR_FULL;  // retries exhausted — say so, never spin
}

uint64_t weft_ipc_session_id(const weft_ipc_session_t* s) {
    return (s && s->active) ? s->session_id : 0;
}

int weft_ipc_heartbeat(weft_ipc_session_t* s, uint64_t latest_seq) {
    if (s == NULL || !s->active || s->reg == NULL || s->reg->read_only) {
        return WEFT_IPC_ERR_INVALID;
    }
    const uint64_t sg = atomic_load_explicit(e_a64(s->reg, s->slot, E_STATE_GEN),
                                             memory_order_acquire);
    if (sg_state(sg) != WEFT_IPC_SLOT_ACTIVE || sg_gen(sg) != s->generation) {
        return WEFT_IPC_ERR_INVALID;  // healed out — loud, not silent
    }
    atomic_fetch_add_explicit(e_a64(s->reg, s->slot, E_HB_SEQ), 1,
                              memory_order_relaxed);
    atomic_store_explicit(e_a64(s->reg, s->slot, E_HB_NS), unix_ns_now(),
                          memory_order_relaxed);
    atomic_store_explicit(e_a64(s->reg, s->slot, E_LATEST), latest_seq,
                          memory_order_relaxed);
    return 0;
}

int weft_ipc_unregister(weft_ipc_session_t* s) {
    if (s == NULL || s->reg == NULL || s->reg->read_only) {
        return WEFT_IPC_ERR_INVALID;
    }
    if (!s->active) return 0;  // idempotent
    uint64_t expected = sg_pack(WEFT_IPC_SLOT_ACTIVE, s->generation);
    const uint64_t desired = sg_pack(WEFT_IPC_SLOT_DEAD, s->generation);
    if (!atomic_compare_exchange_strong_explicit(
            e_a64(s->reg, s->slot, E_STATE_GEN), &expected, desired,
            memory_order_acq_rel, memory_order_acquire)) {
        s->active = 0;
        return WEFT_IPC_ERR_INVALID;  // healed out under us — loud
    }
    s->active = 0;
    return 0;
}

int weft_ipc_session_publish_token(weft_ipc_session_t* s, uint32_t key_id,
                                   const uint8_t* tag) {
    if (s == NULL || !s->active || s->reg == NULL || s->reg->read_only ||
        (key_id != 0 && tag == NULL)) {
        return WEFT_IPC_ERR_INVALID;
    }
    const uint64_t sg = atomic_load_explicit(e_a64(s->reg, s->slot, E_STATE_GEN),
                                             memory_order_acquire);
    if (sg_state(sg) != WEFT_IPC_SLOT_ACTIVE || sg_gen(sg) != s->generation) {
        return WEFT_IPC_ERR_INVALID;
    }
    e_put_u32(s->reg, s->slot, E_KEYID, key_id);
    if (tag != NULL) e_put_words(s->reg, s->slot, E_TAG, tag, 32);
    return 0;
}

// ---------------------------------------------------------------------------
// Consumers (advisory counter)
// ---------------------------------------------------------------------------

static int find_active_session(weft_ipc_registry_t* reg, uint64_t session_id,
                               uint32_t* out_slot) {
    for (uint32_t i = 0; i < WEFT_IPC_REGISTRY_ENTRY_COUNT; i++) {
        const uint64_t sg = atomic_load_explicit(e_a64(reg, i, E_STATE_GEN),
                                                 memory_order_acquire);
        if (sg_state(sg) != WEFT_IPC_SLOT_ACTIVE) continue;
        if (e_get_u64(reg, i, E_SESSION) == session_id) {
            *out_slot = i;
            return 0;
        }
    }
    return WEFT_IPC_ERR_INVALID;
}

int weft_ipc_consumer_attach(weft_ipc_registry_t* reg, uint64_t session_id) {
    if (reg == NULL || reg->read_only) return WEFT_IPC_ERR_INVALID;
    uint32_t slot = 0;
    if (find_active_session(reg, session_id, &slot) != 0) {
        return WEFT_IPC_ERR_INVALID;
    }
    atomic_fetch_add_explicit(e_a32(reg, slot, E_CONSUMERS), 1,
                              memory_order_relaxed);
    return 0;
}

int weft_ipc_consumer_leave(weft_ipc_registry_t* reg, uint64_t session_id) {
    if (reg == NULL || reg->read_only) return WEFT_IPC_ERR_INVALID;
    uint32_t slot = 0;
    if (find_active_session(reg, session_id, &slot) != 0) {
        return WEFT_IPC_ERR_INVALID;
    }
    // Guarded decrement: a crash-inflated counter must never wrap below 0.
    uint32_t cur = atomic_load_explicit(e_a32(reg, slot, E_CONSUMERS),
                                        memory_order_relaxed);
    do {
        if (cur == 0) return 0;  // already zero — leave (advisory honesty)
    } while (!atomic_compare_exchange_weak_explicit(
        e_a32(reg, slot, E_CONSUMERS), &cur, cur - 1, memory_order_relaxed,
        memory_order_relaxed));
    return 0;
}

// ---------------------------------------------------------------------------
// Discovery / healing
// ---------------------------------------------------------------------------

int weft_ipc_discover(weft_ipc_registry_t* reg, uint64_t stale_ms,
                      const char* name_filter, weft_ipc_discovered_t* out,
                      unsigned max) {
    if (reg == NULL || out == NULL) return WEFT_IPC_ERR_INVALID;
    const uint64_t now = unix_ns_now();
    const uint64_t stale_ns = stale_ms * 1000000ull;
    unsigned n = 0;
    for (uint32_t i = 0; i < WEFT_IPC_REGISTRY_ENTRY_COUNT; i++) {
        const uint64_t sg = atomic_load_explicit(e_a64(reg, i, E_STATE_GEN),
                                                 memory_order_acquire);
        if (sg_state(sg) != WEFT_IPC_SLOT_ACTIVE) continue;
        // Immutable set: consistent by the activation release store.
        weft_ipc_discovered_t d;
        memset(&d, 0, sizeof d);
        d.slot = i;
        d.session_id = e_get_u64(reg, i, E_SESSION);
        d.epoch = e_get_u32(reg, i, E_EPOCH);
        d.transport = e_get_u32(reg, i, E_TRANSPORT);
        d.payload_bytes = e_get_u32(reg, i, E_PB);
        d.slot_count = e_get_u32(reg, i, E_SLOTS);
        e_get_words(reg, i, E_NAME, (uint8_t*)d.name, WEFT_IPC_NAME_MAX);
        d.name[WEFT_IPC_NAME_MAX] = '\0';
        // Advisory set: relaxed atomic loads.
        d.producer_pid = atomic_load_explicit(e_a32(reg, i, E_PID),
                                              memory_order_relaxed);
        d.n_consumers = atomic_load_explicit(e_a32(reg, i, E_CONSUMERS),
                                             memory_order_relaxed);
        d.heartbeat_seq = atomic_load_explicit(e_a64(reg, i, E_HB_SEQ),
                                               memory_order_relaxed);
        d.heartbeat_ns = atomic_load_explicit(e_a64(reg, i, E_HB_NS),
                                              memory_order_relaxed);
        d.latest_seq_pub = atomic_load_explicit(e_a64(reg, i, E_LATEST),
                                                memory_order_relaxed);
        d.token_key_id = e_get_u32(reg, i, E_KEYID);
        d.stale = (now - d.heartbeat_ns) > stale_ns;
        d.producer_alive = pid_alive(d.producer_pid);
        if (name_filter != NULL && strcmp(d.name, name_filter) != 0) continue;
        if (n < max) out[n++] = d;
    }
    return (int)n;
}

int weft_ipc_heal(weft_ipc_registry_t* reg, uint64_t stale_ms,
                  weft_ipc_heal_report_t* report) {
    if (reg == NULL || report == NULL) return WEFT_IPC_ERR_INVALID;
    memset(report, 0, sizeof *report);
    if (reg->read_only) return WEFT_IPC_ERR_READONLY;
    const uint64_t now = unix_ns_now();
    const uint64_t stale_ns = stale_ms * 1000000ull;

    for (uint32_t i = 0; i < WEFT_IPC_REGISTRY_ENTRY_COUNT; i++) {
        _Atomic uint64_t* word = e_a64(reg, i, E_STATE_GEN);
        uint64_t sg = atomic_load_explicit(word, memory_order_acquire);
        report->scanned++;
        const uint32_t gen = sg_gen(sg);

        switch (sg_state(sg)) {
            case WEFT_IPC_SLOT_ACTIVE: {
                const uint64_t hb = atomic_load_explicit(e_a64(reg, i, E_HB_NS),
                                                         memory_order_relaxed);
                const uint32_t pid = atomic_load_explicit(e_a32(reg, i, E_PID),
                                                          memory_order_relaxed);
                if ((now - hb) > stale_ns && !pid_alive(pid)) {
                    // Stale AND provably dead — both legs, never one.
                    uint64_t expected = sg;
                    const uint64_t desired = sg_pack(WEFT_IPC_SLOT_DEAD, gen);
                    if (atomic_compare_exchange_strong_explicit(
                            word, &expected, desired, memory_order_acq_rel,
                            memory_order_acquire)) {
                        report->crashed_detected++;
                    }
                }
                break;
            }
            case WEFT_IPC_SLOT_DEAD: {
                uint64_t expected = sg;
                const uint64_t desired = sg_pack(WEFT_IPC_SLOT_FREE, gen + 1);
                if (atomic_compare_exchange_strong_explicit(
                        word, &expected, desired, memory_order_acq_rel,
                        memory_order_acquire)) {
                    report->reaped_dead++;
                }
                break;
            }
            case WEFT_IPC_SLOT_RESERVED: {
                // Abandoned reservation: pid written first after the CAS
                // win, hb_ns stamped at reservation — require stale AND
                // dead, same as ACTIVE (a live-but-slow registrar with a
                // pre-pid-read heal race is bounded by the retry logic).
                const uint64_t hb = atomic_load_explicit(e_a64(reg, i, E_HB_NS),
                                                         memory_order_relaxed);
                const uint32_t pid = atomic_load_explicit(e_a32(reg, i, E_PID),
                                                          memory_order_relaxed);
                if ((now - hb) > stale_ns && !pid_alive(pid)) {
                    uint64_t expected = sg;
                    const uint64_t desired = sg_pack(WEFT_IPC_SLOT_FREE, gen + 1);
                    if (atomic_compare_exchange_strong_explicit(
                            word, &expected, desired, memory_order_acq_rel,
                            memory_order_acquire)) {
                        report->reaped_reserved++;
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    // Post-pass count: advisory, informational.
    for (uint32_t i = 0; i < WEFT_IPC_REGISTRY_ENTRY_COUNT; i++) {
        const uint64_t sg = atomic_load_explicit(e_a64(reg, i, E_STATE_GEN),
                                                 memory_order_acquire);
        if (sg_state(sg) == WEFT_IPC_SLOT_ACTIVE) report->active_now++;
    }
    return 0;
}

weft_ring_health_t weft_ipc_heal_ring(weft_shm_map_t* m,
                                      weft_ring_health_t* after_recovery) {
    if (m == NULL || m->ring == NULL) {
        if (after_recovery) *after_recovery = WEFT_RING_CORRUPT_SPLIT_BRAIN;
        return WEFT_RING_CORRUPT_SPLIT_BRAIN;  // no ring — worst-case verdict
    }
    const size_t pb = weft_shm_payload_bytes(m);
    const unsigned slots = weft_shm_slot_count(m);
    const size_t rb = weft_shm_ring_bytes(m);
    weft_ring_health_t h =
        weft_ring_health_check(m->ring, rb, pb, slots);
    if (after_recovery) *after_recovery = h;
    if (h != WEFT_RING_HEALTHY) {
        if (weft_ring_recover((void*)m->ring, rb, pb, slots) == 0) {
            const weft_ring_health_t h2 =
                weft_ring_health_check(m->ring, rb, pb, slots);
            if (after_recovery) *after_recovery = h2;
        }
    }
    return h;
}

void weft_ipc_reader_resync(weft_fanout_reader_t* r, uint64_t baseline) {
    if (r == NULL) return;
    r->last_seq = baseline;  // public field, driver-layer discipline
}

// ---------------------------------------------------------------------------
// Capability tokens
// ---------------------------------------------------------------------------

int weft_ipc_random_bytes(void* out, size_t n) {
    if (out == NULL) return -1;
    uint8_t* p = (uint8_t*)out;
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > 256) chunk = 256;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
        if (getentropy(p + off, chunk) != 0) {
            return -1;
        }
#else
        const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        size_t done = 0;
        while (done < chunk) {
            ssize_t k = read(fd, p + off + done, chunk - done);
            if (k < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return -1;
            }
            done += (size_t)k;
        }
        close(fd);
#endif
        off += chunk;
    }
    return 0;
}

/// Canonical claims layout (32 bytes): 0 session u64 | 8 perms u32 |
/// 12 epoch u32 | 16 expiry u64 | 24 key_id u32 | 28 nonce u32.
int weft_ipc_claims_encode(const weft_ipc_claims_t* c, uint8_t* out) {
    if (c == NULL || out == NULL) return -1;
    put_u64le(out + 0, c->session_id);
    put_u32le(out + 8, c->perms);
    put_u32le(out + 12, c->epoch);
    put_u64le(out + 16, c->expiry_unix);
    put_u32le(out + 24, c->key_id);
    put_u32le(out + 28, c->nonce);
    return 0;
}

int weft_ipc_claims_decode(const uint8_t* in, weft_ipc_claims_t* c) {
    if (in == NULL || c == NULL) return -1;
    c->session_id = get_u64le(in + 0);
    c->perms = get_u32le(in + 8);
    c->epoch = get_u32le(in + 12);
    c->expiry_unix = get_u64le(in + 16);
    c->key_id = get_u32le(in + 24);
    c->nonce = get_u32le(in + 28);
    if (c->session_id == 0) return -1;  // malformed: all-zero claims
    if ((c->perms & ~(WEFT_IPC_PERM_READ | WEFT_IPC_PERM_WRITE |
                      WEFT_IPC_PERM_CLAIM | WEFT_IPC_PERM_ADMIN)) != 0) {
        return -1;  // unknown permission bits — refuse, never guess
    }
    return 0;
}

int weft_ipc_token_issue(const weft_ipc_claims_t* claims,
                         const uint8_t* key, uint8_t* out) {
    if (claims == NULL || key == NULL || out == NULL) return -1;
    uint8_t canon[32];
    if (weft_ipc_claims_encode(claims, canon) != 0) return -1;
    memcpy(out, canon, 32);
    hmac_sha256(key, WEFT_IPC_KEY_BYTES, canon, 32, out + 32);
    return 0;
}

/// Constant-time tag comparison (no early exit on first differing byte).
static int tag_equal(const uint8_t* a, const uint8_t* b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

int weft_ipc_token_verify(const uint8_t* token, const uint8_t* key,
                          uint64_t session_id, uint32_t epoch_now,
                          weft_ipc_claims_t* out) {
    if (token == NULL || key == NULL) return -2;
    weft_ipc_claims_t cl;
    if (weft_ipc_claims_decode(token, &cl) != 0) return -6;  // malformed
    uint8_t tag[32];
    hmac_sha256(key, WEFT_IPC_KEY_BYTES, token, 32, tag);
    if (!tag_equal(tag, token + 32)) return -2;  // bad HMAC
    if (session_id != 0 && cl.session_id != session_id) return -3;
    if (cl.expiry_unix != 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        if ((uint64_t)ts.tv_sec > cl.expiry_unix) return -4;  // expired
    }
    if (epoch_now != 0 && cl.epoch != epoch_now) return -5;  // incarnation
    if (out) *out = cl;
    return 0;
}

int weft_ipc_token_verify_adapter(void* ctx, const uint8_t* token,
                                  uint64_t session_id, uint32_t required_perms,
                                  uint32_t* out_perms) {
    (void)required_perms;  // serve() applies the requested-perms mask
    const weft_ipc_token_ctx_t* c = (const weft_ipc_token_ctx_t*)ctx;
    if (c == NULL || token == NULL) return 1;
    weft_ipc_claims_t cl;
    const int rc = weft_ipc_token_verify(token, c->key, session_id, c->epoch,
                                         &cl);
    if (rc == 0) {
        if (out_perms) *out_perms = cl.perms;
        return 0;
    }
    switch (rc) {
        case -4: return 2;   // expired
        case -5: return 4;   // epoch mismatch -> protocol denial
        case -3: return 1;   // wrong session -> not our token
        default: return 1;   // bad tag / malformed
    }
}
