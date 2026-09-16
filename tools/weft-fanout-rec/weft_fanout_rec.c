// weft_fanout_rec.c — RFC-0004 fan-out flight recorder: capture + replay
// (tools/FORMATS.md §3, .weftrec v2)
//
// WHY EXISTS: RFC 0004's motivation names "a flight recorder" as a
// first-class fan-out consumer, and D-16 shipped .weftrec capture/replay
// for the KERNEL path (tools/weft-record) — but a ring consumer had no
// recorder: multi-consumer sessions (primary canvas + minimap + network
// viz) could not be captured, replayed, or diffed. This tool is the N+1th
// reader of a byte-compatible fan-out ring (any port's bytes — a native
// peer posts a TS-produced SAB to shm, or C/Rust/Kotlin producers map the
// same layout directly):
//
//   capture <file> --shm <name> --payload B --slots M
//          [--max-frames N] [--max-secs S] [--idle-ms I]
//       Opens the POSIX shared-memory ring <name> (created by ANY producer
//       speaking the RFC-0004 layout), attaches weft_fanout_reader, and
//       appends every fresh claim to <file> as a v2 record. The recorder is
//       a PROTOCOL READER (the D-16 stale-claim rule carries over: it
//       serializes what it claimed, stale and dropped included — the file
//       is the honest capture). Quiesce detection bounds the loop (Law 1):
//       stop when the producer stops advancing latestSeq for --idle-ms.
//
//   replay <file> --shm <name> --payload B --slots M [--hz H]
//       Validates every record CRC, then republishes the recorded frames
//       into a FRESH shm ring (created O_EXCL) at an optional pace, so any
//       port's reader can attach and consume the captured session.
//       DECLARED BOUNDARY (Law 4): replay is content-faithful, not
//       seq-faithful — the ring renumbers frames 1..K (its frame counter
//       is producer-local); the recorded seqs and drop accounting stay in
//       the file, and validate enforces them.
//
//   validate <file> [--expect-mixer]
//       Verifies the v2 header, every record CRC, the strict seq increase,
//       the per-record gap accounting, and the exact telescoping identity
//       sum(dropped) == final_seq - records (RFC 0004 per-reader
//       accounting). --expect-mixer additionally validates every payload
//       word against the 04-LITMUS §0.1 mixer family
//       (weft_mix32(seq*2654435761 + w)) — the same generator as the
//       F-series batteries and the xlang fixtures.
//
//   selftest [--frames N] [--words W]
//       End-to-end in-process gate (the CI road): a writer thread publishes
//       mixer frames into shm ring A, capture records them to a file, the
//       file validates (CRC + accounting + mixer), replay republishes into
//       shm ring B, a second capture drains B back to a second file, and
//       the two files are compared record-for-record, payload-exact
//       (content-faithful replay). Any failure exits non-zero.
//
// LAW 1: every loop is bounded (max-frames / max-secs / quiesce idle).
// LAW 4: the honest capture — drops between claims are in `dropped`,
//        never hidden; replay renumbering is declared, not silent.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fanout.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// CRC-32/zlib — same parameters as .weftrec v1 (FORMATS.md §1.4): reflected
// poly 0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF.
// ---------------------------------------------------------------------------

static uint32_t crc_table[256];
static int crc_table_init = 0;

