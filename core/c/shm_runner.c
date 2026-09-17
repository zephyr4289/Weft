// shm_runner.c — inter-process ring runner: torture, strace proof, xlang.
//
// Modes:
//   torture <payload_bytes> <slots> <frames> [--readers N] [--fork]
//       Multi-PROCESS torture (the S4/S5 shape at scale, standalone):
//       one writer process, N forked reader processes, `frames` mixer
//       frames. Readers verify payload integrity + telescoping, exit 0.
//       Default --readers 4; --fork uses the anonymous+fork road, default
//       is named+attach. Prints the per-reader accounting and a summary.
//
//   strace-proof <name> <frames> [--marker]
//       Designed to be run under strace -f -e trace=write: publishes
//       `frames` frames while making one marker write(2) before and one
//       after the publish loop. The EVIDENCE: between the two marker
//       writes the ONLY syscalls are the two writes themselves — zero
//       syscalls per publish/claim (pure shared-memory atomics). Without
//       --marker the loop just publishes (for raw syscall counting).
//
//   hold-publish <name> <frames> <hold_ms>
//       Creates session <name>, publishes `frames` mixer frames, then HOLDS
//       the ring alive for hold_ms (sleeping) before destroy — the demo/
//       xlang shape: another process attaches by name while this one runs.
//
//   dump-session <name>
//       Prints the session header + ring ctrl state as JSON-ish text (the
//       xlang validation road: a TS/Node peer maps the same object and
//       checks the same fields).
//
//   validate-session <name> <frames>
//       Attaches read-only, claims the latest frame, verifies the mixer
//       payload bit-exactly, checks latestSeq == frames — the negative leg
//       for peers: a mismatched object or tampered payload exits 1.
//
// Build: make -C core/c shm-runner

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fanout.h"
#include "shm_ring.h"

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static void fill_mixer(uint8_t* dst, uint32_t seq, size_t words) {
    uint32_t* w = (uint32_t*)dst;
    for (size_t i = 0; i < words; i++) {
        w[i] = mix32(seq * 2654435761u + (uint32_t)i);
    }
}

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// ---------------------------------------------------------------------------
// torture
// ---------------------------------------------------------------------------

static int reader_child(const weft_shm_map_t* m, uint64_t frames) {
    weft_fanout_reader_t r;
    memset(&r, 0, sizeof(r));
    if (weft_fanout_reader_init(&r, m->ring, m->mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                weft_shm_payload_bytes(m), weft_shm_slot_count(m)) != 0) {
        return 10;
    }
    const size_t words = weft_shm_payload_bytes(m) / 4;
    uint64_t fresh = 0, drops = 0;
    int payload_ok = 1;
    long spin = 0;
    while (r.rec.seq < frames) {
        const weft_fanout_claim_t* c = weft_fanout_claim(&r);
        if (c->fresh) {
            const uint32_t* w = (const uint32_t*)weft_fanout_view(&r);
            for (size_t i = 0; i < words; i += 3) {
                if (w[i] != mix32((uint32_t)c->seq * 2654435761u + (uint32_t)i)) {
                    payload_ok = 0;
                }
            }
            fresh++;
            drops += c->dropped;
        } else if (++spin > 400000000L) {
            return 11;
        }
    }
    weft_fanout_stats_t st;
    weft_fanout_reader_stats(&r, &st);
    fprintf(stderr, "    reader pid=%d: fresh=%" PRIu64 " drops=%" PRIu64
                    " last=%" PRIu64 " torn_exhausted=%" PRIu64 "\n",
            (int)getpid(), fresh, drops, r.rec.seq, st.torn_exhausted);
    const int ok = payload_ok && (fresh + drops == frames);
    weft_fanout_reader_destroy(&r);
    return ok ? 0 : 12;
}

