// weft_shm.c — Zero-Copy IPC Mesh layer 1: the SHM fabric (RFC-0016).
//
// Implementation notes (mirroring shm_ring.c discipline):
//   - byte-wise little-endian encode/decode, no packed structs;
//   - the WFSH v1 header bytes are IDENTICAL to shm_ring.c's (RFC-0011
//     table) — re-encoded here because the creator road writes them via
//     pwrite through the fd, before any mapping exists;
//   - POSIX only (memfd/seals/SCM_RIGHTS); Windows uses RFC-0011 named
//     mappings — no _WIN32 branch here, by design;
//   - cold paths may syscall/allocate freely; nothing here runs in the
//     data path (Law 2's boundary).
//
// Sealing stance (probed green before this file was written): seals are
// GROW|SHRINK|SEAL — F_SEAL_WRITE would refuse while the producer's own
// writable mapping exists (memfd_create(2)) and consumer write-protection
// is enforced anyway by the RO-view's open()-time access rights, which no
// recipient of that fd can escalate.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // memfd pieces, open O_CLOEXEC, struct cmsghdr extras
#endif

#include "weft_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Kernel uapi fallbacks (glibc < 2.27 hosts; values from linux/fcntl.h)
// ---------------------------------------------------------------------------

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0008
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0004
#endif

/// The seal set this fabric applies (documented stance, above).
#define WEFT_SHM_SEAL_SET (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)

// ---------------------------------------------------------------------------
// Small local helpers (byte-wise LE, same discipline as shm_ring.c)
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

/// Nonce: 8 random bytes; getentropy with a time/pid-mixed xorshift
/// fallback (a fallback nonce is a freshness aid, not a security claim —
/// the token HMAC is the cryptographic binding).
static uint64_t nonce_next(void) {
    uint8_t buf[8];
    memset(buf, 0, sizeof buf);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    if (getentropy(buf, sizeof buf) != 0)
#endif
    {
        uint64_t s = (uint64_t)unix_ns_now() ^ ((uint64_t)getpid() << 32);
        for (size_t i = 0; i < sizeof buf; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;  // xorshift64
            buf[i] = (uint8_t)s;
        }
    }
    return get_u64le(buf);
}