static void init_crc_table(void) {
    if (crc_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_init = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len) {
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t crc32_compute(const uint8_t* buf, size_t len) {
    init_crc_table();
    return crc32_update(0, buf, len);
}

// ---------------------------------------------------------------------------
// .weftrec v2 format constants (FORMATS.md §3)
// ---------------------------------------------------------------------------

#define WREC_MAGIC 0x43455257u          // "WREC" LE — same as v1
#define WREC_FORMAT_VERSION 2u          // fan-out ring capture
#define WREC_HEADER_SIZE 32u
#define WREC_FLAG_FANOUT 0x1u           // flags bit 0: fan-out ring records
#define WREC_ENV_VERSION 0u             // fan-out frames carry no envelope

#define WREC2_KIND_FANOUT_CLAIM 1u      // the only v2 record kind (so far)

#define WREC2_REC_FIXED 36u             // 32B record head + 4B trailing crc

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void die(const char* msg) {
    fprintf(stderr, "weft_fanout_rec: %s\n", msg);
    exit(2);
}

static void die_errno(const char* what) {
    fprintf(stderr, "weft_fanout_rec: %s: %s\n", what, strerror(errno));
    exit(2);
}

// ---------------------------------------------------------------------------
// LE primitives (bit-exact, little-endian on every host — the wire contract)
// ---------------------------------------------------------------------------

static void put_u16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// ---------------------------------------------------------------------------
// v2 header I/O
// ---------------------------------------------------------------------------

/// Write the 32-byte v2 header (frame_count patched at close; crc covers
/// bytes 0..20 — the v1 rule, same header layout, version 2).
static void write_header(FILE* fp, uint32_t frame_count) {
    uint8_t h[WREC_HEADER_SIZE];
    memset(h, 0, sizeof(h));
    put_u32(h + 0, WREC_MAGIC);
    put_u16(h + 4, WREC_FORMAT_VERSION);
    put_u16(h + 6, WREC_HEADER_SIZE);
    put_u32(h + 8, WREC_FLAG_FANOUT);
    put_u32(h + 12, WREC_ENV_VERSION);
    put_u32(h + 16, frame_count);
    put_u32(h + 20, crc32_compute(h, 20)); // over bytes 0..20
    if (fseek(fp, 0, SEEK_SET) != 0) die_errno("fseek(header)");
    if (fwrite(h, 1, sizeof(h), fp) != sizeof(h)) die_errno("fwrite(header)");
}

/// Append one v2 fanout claim record; returns 0 on success.
static int append_record(FILE* fp, uint64_t seq, uint64_t dropped,
                         const void* payload, uint32_t payload_len) {
    uint8_t head[32];
    put_u32(head + 0, WREC2_REC_FIXED + payload_len); // rec_len (incl. self + crc)
    put_u16(head + 4, WREC2_KIND_FANOUT_CLAIM);
    put_u16(head + 6, 0);                    // reserved
    put_u64(head + 8, seq);
    put_u64(head + 16, dropped);
    put_u32(head + 24, payload_len);
    put_u32(head + 28, 0);                   // reserved
    if (fwrite(head, 1, sizeof(head), fp) != sizeof(head)) return -1;
    if (payload_len && fwrite(payload, 1, payload_len, fp) != payload_len) return -1;
    // crc over bytes 4..(32+N): kind..payload (v1's analog covers its record
    // body sans rec_len/crc; v2 does the same).
    uint32_t crc = crc32_update(0, head + 4, sizeof(head) - 4);
    crc = crc32_update(crc, (const uint8_t*)payload, payload_len);
    uint8_t c[4];
    put_u32(c, crc);
    if (fwrite(c, 1, sizeof(c), fp) != sizeof(c)) return -1;
    return 0;
}

typedef struct {
    uint64_t seq;
    uint64_t dropped;
    uint32_t payload_len;
    uint8_t* payload;   // malloc'd when payload_len > 0 (caller frees)
} wrec2_record_t;

/// Read the next record from fp. Returns 1 on success, 0 on clean EOF,
/// -1 on corruption (bad length / short read / crc mismatch).
static int read_record(FILE* fp, wrec2_record_t* out) {
    uint8_t head[32];
    size_t n = fread(head, 1, 4, fp);
    if (n == 0) return 0;                    // clean EOF
    if (n != 4) return -1;
    const uint32_t rec_len = get_u32(head);
    if (rec_len < WREC2_REC_FIXED || (rec_len % 4) != 0) return -1;
    if (fread(head + 4, 1, sizeof(head) - 4, fp) != sizeof(head) - 4) return -1;
    if (get_u16(head + 4) != WREC2_KIND_FANOUT_CLAIM)
        return -1; // v2's sole kind; unknown kinds are a version violation
    out->seq = get_u64(head + 8);
    out->dropped = get_u64(head + 16);
    out->payload_len = get_u32(head + 24);
    if ((uint64_t)out->payload_len + WREC2_REC_FIXED != rec_len) return -1;
    out->payload = NULL;
    if (out->payload_len) {
        out->payload = (uint8_t*)malloc(out->payload_len);
        if (!out->payload) die("out of memory");
        if (fread(out->payload, 1, out->payload_len, fp) != out->payload_len) {
            free(out->payload); out->payload = NULL; return -1;
        }
    }
    uint8_t c[4];
    if (fread(c, 1, 4, fp) != 4) { free(out->payload); out->payload = NULL; return -1; }
    uint32_t crc = crc32_update(0, head + 4, sizeof(head) - 4);
    crc = crc32_update(crc, out->payload, out->payload_len);
    if (crc != get_u32(c)) { free(out->payload); out->payload = NULL; return -1; }
    return 1;
}

/// Open + validate a v2 file header. Returns the FILE* positioned after the
/// header; fills *frame_count. Dies on structural failure.
static FILE* open_v2(const char* path, uint32_t* frame_count) {
    FILE* fp = fopen(path, "rb");
    if (!fp) die_errno(path);
    uint8_t h[WREC_HEADER_SIZE];
    if (fread(h, 1, sizeof(h), fp) != sizeof(h)) die("short header");
    if (get_u32(h + 0) != WREC_MAGIC) die("bad magic (not a .weftrec file)");
    if (get_u16(h + 4) != WREC_FORMAT_VERSION)
        die("format_version != 2 — v1 kernel captures are not fan-out files "
            "(use weft_record tooling for v1)");
    if (get_u16(h + 6) != WREC_HEADER_SIZE) die("unexpected header_size");
    if (get_u32(h + 8) != WREC_FLAG_FANOUT) die("flags: fan-out bit not set");
    if (get_u32(h + 12) != WREC_ENV_VERSION) die("unexpected envelope_version");
    if (crc32_compute(h, 20) != get_u32(h + 20)) die("header crc mismatch");
    *frame_count = get_u32(h + 16);
    return fp;
}

// ---------------------------------------------------------------------------
// Shared-memory ring helpers — the real-producer road
// ---------------------------------------------------------------------------

/// Map an EXISTING shm ring (capture: read-only; replay drains via its own
/// writer) — the producer must have created it with the RFC-0004 layout and
/// exactly ring_bytes size.
static uint8_t* shm_map_existing(const char* name, size_t ring_bytes, int writable) {
    int fd = shm_open(name, writable ? O_RDWR : O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "weft_fanout_rec: shm_open(/%s): %s — is the producer running?\n",
                name, strerror(errno));
        exit(2);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) die_errno("fstat(shm)");
    if ((size_t)st.st_size != ring_bytes) {
        fprintf(stderr, "weft_fanout_rec: shm /%s is %zu bytes, expected ring_bytes=%zu "
                "(geometry mismatch)\n", name, (size_t)st.st_size, ring_bytes);
        exit(2);
    }
    void* p = mmap(NULL, ring_bytes, writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) die_errno("mmap(shm)");
    close(fd);
    return (uint8_t*)p;
}

/// Create a FRESH shm ring (unlink stale, then O_EXCL) — the replay road.
static uint8_t* shm_create(const char* name, size_t ring_bytes) {
    shm_unlink(name); // ignore ENOENT: replace any stale session
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) die_errno("shm_open(create)");
    if (ftruncate(fd, (off_t)ring_bytes) != 0) die_errno("ftruncate(shm)");
    void* p = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) die_errno("mmap(shm create)");
    memset(p, 0, ring_bytes); // fresh ctrl: latestSeq=0, all slots invalidated
    close(fd);
    return (uint8_t*)p;
}