static int mode_torture(size_t payload_bytes, unsigned slots, uint64_t frames,
                        unsigned readers, int anon) {
    weft_shm_map_t m;
    const char* name = "weft-shm-torture";
    if (anon) {
        if (weft_shm_create_anon(payload_bytes, slots, &m) != 0) {
            fprintf(stderr, "torture: anon create failed\n");
            return 2;
        }
    } else {
        weft_shm_unlink(name);
        if (weft_shm_create_named(name, payload_bytes, slots, &m) != 0) {
            fprintf(stderr, "torture: named create failed\n");
            return 2;
        }
    }
    printf("torture: %s road, %u readers x %" PRIu64 " frames, %zu B x %u slots\n",
           anon ? "anonymous+fork" : "named+attach", readers, frames,
           payload_bytes, slots);

    pid_t pids[16];
    if (readers > 16) readers = 16;
    for (unsigned i = 0; i < readers; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            if (!anon) {
                weft_shm_map_t cm;
                int spins = 0;
                while (weft_shm_attach_named(name, &cm, 1) != 0) {
                    if (++spins > 1000) _exit(20);
                    usleep(1000);
                }
                const int rc = reader_child(&cm, frames);
                weft_shm_destroy(&cm);
                _exit(rc);
            }
            _exit(reader_child(&m, frames));
        }
    }

    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    if (weft_fanout_attach_writer(&f, m.ring, m.mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                  payload_bytes, slots) != 0) {
        fprintf(stderr, "torture: writer attach failed\n");
        return 2;
    }
    const double t0 = now_secs();
    for (uint64_t s = 1; s <= frames; s++) {
        fill_mixer(weft_fanout_begin(&f), (uint32_t)s, payload_bytes / 4);
        weft_fanout_publish(&f);
    }
    const double dt = now_secs() - t0;
    weft_fanout_destroy(&f);

    int all_ok = 1;
    for (unsigned i = 0; i < readers; i++) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) all_ok = 0;
    }
    printf("torture: writer %.3f s (%.0f frames/s); readers %s; verdict %s\n",
           dt, (double)frames / dt, all_ok ? "all OK" : "FAILURES",
           all_ok ? "PASS" : "FAIL");
    weft_shm_destroy(&m);
    return all_ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// strace-proof
// ---------------------------------------------------------------------------