/// Bounded robust I/O: exact-length send/recv with EINTR retry.
static int send_all(int sock, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(sock, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int read_exact(int sock, void* data, size_t len) {
    uint8_t* p = (uint8_t*)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(sock, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -4;  // EOF before the full frame
        off += (size_t)n;
    }
    return 0;
}

/// CPU pause hint for the bounded park (x86 `rep; nop`, ARM `yield`).
static inline void weft_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

// ---------------------------------------------------------------------------
// Sealed anonymous session lifecycle
// ---------------------------------------------------------------------------

/// WFSH v1 session header, byte-identical to shm_ring.c's header_write
/// (RFC-0011 table — cross-referenced, not re-derived).
static void wfsh_header_encode(uint8_t* hdr, size_t payload_bytes,
                               unsigned slot_count) {
    memset(hdr, 0, WEFT_SHM_HEADER_BYTES);
    put_u32le(hdr + 0, WEFT_SHM_MAGIC);
    put_u16le(hdr + 4, (uint16_t)WEFT_SHM_VERSION);
    put_u16le(hdr + 6, (uint16_t)WEFT_SHM_HEADER_BYTES);
    put_u32le(hdr + 8, 0);  // flags
    put_u32le(hdr + 12, (uint32_t)payload_bytes);
    put_u32le(hdr + 16, (uint32_t)slot_count);
    put_u64le(hdr + 20, (uint64_t)weft_fanout_ring_bytes(payload_bytes, slot_count));
    put_u32le(hdr + 28, (uint32_t)getpid());
    put_u64le(hdr + 32, unix_ns_now());
}

int weft_shm_memfd_create(size_t payload_bytes, unsigned slot_count,
                          weft_shm_memfd_t* out) {
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0 || payload_bytes > 0xFFFFFFFFu || slot_count > 0xFFFFFFFFu) {
        return -1;
    }
    const size_t total = (size_t)WEFT_SHM_HEADER_BYTES + rb;

    int fd = (int)syscall(SYS_memfd_create, "weft-session", (unsigned)MFD_ALLOW_SEALING);
    if (fd < 0) return -1;

    if (ftruncate(fd, (off_t)total) != 0) {
        close(fd);
        return -1;
    }

    // Header via pwrite: no mapping exists yet, and ftruncate's tmpfs
    // zero-fill covers the ring (latestSeq=0, all slots invalidated — the
    // fresh-ring invariant the S-series pins).
    uint8_t hdr[WEFT_SHM_HEADER_BYTES];
    wfsh_header_encode(hdr, payload_bytes, slot_count);
    size_t off = 0;
    while (off < sizeof hdr) {
        ssize_t n = pwrite(fd, hdr + off, sizeof hdr - off, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }

    // Seal the geometry BEFORE any fd leaves this process: from here on
    // no holder — including future hostile ones — can resize the object.
    if (fcntl(fd, F_ADD_SEALS, WEFT_SHM_SEAL_SET) != 0) {
        close(fd);
        return -1;
    }

    // Producer mapping: the full validated attach (magic/version/size/
    // reserved-zero — we eat our own dogfood on the way in).
    if (weft_shm_attach_fd(fd, &out->map, 0) != 0) {
        close(fd);
        return -1;
    }
    out->fd_ro = -1;
    out->writable = 1;
    const int seals = fcntl(fd, F_GET_SEALS);
    out->seals = seals > 0 ? (uint32_t)seals : 0u;
    return 0;
}

int weft_shm_memfd_ro_view(weft_shm_memfd_t* m) {
    if (m == NULL || m->map.fd < 0) return -1;
    char path[32];
    snprintf(path, sizeof path, "/proc/self/fd/%d", m->map.fd);
    const int ro = open(path, O_RDONLY | O_CLOEXEC);
    if (ro < 0) return -1;
    // Identity check: the RO view MUST be the same object (dev+ino+size).
    // A mismatched reopen is refused, never guessed at (Law 4).
    struct stat a, b;
    if (fstat(m->map.fd, &a) != 0 || fstat(ro, &b) != 0 ||
        a.st_dev != b.st_dev || a.st_ino != b.st_ino ||
        a.st_size != b.st_size) {
        close(ro);
        return -1;
    }
    if (m->fd_ro >= 0 && m->fd_ro != m->map.fd) close(m->fd_ro);
    m->fd_ro = ro;
    return 0;
}

int weft_shm_memfd_ro_fd(const weft_shm_memfd_t* m) {
    return m ? m->fd_ro : -1;
}

void weft_shm_memfd_destroy(weft_shm_memfd_t* m) {
    if (m == NULL) return;
    const int fd_ro = m->fd_ro;
    const int map_fd = m->map.fd;
    weft_shm_destroy(&m->map);  // unmap + close map.fd (idempotent)
    if (fd_ro >= 0 && fd_ro != map_fd) close(fd_ro);
    memset(m, 0, sizeof *m);
}

// ---------------------------------------------------------------------------
// SCM_RIGHTS primitives
// ---------------------------------------------------------------------------

int weft_shm_send_fd(int sock, int fd, const void* data, size_t len) {
    if (sock < 0 || fd < 0 || (data == NULL && len > 0) || len > 480) return -2;

    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    struct iovec iov;
    iov.iov_base = (void*)data;
    iov.iov_len = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = len > 0 ? 1 : 0;

    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } u;
    memset(&u, 0, sizeof u);
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));

    for (;;) {
        ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return (n == (ssize_t)len) ? 0 : -1;
    }
}