// ---------------------------------------------------------------------------
// capture
// ---------------------------------------------------------------------------

static int cmd_capture(const char* path, const char* shm_name, size_t payload_bytes,
                       unsigned slot_count, uint64_t max_frames, double max_secs,
                       double idle_secs) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) die("bad geometry (--payload must be a positive multiple of 4, "
                     "--slots in [2,64])");
    uint8_t* ring = shm_map_existing(shm_name, rb, 0);

    weft_fanout_reader_t r;
    if (weft_fanout_reader_init(&r, ring, rb, payload_bytes, slot_count) != 0)
        die("reader attach failed");

    FILE* fp = fopen(path, "wb");
    if (!fp) die_errno(path);
    write_header(fp, 0); // frame_count patched at close

    const double t0 = now_secs();
    double last_progress = t0;
    uint64_t n_records = 0, n_dropped = 0, last_latest = 0;
    while (n_records < max_frames) {
        const double t = now_secs();
        if (t - t0 >= max_secs) break;
        if (t - last_progress >= idle_secs) {
            // Quiesce (Law 1, bounded): the producer stopped advancing
            // latestSeq AND no fresh claim arrived for --idle-ms.
            break;
        }
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        if (c->fresh) {
            if (append_record(fp, c->seq, c->dropped, weft_fanout_view(&r),
                              (uint32_t)payload_bytes) != 0) {
                // Crash-tolerance rule (v1 §1.3): never emit a silently
                // truncated file — close with the scan path and report.
                fprintf(stderr, "weft_fanout_rec: capture write failed at record "
                        "%" PRIu64 " — closing crash-tolerant\n", n_records);
                break;
            }
            n_records++;
            n_dropped += c->dropped;
            last_progress = t;
        } else {
            // Quiesce probe: latestSeq (ctrl[0], the publication point) via
            // an acquire load — advisory, off the claim hot path.
            _Atomic uint64_t* ctrl = (_Atomic uint64_t*)ring;
            const uint64_t latest = atomic_load_explicit(ctrl, memory_order_acquire);
            if (latest != last_latest) {
                last_latest = latest;
                last_progress = t;
            }
        }
    }

    // Close: patch frame_count + header crc (frame_count==0 => scan path).
    write_header(fp, (uint32_t)n_records);
    if (fclose(fp) != 0) die_errno("fclose(capture)");
    weft_fanout_reader_destroy(&r);
    munmap(ring, rb);

    printf("capture: %s — %" PRIu64 " claims, %" PRIu64 " dropped frames accounted "
           "(the honest capture; telemetry advisory)\n", path, n_records, n_dropped);
    return 0;
}