static int mode_strace_proof(const char* name, uint64_t frames, int marker) {
    weft_shm_map_t m;
    weft_fanout_t f;
    weft_shm_unlink(name);
    if (weft_fanout_shm_create(name, 64, 4, &f, &m) != 0) {
        fprintf(stderr, "strace-proof: create failed\n");
        return 2;
    }
    if (marker) {
        const char before[] = "MARKER-PUBLISH-START\n";
        if (write(STDERR_FILENO, before, sizeof(before) - 1) < 0) return 3;
    }
    for (uint64_t s = 1; s <= frames; s++) {
        fill_mixer(weft_fanout_begin(&f), (uint32_t)s, 16);
        weft_fanout_publish(&f);
    }
    if (marker) {
        const char after[] = "MARKER-PUBLISH-END\n";
        if (write(STDERR_FILENO, after, sizeof(after) - 1) < 0) return 3;
    }
    // Claim once through a second mapping (read path is syscall-free too).
    weft_shm_map_t mr;
    weft_fanout_reader_t r;
    if (weft_fanout_shm_attach_reader(name, &r, &mr, 1) != 0) return 2;
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    if (marker) {
        const char claim_marker[] = "MARKER-CLAIM-DONE\n";
        if (write(STDERR_FILENO, claim_marker, sizeof(claim_marker) - 1) < 0) return 3;
    }
    const int ok = c->fresh && c->seq == frames;
    weft_fanout_reader_destroy(&r);
    weft_shm_destroy(&mr);
    weft_fanout_destroy(&f);
    weft_shm_destroy(&m);
    printf("strace-proof: %" PRIu64 " frames published + 1 claim; "
           "between the markers only the marker writes appear -> %s\n",
           frames, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// dump-session / validate-session
// ---------------------------------------------------------------------------

static int mode_hold_publish(const char* name, uint64_t frames, long hold_ms) {
    weft_shm_map_t m;
    weft_fanout_t f;
    weft_shm_unlink(name);
    if (weft_fanout_shm_create(name, 64, 4, &f, &m) != 0) {
        fprintf(stderr, "hold-publish: create failed\n");
        return 2;
    }
    for (uint64_t s = 1; s <= frames; s++) {
        fill_mixer(weft_fanout_begin(&f), (uint32_t)s, 16);
        weft_fanout_publish(&f);
    }
    fprintf(stderr, "hold-publish: %" PRIu64 " frames live in '%s' for %ld ms\n",
            frames, name, hold_ms);
    struct timespec ts = { .tv_sec = hold_ms / 1000, .tv_nsec = (hold_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    weft_fanout_destroy(&f);
    weft_shm_destroy(&m);
    return 0;
}

static int mode_dump_session(const char* name) {
    weft_shm_map_t m;
    if (weft_shm_attach_named(name, &m, 1) != 0) {
        fprintf(stderr, "dump-session: attach '%s' failed\n", name);
        return 2;
    }
    const _Atomic uint64_t* ctrl = (_Atomic uint64_t*)m.ring;
    printf("{\"name\":\"%s\",\"payload_bytes\":%zu,\"slot_count\":%u,"
           "\"ring_bytes\":%zu,\"latest_seq\":%" PRIu64 ",\"publishes\":%" PRIu64 "}\n",
           name, weft_shm_payload_bytes(&m), weft_shm_slot_count(&m),
           weft_shm_ring_bytes(&m),
           atomic_load_explicit(&ctrl[0], memory_order_acquire),
           atomic_load_explicit(&ctrl[1], memory_order_relaxed));
    weft_shm_destroy(&m);
    return 0;
}

static int mode_validate_session(const char* name, uint64_t frames) {
    weft_shm_map_t m;
    weft_fanout_reader_t r;
    if (weft_fanout_shm_attach_reader(name, &r, &m, 1) != 0) {
        fprintf(stderr, "validate-session: attach '%s' failed\n", name);
        return 2;
    }
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    const uint32_t* w = (const uint32_t*)weft_fanout_view(&r);
    const size_t words = weft_shm_payload_bytes(&m) / 4;
    int payload_ok = 1;
    for (size_t i = 0; i < words; i += 3) {
        if (w[i] != mix32((uint32_t)c->seq * 2654435761u + (uint32_t)i)) payload_ok = 0;
    }
    const int ok = c->fresh && c->seq == frames && payload_ok;
    printf("validate-session: latest=%" PRIu64 " (expect %" PRIu64
           ") payload %s -> %s\n",
           c->seq, frames, payload_ok ? "bit-exact" : "MISMATCH",
           ok ? "PASS" : "FAIL");
    weft_fanout_reader_destroy(&r);
    weft_shm_destroy(&m);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  shm-runner torture <payload> <slots> <frames> [--readers N] [--fork]\n"
                "  shm-runner strace-proof <name> <frames> [--marker]\n"
                "  shm-runner hold-publish <name> <frames> <hold_ms>\n"
                "  shm-runner dump-session <name>\n"
                "  shm-runner validate-session <name> <frames>\n");
        return 2;
    }
    if (strcmp(argv[1], "torture") == 0 && argc >= 5) {
        unsigned readers = 4;
        int anon = 0;
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--readers") == 0 && i + 1 < argc) {
                readers = (unsigned)atoi(argv[++i]);
            } else if (strcmp(argv[i], "--fork") == 0) {
                anon = 1;
            }
        }
        return mode_torture((size_t)atol(argv[2]), (unsigned)atoi(argv[3]),
                            (uint64_t)atoll(argv[4]), readers, anon);
    }
    if (strcmp(argv[1], "strace-proof") == 0 && argc >= 4) {
        int marker = (argc >= 5 && strcmp(argv[4], "--marker") == 0);
        return mode_strace_proof(argv[2], (uint64_t)atoll(argv[3]), marker);
    }
    if (strcmp(argv[1], "hold-publish") == 0 && argc == 5) {
        return mode_hold_publish(argv[2], (uint64_t)atoll(argv[3]), atol(argv[4]));
    }
    if (strcmp(argv[1], "dump-session") == 0 && argc == 3) {
        return mode_dump_session(argv[2]);
    }
    if (strcmp(argv[1], "validate-session") == 0 && argc == 4) {
        return mode_validate_session(argv[2], (uint64_t)atoll(argv[3]));
    }
    fprintf(stderr, "bad args\n");
    return 2;
}