int weft_shm_recv_fd(int sock, int* fd_out, void* buf, size_t buflen,
                     size_t* got) {
    if (sock < 0 || (buf == NULL && buflen > 0) || got == NULL) return -2;
    *got = 0;
    if (fd_out) *fd_out = -1;

    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = buflen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = buflen > 0 ? 1 : 0;

    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } u;
    memset(&u, 0, sizeof u);
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;

    ssize_t n;
    for (;;) {
        n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        break;
    }
    // A return of 0 is EOF ONLY when no control message arrived: a
    // zero-length data message carrying an fd (iovec omitted) is a
    // MESSAGE — the cmsg below is processed before any EOF verdict.
    if (n == 0 && CMSG_FIRSTHDR(&msg) == NULL) return -4;  // orderly EOF

    *got = (size_t)n;
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    if (cm == NULL) return 1;  // data-only message — legitimate, caller decides
    if (msg.msg_flags & MSG_CTRUNC) return -3;
    if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) return -3;
    if (cm->cmsg_len != CMSG_LEN(sizeof(int))) return -3;  // exactly ONE fd
    if (CMSG_NXTHDR(&msg, cm) != NULL) return -3;          // no extras

    int fd;
    memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    if (fd < 0) return -3;
    if (fd_out == NULL) {
        close(fd);  // no destination — close loudly rather than leak
        return -2;
    }
    *fd_out = fd;
    return 0;
}

// ---------------------------------------------------------------------------
// Handshake wire codec
// ---------------------------------------------------------------------------
//
// HELLO (96B): 0 magic u32 | 4 version u16 | 6 cmd u16 | 8 requested u32
//              | 12 session u64 | 20 nonce u64 | 28 token[64] | 92 pad[4]
// GRANT (64B): 0 magic u32 | 4 version u16 | 6 status u16 | 8 session u64
//              | 16 nonce u64 | 24 payload u32 | 28 slots u32 | 32 ring u64
//              | 40 epoch u32 | 44 granted u32 | 48 latest u64 | 56 pad[8]

static void hello_encode(uint8_t* h, uint32_t cmd, uint32_t requested,
                         uint64_t session, uint64_t nonce, const uint8_t* token) {
    memset(h, 0, WEFT_SHM_HELLO_BYTES);
    put_u32le(h + 0, WEFT_SHM_HELLO_MAGIC);
    put_u16le(h + 4, (uint16_t)WEFT_SHM_HANDSHAKE_VERSION);
    put_u16le(h + 6, (uint16_t)cmd);
    put_u32le(h + 8, requested);
    put_u64le(h + 12, session);
    put_u64le(h + 20, nonce);
    if (token != NULL) memcpy(h + 28, token, WEFT_SHM_TOKEN_BYTES);
}

static void grant_encode(uint8_t* g, uint32_t status, uint64_t session,
                         uint64_t nonce, uint32_t payload_bytes,
                         uint32_t slot_count, uint64_t ring_bytes,
                         uint32_t epoch, uint32_t granted, uint64_t latest) {
    memset(g, 0, WEFT_SHM_GRANT_BYTES);
    put_u32le(g + 0, WEFT_SHM_GRANT_MAGIC);
    put_u16le(g + 4, (uint16_t)WEFT_SHM_HANDSHAKE_VERSION);
    put_u16le(g + 6, (uint16_t)status);
    put_u64le(g + 8, session);
    put_u64le(g + 16, nonce);
    put_u32le(g + 24, payload_bytes);
    put_u32le(g + 28, slot_count);
    put_u64le(g + 32, ring_bytes);
    put_u32le(g + 40, epoch);
    put_u32le(g + 44, granted);
    put_u64le(g + 48, latest);
}

static int grant_send(int conn, const uint8_t* g, int fd) {
    if (fd >= 0) return weft_shm_send_fd(conn, fd, g, WEFT_SHM_GRANT_BYTES);
    return send_all(conn, g, WEFT_SHM_GRANT_BYTES);
}

/// Map the verifier's denial int onto a GRANT status (the adapter in
/// weft_ipc returns 0 / 1 token / 2 expired / 3 perms / 4 epoch).
static uint32_t verifier_status(int rc) {
    switch (rc) {
        case 2: return WEFT_SHM_GRANT_DENIED_EXPIRED;
        case 3: return WEFT_SHM_GRANT_DENIED_PERMS;
        case 4: return WEFT_SHM_GRANT_DENIED_PROTOCOL;  // epoch mismatch
        default: return WEFT_SHM_GRANT_DENIED_TOKEN;
    }
}

static int token_is_zero(const uint8_t* t) {
    for (size_t i = 0; i < WEFT_SHM_TOKEN_BYTES; i++) {
        if (t[i] != 0) return 0;
    }
    return 1;
}