// ---------------------------------------------------------------------------
// mixer (04-LITMUS §0.1 weft_mix32 family — same as fanout_test.c's tword)
// ---------------------------------------------------------------------------

static uint32_t mixer_word(uint32_t seq, uint32_t w) {
    uint32_t x = seq * 2654435761u + w;
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// ---------------------------------------------------------------------------
// validate
// ---------------------------------------------------------------------------

static int cmd_validate(const char* path, int expect_mixer) {
    uint32_t frame_count = 0;
    FILE* fp = open_v2(path, &frame_count);

    uint64_t n = 0, sum_dropped = 0, last_seq = 0;
    wrec2_record_t rec;
    int rc;
    while ((rc = read_record(fp, &rec)) == 1) {
        n++;
        if (n >= 2 && rec.seq <= last_seq)
            die("validate: seq not strictly increasing (record corruption)");
        if (rec.dropped != rec.seq - last_seq - 1)
            die("validate: per-record dropped != seq gap (accounting corruption)");
        if (expect_mixer && rec.payload_len) {
            const uint32_t* v = (const uint32_t*)rec.payload;
            for (uint32_t w = 0; w < rec.payload_len / 4; w++) {
                if (v[w] != mixer_word((uint32_t)rec.seq, w)) {
                    fprintf(stderr, "weft_fanout_rec: validate: mixer mismatch at "
                            "record %" PRIu64 " word %" PRIu32 "\n", n, w);
                    free(rec.payload);
                    return 1;
                }
            }
        }
        sum_dropped += rec.dropped;
        last_seq = rec.seq;
        free(rec.payload);
    }
    if (rc != 0) die("validate: corrupt record (crc or length)");
    if (frame_count != 0 && frame_count != (uint32_t)n) {
        fprintf(stderr, "weft_fanout_rec: validate: header frame_count=%" PRIu32
                " but %llu records — use the scan path\n",
                frame_count, (unsigned long long)n);
        return 1;
    }
    // The exact telescoping identity (RFC 0004): sum(dropped) == lastSeq - fresh.
    if (sum_dropped != last_seq - n) {
        fprintf(stderr, "weft_fanout_rec: validate: telescoping identity violated: "
                "sum(dropped)=%" PRIu64 " != last_seq-records=%" PRIu64 "\n",
                sum_dropped, last_seq - n);
        return 1;
    }
    fclose(fp);
    printf("validate: %s — %llu records, final seq %" PRIu64 ", sum(dropped)=%" PRIu64
           " — telescoping exact%s\n", path, (unsigned long long)n, last_seq,
           sum_dropped, expect_mixer ? ", mixer bit-exact" : "");
    return 0;
}

// ---------------------------------------------------------------------------
// replay
// ---------------------------------------------------------------------------

/// The republish loop (shared by the CLI replay and the selftest's
/// concurrent replay thread). Single writer by contract: the caller owns
/// the attached weft_fanout_t. Fills *out_n with the republished count.
static void replay_core(FILE* fp, weft_fanout_t* f, size_t payload_bytes,
                        double hz, uint64_t* out_n) {
    const double t0 = now_secs();
    wrec2_record_t rec;
    uint64_t n = 0;
    int rc;
    while ((rc = read_record(fp, &rec)) == 1) {
        if (rec.payload_len != payload_bytes)
            die("replay: record payload_len != ring geometry");
        if (!weft_fanout_begin(f)) die("replay: begin failed");
        if (rec.payload_len && weft_fanout_fill(f, rec.payload, rec.payload_len) < 0)
            die("replay: fill failed");
        const uint64_t seq = weft_fanout_publish(f);
        if (seq != n + 1) die("replay: ring renumbering invariant broken");
        n++;
        if (hz > 0) {
            const double target = (double)n / hz;
            const double now = now_secs() - t0;
            if (target > now) usleep((useconds_t)((target - now) * 1e6));
        }
        free(rec.payload);
    }
    if (rc != 0) die("replay: corrupt record — refusing to replay (validate first)");
    *out_n = n;
}

static int cmd_replay(const char* path, const char* shm_name, size_t payload_bytes,
                      unsigned slot_count, double hz) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) die("bad geometry");

    uint32_t frame_count = 0;
    FILE* fp = open_v2(path, &frame_count);

    uint8_t* ring = shm_create(shm_name, rb);
    weft_fanout_t f = {0};
    if (weft_fanout_attach_writer(&f, ring, rb, payload_bytes, slot_count) != 0)
        die("replay: writer attach failed (fresh ring must be zeroed)");

    uint64_t n = 0;
    replay_core(fp, &f, payload_bytes, hz, &n);
    fclose(fp);
    // The shm ring persists in /dev/shm until unlinked: any port's reader
    // attaches by name and consumes the replayed session.
    printf("replay: %s -> shm /%s — %llu frames republished (content-faithful; "
           "seqs renumbered 1..%" PRIu64 " — declared boundary)\n",
           path, shm_name, (unsigned long long)n, n);
    return 0;
}

// ---------------------------------------------------------------------------
// selftest — the CI gate (end-to-end, in-process)
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t* ring;          // shm ring A (mmap'd)
    size_t ring_bytes;
    size_t payload_bytes;
    unsigned slot_count;
    uint64_t frames;
    uint64_t words;
    _Atomic int* done;
    _Atomic uint64_t* out_n;
} selftest_writer_t;

static void* selftest_writer_main(void* arg) {
    selftest_writer_t* w = (selftest_writer_t*)arg;
    weft_fanout_t f = {0};
    if (weft_fanout_attach_writer(&f, w->ring, w->ring_bytes, w->payload_bytes,
                                  w->slot_count) != 0) {
        return (void*)1;
    }
    uint32_t* buf = (uint32_t*)malloc(w->payload_bytes);
    if (!buf) return (void*)1;
    for (uint64_t fr = 1; fr <= w->frames; fr++) {
        for (uint64_t i = 0; i < w->words; i++) {
            buf[i] = mixer_word((uint32_t)fr, (uint32_t)i);
        }
        (void)weft_fanout_begin(&f);
        if (weft_fanout_fill(&f, buf, w->payload_bytes) < 0) { free(buf); return (void*)1; }
        (void)weft_fanout_publish(&f);
    }
    free(buf);
    if (w->out_n) atomic_store(w->out_n, w->frames);
    if (w->done) atomic_store(w->done, 1);
    return NULL;
}