int weft_shm_handshake_serve(int conn, const weft_shm_memfd_t* m,
                             uint64_t session_id, uint32_t epoch,
                             weft_shm_token_verifier_t verify,
                             void* verify_ctx) {
    if (m == NULL || m->map.base == NULL) return -1;
    // Session facts from the object itself — the GRANT's geometry words
    // are exactly what the client will cross-check against the sealed
    // header, so a lying or stale server is caught by construction.
    const uint32_t pb = (uint32_t)weft_shm_payload_bytes(&m->map);
    const uint32_t slots = (uint32_t)weft_shm_slot_count(&m->map);
    const uint64_t rb = weft_shm_ring_bytes(&m->map);
    const uint64_t latest_seq_now = weft_shm_latest_seq(&m->map);
    const int fd_ro_export = m->fd_ro;
    const int fd_rw_export = m->writable ? m->map.fd : -1;

    uint8_t hello[WEFT_SHM_HELLO_BYTES];
    if (read_exact(conn, hello, sizeof hello) != 0) return -1;

    const uint32_t magic = get_u32le(hello + 0);
    const uint32_t version = get_u16le(hello + 4);
    const uint32_t cmd = get_u16le(hello + 6);
    const uint32_t requested = get_u32le(hello + 8);
    const uint64_t hello_session = get_u64le(hello + 12);
    const uint64_t nonce = get_u64le(hello + 20);
    const uint8_t* token = hello + 28;
    const uint8_t pad_ok = (hello[92] | hello[93] | hello[94] | hello[95]) == 0;

    if (magic != WEFT_SHM_HELLO_MAGIC || version != WEFT_SHM_HANDSHAKE_VERSION ||
        !pad_ok) {
        uint8_t g[WEFT_SHM_GRANT_BYTES];
        grant_encode(g, WEFT_SHM_GRANT_DENIED_PROTOCOL, session_id, nonce,
                     pb, slots, rb, epoch, 0, 0);
        (void)grant_send(conn, g, -1);
        return 0;
    }
    if (hello_session != 0 && hello_session != session_id) {
        uint8_t g[WEFT_SHM_GRANT_BYTES];
        grant_encode(g, WEFT_SHM_GRANT_DENIED_SESSION, session_id, nonce,
                     pb, slots, rb, epoch, 0, 0);
        (void)grant_send(conn, g, -1);
        return 0;
    }

    uint8_t g[WEFT_SHM_GRANT_BYTES];
    const int anon = token_is_zero(token);

    if (cmd == WEFT_SHM_CMD_PING) {
        grant_encode(g, WEFT_SHM_GRANT_OK, session_id, nonce, pb, slots, rb,
                     epoch, 0, latest_seq_now);
        return grant_send(conn, g, -1) == 0 ? 0 : -1;
    }

    if (cmd == WEFT_SHM_CMD_SUBSCRIBE) {
        uint32_t granted = 0;
        if ((requested & ~(WEFT_SHM_PERM_READ | WEFT_SHM_PERM_CLAIM)) == 0) {
            // Anonymous posture: READ|CLAIM by default, or the subset asked.
            granted = (requested != 0) ? requested : WEFT_SHM_PERM_DEFAULT;
        } else {
            if (anon || verify == NULL) {
                grant_encode(g, WEFT_SHM_GRANT_DENIED_ANON, session_id, nonce,
                             pb, slots, rb, epoch, 0, 0);
                return grant_send(conn, g, -1) == 0 ? 0 : -1;
            }
            uint32_t token_perms = 0;
            const int rc = verify(verify_ctx, token, session_id, requested,
                                  &token_perms);
            if (rc != 0) {
                grant_encode(g, verifier_status(rc), session_id, nonce,
                             pb, slots, rb, epoch, 0, 0);
                return grant_send(conn, g, -1) == 0 ? 0 : -1;
            }
            granted = token_perms & requested;
            if (granted == 0) {
                grant_encode(g, WEFT_SHM_GRANT_DENIED_PERMS, session_id, nonce,
                             pb, slots, rb, epoch, 0, 0);
                return grant_send(conn, g, -1) == 0 ? 0 : -1;
            }
        }
        const int fd = (granted & WEFT_SHM_PERM_WRITE)
                           ? fd_rw_export
                           : fd_ro_export;
        if (fd < 0) {
            // Server is not offering the descriptor class this grant needs.
            grant_encode(g, WEFT_SHM_GRANT_DENIED_PROTOCOL, session_id, nonce,
                         pb, slots, rb, epoch, 0, 0);
            return grant_send(conn, g, -1) == 0 ? 0 : -1;
        }
        grant_encode(g, WEFT_SHM_GRANT_OK, session_id, nonce, pb, slots, rb,
                     epoch, granted, latest_seq_now);
        return grant_send(conn, g, fd) == 0 ? 0 : -1;
    }

    if (cmd == WEFT_SHM_CMD_WRITER_HANDOFF) {
        if (anon || verify == NULL) {
            grant_encode(g, WEFT_SHM_GRANT_DENIED_ANON, session_id, nonce,
                         pb, slots, rb, epoch, 0, 0);
            return grant_send(conn, g, -1) == 0 ? 0 : -1;
        }
        uint32_t token_perms = 0;
        const int rc = verify(verify_ctx, token, session_id,
                              WEFT_SHM_PERM_ADMIN, &token_perms);
        if (rc != 0) {
            grant_encode(g, verifier_status(rc), session_id, nonce,
                         pb, slots, rb, epoch, 0, 0);
            return grant_send(conn, g, -1) == 0 ? 0 : -1;
        }
        if (!(token_perms & WEFT_SHM_PERM_ADMIN)) {
            grant_encode(g, WEFT_SHM_GRANT_DENIED_PERMS, session_id, nonce,
                         pb, slots, rb, epoch, 0, 0);
            return grant_send(conn, g, -1) == 0 ? 0 : -1;
        }
        if (fd_ro_export < 0) {
            grant_encode(g, WEFT_SHM_GRANT_DENIED_PROTOCOL, session_id, nonce,
                         pb, slots, rb, epoch, 0, 0);
            return grant_send(conn, g, -1) == 0 ? 0 : -1;
        }
        // The successor reads the old ring through the RO view to seed its
        // numbering — a READ grant, token-bound to the ADMIN holder.
        grant_encode(g, WEFT_SHM_GRANT_OK, session_id, nonce, pb, slots, rb,
                     epoch, WEFT_SHM_PERM_READ | WEFT_SHM_PERM_ADMIN,
                     latest_seq_now);
        return grant_send(conn, g, fd_ro_export) == 0 ? 0 : -1;
    }

    // Unknown command.
    grant_encode(g, WEFT_SHM_GRANT_DENIED_PROTOCOL, session_id, nonce,
                 pb, slots, rb, epoch, 0, 0);
    return grant_send(conn, g, -1) == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------

/// Shared GRANT intake: recv (+/- fd), validate framing, fill grant info.
/// Returns 0 (grant parsed; fd>=0 iff a fd arrived), -1 protocol/I-O,
/// -4 EOF. `fd_out` receives the descriptor when one arrives.
static int grant_recv(int conn, weft_shm_grant_info_t* grant, int* fd_out) {
    uint8_t g[WEFT_SHM_GRANT_BYTES];
    size_t got = 0;
    int fd = -1;
    const int rc = weft_shm_recv_fd(conn, &fd, g, sizeof g, &got);
    if (rc == -1 || rc == -2 || rc == -3) return -1;
    if (rc == -4) return -4;
    if (rc == 1) fd = -1;                 // data-only message
    if (got != sizeof g) return -1;       // short frame — protocol error

    grant->status = get_u16le(g + 6);
    grant->session_id = get_u64le(g + 8);
    grant->nonce = get_u64le(g + 16);
    grant->payload_bytes = get_u32le(g + 24);
    grant->slot_count = get_u32le(g + 28);
    grant->ring_bytes = get_u64le(g + 32);
    grant->epoch = get_u32le(g + 40);
    grant->perms_granted = get_u32le(g + 44);
    grant->latest_seq = get_u64le(g + 48);

    if (get_u32le(g + 0) != WEFT_SHM_GRANT_MAGIC) return -1;
    if (get_u16le(g + 4) != WEFT_SHM_HANDSHAKE_VERSION) return -1;
    *fd_out = fd;
    return 0;
}

int weft_shm_handshake_connect_fd(int conn, uint32_t requested_perms,
                                  uint64_t expect_session_id,
                                  const uint8_t* token,
                                  weft_shm_memfd_t* out,
                                  weft_shm_grant_info_t* grant) {
    if (out == NULL || grant == NULL) return -2;
    memset(out, 0, sizeof *out);
    memset(grant, 0, sizeof *grant);
    if (token != NULL && token_is_zero(token)) token = NULL;

    if (requested_perms == 0) requested_perms = WEFT_SHM_PERM_DEFAULT;
    if ((requested_perms & ~(WEFT_SHM_PERM_READ | WEFT_SHM_PERM_CLAIM |
                             WEFT_SHM_PERM_WRITE | WEFT_SHM_PERM_ADMIN)) != 0) {
        return -2;  // unknown permission bits — refuse, never guess
    }

    const uint64_t nonce = nonce_next();
    uint8_t hello[WEFT_SHM_HELLO_BYTES];
    hello_encode(hello, WEFT_SHM_CMD_SUBSCRIBE, requested_perms,
                 expect_session_id, nonce, token);
    if (send_all(conn, hello, sizeof hello) != 0) return -1;

    int fd = -1;
    const int rc = grant_recv(conn, grant, &fd);
    if (rc != 0) return rc == -4 ? -1 : -1;

    // Replay binding first (Law 4); then the VERDICT — a denial's
    // session_id is the SERVER's (the client asked for one it now knows is
    // wrong), so session binding is meaningful only on an OK grant.
    if (grant->nonce != nonce) return -1;
    if (grant->status != WEFT_SHM_GRANT_OK) {
        if (fd >= 0) close(fd);
        return -2;  // grant->status carries the explicit reason
    }
    if (expect_session_id != 0 && grant->session_id != expect_session_id) {
        close(fd);
        return -1;
    }
    if (fd < 0) return -1;  // claimed OK but granted no descriptor

    // Principle of least astonishment: never accept more than we asked.
    if ((grant->perms_granted & ~requested_perms) != 0) {
        close(fd);
        return -3;
    }

    const int writable = (grant->perms_granted & WEFT_SHM_PERM_WRITE) != 0;
    if (weft_shm_attach_fd(fd, &out->map, writable ? 0 : 1) != 0) {
        // A server granting WRITE but handing an RO fd fails HERE with
        // EACCES from the kernel — the lie never reaches the data path.
        close(fd);
        return -1;
    }
    // Geometry cross-check: GRANT words vs the sealed object's own header.
    if ((uint32_t)weft_shm_payload_bytes(&out->map) != grant->payload_bytes ||
        (uint32_t)weft_shm_slot_count(&out->map) != grant->slot_count ||
        (uint64_t)weft_shm_ring_bytes(&out->map) != grant->ring_bytes) {
        weft_shm_destroy(&out->map);
        return -3;
    }
    out->fd_ro = out->map.fd;  // imported view (alias — destroy closes once)
    out->writable = writable;
    const int seals = fcntl(out->map.fd, F_GET_SEALS);
    out->seals = seals > 0 ? (uint32_t)seals : 0u;
    return 0;
}

int weft_shm_handshake_ping_fd(int conn, uint64_t expect_session_id,
                               weft_shm_grant_info_t* grant) {
    if (grant == NULL) return -2;
    memset(grant, 0, sizeof *grant);

    const uint64_t nonce = nonce_next();
    uint8_t hello[WEFT_SHM_HELLO_BYTES];
    hello_encode(hello, WEFT_SHM_CMD_PING, 0, expect_session_id, nonce, NULL);
    if (send_all(conn, hello, sizeof hello) != 0) return -1;

    int fd = -1;
    const int rc = grant_recv(conn, grant, &fd);
    if (rc != 0) return -1;
    if (fd >= 0) {
        close(fd);  // a PING answered with a descriptor is a violation
        return -1;
    }
    if (grant->nonce != nonce) return -1;
    if (grant->status != WEFT_SHM_GRANT_OK) return -2;
    if (expect_session_id != 0 && grant->session_id != expect_session_id) {
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Named-socket conveniences
// ---------------------------------------------------------------------------

static int sock_name_valid(const char* name) {
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

int weft_shm_sock_path(const char* name, char* buf, size_t buflen) {
    if (!sock_name_valid(name) || buf == NULL) return -1;
    const int n = snprintf(buf, buflen, "/tmp/weft-ipc/%s.sock", name);
    if (n < 0 || (size_t)n >= buflen) return -1;
    return 0;
}

int weft_shm_listen(const char* name, int* out_sock) {
    char path[128];
    if (weft_shm_sock_path(name, path, sizeof path) != 0 || out_sock == NULL) {
        return -1;
    }
    if (mkdir("/tmp/weft-ipc", 0777) != 0 && errno != EEXIST) return -1;

    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    const size_t plen = strlen(path);
    if (plen >= sizeof sa.sun_path) {
        close(s);
        return -1;  // name grammar allows 80 chars; path fits 108 — refused
    }
    memcpy(sa.sun_path, path, plen + 1);

    if (bind(s, (struct sockaddr*)&sa, sizeof sa) != 0) {
        if (errno == EADDRINUSE) {
            // Live listener or stale socket file? Probe with a connect:
            // live → refuse (Law 4); stale → unlink is OUR explicit choice.
            const int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            int live = 0;
            if (c >= 0) {
                live = connect(c, (struct sockaddr*)&sa, sizeof sa) == 0;
                close(c);
            }
            if (live) {
                close(s);
                return -1;  // a server already owns this name
            }
            if (unlink(path) != 0 && errno != ENOENT) {
                close(s);
                return -1;
            }
            if (bind(s, (struct sockaddr*)&sa, sizeof sa) != 0) {
                close(s);
                return -1;
            }
        } else {
            close(s);
            return -1;
        }
    }
    if (listen(s, 64) != 0) {
        close(s);
        return -1;
    }
    *out_sock = s;
    return 0;
}

int weft_shm_connect(const char* name) {
    char path[128];
    if (weft_shm_sock_path(name, path, sizeof path) != 0) return -1;
    const int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    const size_t plen = strlen(path);
    if (plen >= sizeof sa.sun_path) return -1;
    memcpy(sa.sun_path, path, plen + 1);
    if (connect(c, (struct sockaddr*)&sa, sizeof sa) != 0) {
        close(c);
        return -1;
    }
    return c;
}

int weft_shm_accept(int listen_sock) {
    if (listen_sock < 0) return -1;
    for (;;) {
        const int c = accept4(listen_sock, NULL, NULL, SOCK_CLOEXEC);
        if (c >= 0) return c;
        if (errno == EINTR) continue;
        return -1;
    }
}

int weft_shm_handshake_connect(const char* name, uint32_t requested_perms,
                               uint64_t expect_session_id,
                               const uint8_t* token,
                               weft_shm_memfd_t* out,
                               weft_shm_grant_info_t* grant) {
    const int c = weft_shm_connect(name);
    if (c < 0) return -1;
    const int rc = weft_shm_handshake_connect_fd(c, requested_perms,
                                                 expect_session_id, token,
                                                 out, grant);
    close(c);
    return rc;
}

// ---------------------------------------------------------------------------
// Doorbell-free bounded park
// ---------------------------------------------------------------------------

uint64_t weft_shm_latest_seq(const weft_shm_map_t* m) {
    if (m == NULL || m->ring == NULL) return 0;
    const _Atomic uint64_t* latest = (const _Atomic uint64_t*)m->ring;
    return atomic_load_explicit(latest, memory_order_acquire);
}

int weft_shm_park(const weft_shm_map_t* m, uint64_t* last_seq,
                  uint32_t spin_cap) {
    if (m == NULL || m->ring == NULL || last_seq == NULL) return -1;
    const _Atomic uint64_t* latest = (const _Atomic uint64_t*)m->ring;
    for (uint32_t i = 0; i < spin_cap; i++) {
        const uint64_t L = atomic_load_explicit(latest, memory_order_acquire);
        if (L != *last_seq) {
            *last_seq = L;
            return 0;  // fresh frame observed — bounded, no syscall
        }
        weft_pause();
    }
    return 1;  // budget exhausted — the caller re-parks at its own cadence
}