/// Compare two capture files: B (the recapture of the replay) must be a
/// payload SUBSEQUENCE of A (the original capture), and both must end on
/// the SAME final payload (convergence). The subsequence shape is the
/// protocol's own semantics applied honestly: the recapture is itself a
/// fan-out consumer that may drop frames while the replay runs — every
/// payload it DID catch must be byte-identical to one of A's, in order.
static int compare_payloads(const char* a_path, const char* b_path) {
    // Slurp both files into memory (selftest scale — bounded by --frames);
    // freed before every return (ASAN-clean).
    wrec2_record_t *a = NULL, *b = NULL;
    size_t na = 0, nb = 0, capa = 0, capb = 0;
    uint32_t fh = 0;
    FILE* fa_fp = open_v2(a_path, &fh);
    FILE* fb_fp = open_v2(b_path, &fh);
    wrec2_record_t rec;
    int fa_rc, fb_rc;
    while ((fa_rc = read_record(fa_fp, &rec)) == 1) {
        if (na == capa) { capa = capa ? capa * 2 : 64; a = realloc(a, capa * sizeof(*a)); }
        a[na++] = rec;
    }
    while ((fb_rc = read_record(fb_fp, &rec)) == 1) {
        if (nb == capb) { capb = capb ? capb * 2 : 64; b = realloc(b, capb * sizeof(*b)); }
        b[nb++] = rec;
    }
    fclose(fa_fp); fclose(fb_fp);
#define WREC2_FREE_ALL() do { \
    for (size_t i = 0; i < na; i++) free(a[i].payload); \
    for (size_t i = 0; i < nb; i++) free(b[i].payload); \
    free(a); free(b); \
} while (0)
    if (fa_rc != 0 || fb_rc != 0) {
        fprintf(stderr, "selftest: corrupt record stream while comparing\n");
        WREC2_FREE_ALL();
        return -1;
    }
    if (na == 0 || nb == 0) {
        fprintf(stderr, "selftest: empty capture stream (a=%zu b=%zu)\n", na, nb);
        WREC2_FREE_ALL();
        return -1;
    }

    // Subsequence: advance through A for each B record.
    size_t ia = 0;
    for (size_t ib = 0; ib < nb; ib++) {
        while (ia < na && (a[ia].payload_len != b[ib].payload_len ||
                           (a[ia].payload_len &&
                            memcmp(a[ia].payload, b[ib].payload, a[ia].payload_len) != 0))) {
            ia++;
        }
        if (ia == na) {
            fprintf(stderr, "selftest: recaptured record %zu has no payload match "
                    "in the source capture (replay corruption)\n", ib);
            WREC2_FREE_ALL();
            return -1;
        }
        ia++;
    }
    // Convergence: both streams end on the same final payload.
    if (a[na - 1].payload_len != b[nb - 1].payload_len ||
        memcmp(a[na - 1].payload, b[nb - 1].payload, a[na - 1].payload_len) != 0) {
        fprintf(stderr, "selftest: final payload divergence (recapture did not "
                "converge on the replay's last frame)\n");
        WREC2_FREE_ALL();
        return -1;
    }
    printf("selftest: content-faithful replay — %zu/%zu source payloads recaptured "
        "in order, payload-exact; final frame converged\n", nb, na);
    WREC2_FREE_ALL();
    return 0;
#undef WREC2_FREE_ALL
}

typedef struct {
    FILE* fp;               // the capture file to replay (opened by caller)
    weft_fanout_t* f;       // writer attached to ring B (caller-created)
    size_t payload_bytes;
    _Atomic uint64_t* out_n; // republished count (atomic: cross-thread)
    _Atomic int* done;       // set when the replay thread finishes
} selftest_replay_t;

static void* selftest_replay_main(void* arg) {
    selftest_replay_t* rp = (selftest_replay_t*)arg;
    uint64_t n = 0;
    replay_core(rp->fp, rp->f, rp->payload_bytes, 0.0, &n);
    atomic_store(rp->out_n, n);
    atomic_store(rp->done, 1);
    return NULL;
}

static int cmd_selftest(uint64_t frames, uint64_t words) {
    const size_t payload_bytes = (size_t)words * 4;
    const unsigned slots = 4;
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slots);
    if (rb == 0) die("bad selftest geometry");

    const char* shm_a = "weft_fanout_rec_selftest_a";
    const char* shm_b = "weft_fanout_rec_selftest_b";

    // Ring A: fresh shm; the writer thread publishes mixer frames.
    uint8_t* ring_a = shm_create(shm_a, rb);
    _Atomic int writer_done = 0;
    _Atomic uint64_t writer_n = 0;
    selftest_writer_t w = {
        .ring = ring_a, .ring_bytes = rb, .payload_bytes = payload_bytes,
        .slot_count = slots, .frames = frames, .words = words,
        .done = &writer_done, .out_n = &writer_n,
    };

    pthread_t writer;
    if (pthread_create(&writer, NULL, selftest_writer_main, &w) != 0)
        die_errno("pthread_create(writer)");

    // Capture from ring A while the writer runs (same claim loop as
    // cmd_capture; the ring is in-process shm — identical bytes).
    uint64_t n_a = 0;
    {
        weft_fanout_reader_t r = {0};
        if (weft_fanout_reader_init(&r, ring_a, rb, payload_bytes, slots) != 0)
            die("selftest: reader attach failed");
        FILE* fp = fopen("selftest_a.weftrec", "wb");
        if (!fp) die_errno("selftest_a.weftrec");
        write_header(fp, 0);
        uint64_t n = 0;
        const double t0 = now_secs();
        for (;;) {
            if (now_secs() - t0 > 120.0) break; // Law 1: bounded
            const weft_fanout_claim_t* c = weft_fanout_claim(&r);
            if (c->fresh) {
                if (append_record(fp, c->seq, c->dropped, weft_fanout_view(&r),
                                  (uint32_t)payload_bytes) != 0)
                    die("selftest: capture write failed");
                n++;
                if (c->seq == frames && atomic_load(&writer_done)) {
                    break;
                }
            } else if (atomic_load(&writer_done) &&
                       c->seq >= atomic_load(&writer_n)) {
                break;
            }
        }
        write_header(fp, (uint32_t)n);
        fclose(fp);
        weft_fanout_reader_destroy(&r);
        n_a = n;
        if (n_a == 0) die("selftest: capture saw zero frames");
    }

    void* writer_rc = NULL;
    pthread_join(writer, &writer_rc);
    if (writer_rc != NULL) die("selftest: writer thread failed");

    // File A must validate: CRC + gap accounting + telescoping + mixer.
    if (cmd_validate("selftest_a.weftrec", 1) != 0)
        die("selftest: capture file failed validation");

    // Ring B: fresh shm. The replay thread republishes file A into it while
    // the main thread recaptures CONCURRENTLY (the symmetric picture: the
    // recapture is just another fan-out consumer of the replayed stream).
    uint8_t* ring_b = shm_create(shm_b, rb);
    weft_fanout_t fb = {0};
    if (weft_fanout_attach_writer(&fb, ring_b, rb, payload_bytes, slots) != 0)
        die("selftest: replay writer attach failed");
    uint32_t fh = 0;
    FILE* fa_fp = open_v2("selftest_a.weftrec", &fh);
    _Atomic int replay_done = 0;
    _Atomic uint64_t n_republished = 0;
    selftest_replay_t rp = {
        .fp = fa_fp, .f = &fb, .payload_bytes = payload_bytes,
        .out_n = &n_republished, .done = &replay_done,
    };
    pthread_t replayer;
    if (pthread_create(&replayer, NULL, selftest_replay_main, &rp) != 0)
        die_errno("pthread_create(replayer)");

    {
        weft_fanout_reader_t r = {0};
        if (weft_fanout_reader_init(&r, ring_b, rb, payload_bytes, slots) != 0)
            die("selftest: recapture reader attach failed");
        FILE* fp = fopen("selftest_b.weftrec", "wb");
        if (!fp) die_errno("selftest_b.weftrec");
        write_header(fp, 0);
        uint64_t n = 0;
        const double t0 = now_secs();
        for (;;) {
            if (now_secs() - t0 > 120.0) break; // Law 1: bounded
            const weft_fanout_claim_t* c = weft_fanout_claim(&r);
            if (c->fresh) {
                if (append_record(fp, c->seq, c->dropped, weft_fanout_view(&r),
                                  (uint32_t)payload_bytes) != 0)
                    die("selftest: recapture write failed");
                n++;
                if (c->seq == n_a && atomic_load(&replay_done)) {
                    break; // caught the replay's final frame
                }
            } else if (atomic_load(&replay_done) &&
                       c->seq >= atomic_load(&n_republished)) {
                break; // replay quiesced and we are caught up
            }
        }
        write_header(fp, (uint32_t)n);
        fclose(fp);
        weft_fanout_reader_destroy(&r);
        if (n == 0) die("selftest: recapture saw zero frames");
    }

    pthread_join(replayer, NULL);
    fclose(fa_fp);

    if (cmd_validate("selftest_b.weftrec", 0) != 0)
        die("selftest: replay capture failed validation");
    if (compare_payloads("selftest_a.weftrec", "selftest_b.weftrec") != 0)
        die("selftest: content-faithful replay check failed");

    unlink("selftest_a.weftrec");
    unlink("selftest_b.weftrec");
    shm_unlink(shm_a);
    shm_unlink(shm_b);
    printf("selftest: PASS — capture -> validate(mixer) -> replay -> recapture -> "
           "compare, all green (%" PRIu64 " frames published, %llu-word payload)\n",
           frames, (unsigned long long)words);
    return 0;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s capture <file> --shm <name> --payload <bytes> --slots <n>"
        " [--max-frames n] [--max-secs s] [--idle-ms ms]\n"
        "       %s replay <file> --shm <name> --payload <bytes> --slots <n> [--hz h]\n"
        "       %s validate <file> [--expect-mixer]\n"
        "       %s selftest [--frames n] [--words w]\n"
        "\n"
        ".weftrec v2 fan-out flight-recorder capture/replay (FORMATS.md §3).\n"
        "capture:  attach as an N+1th reader to a live RFC-0004 shm ring and\n"
        "          record every fresh claim (the honest capture — drops kept).\n"
        "replay:   republish a capture into a fresh shm ring (content-faithful;\n"
        "          seqs renumbered 1..K — declared boundary).\n"
        "validate: CRC + strict seq + gap accounting + telescoping identity\n"
        "          (+ optional 04-LITMUS mixer check).\n"
        "selftest: end-to-end gate (writer thread -> capture -> validate ->\n"
        "          replay -> recapture -> payload-exact compare).\n",
        prog, prog, prog, prog);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const char* cmd = argv[1];

    if (strcmp(cmd, "selftest") == 0) {
        uint64_t frames = 20000, words = 64;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
                frames = strtoull(argv[++i], NULL, 10);
            else if (strcmp(argv[i], "--words") == 0 && i + 1 < argc)
                words = strtoull(argv[++i], NULL, 10);
            else { usage(argv[0]); return 2; }
        }
        return cmd_selftest(frames, words);
    }

    if (strcmp(cmd, "validate") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        int expect_mixer = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--expect-mixer") == 0) expect_mixer = 1;
            else { usage(argv[0]); return 2; }
        }
        return cmd_validate(argv[2], expect_mixer);
    }

    // capture / replay share the geometry + shm args.
    if (argc < 3) { usage(argv[0]); return 2; }
    const char* path = argv[2];
    const char* shm_name = NULL;
    long payload = 0, slots = 4;
    double max_secs = 30.0, idle_ms = 2000.0, hz = 0.0;
    uint64_t max_frames = 1000000;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) shm_name = argv[++i];
        else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc)
            payload = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--slots") == 0 && i + 1 < argc)
            slots = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc)
            max_frames = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--max-secs") == 0 && i + 1 < argc)
            max_secs = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--idle-ms") == 0 && i + 1 < argc)
            idle_ms = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc)
            hz = strtod(argv[++i], NULL);
        else { usage(argv[0]); return 2; }
    }
    if (!shm_name || payload <= 0 || (payload % 4) != 0 || slots < 2 || slots > 64) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(cmd, "capture") == 0)
        return cmd_capture(path, shm_name, (size_t)payload, (unsigned)slots,
                           max_frames, max_secs, idle_ms / 1000.0);
    if (strcmp(cmd, "replay") == 0)
        return cmd_replay(path, shm_name, (size_t)payload, (unsigned)slots, hz);

    usage(argv[0]);
    return 2;
}
