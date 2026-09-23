// replay_runner.c — RFC 0019 time-travel runner: xlang hash-log emitter +
// checkpoint jump debugger.
//
// Modes:
//   replay-runner [--fold] [STEPS [SEED]]        hash-log mode (G5 style):
//       folds the deterministic scenario (the RFC-0019 fixture grammar,
//       xorshift32-seeded) and prints the per-step u64 state hashes as one
//       lowercase-hex line — byte-compared across C/Rust/TS/Kotlin/Swift/
//       Dart by fixtures/xlang-replay/run.sh.
//   replay-runner --file capture.weftrec [--jump K]
//       post-mortem mode: validates a real capture, folds it, prints the
//       summary; --jump K re-folds with 64-step checkpoints and prints the
//       reconstructed shadow state at step K (the time-travel view).

#define _GNU_SOURCE
#include "weft_replay.h"
#include "trace_rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t xs32(uint32_t x) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

// The scenario emitter mirrors the fold (same shadow rules) so CLAIM events
// carry the seq the model will reconstruct — identical grammar in every
// port's emitter (RFC-0019 fixture).
typedef struct {
    uint32_t latest, w_work, r_work, epoch;
    int revoked;
    uint32_t seq;
    uint32_t bufseq[3];
} scen_t;

static void scen_init(scen_t* c) {
    memset(c, 0, sizeof(*c));
    c->latest = 0; c->w_work = 1; c->r_work = 2;
}

static weft_trace_event scen_next(scen_t* c, uint32_t* state) {
    weft_trace_event e = {0, 0, 0};
    *state = xs32(*state);
    uint32_t op = *state & 15u;
    if (op < 7) {
        c->seq += 1;
        uint32_t len = (*state >> 4) % 1024u;
        if (!c->revoked) {
            e.kind = WEFT_TRACE_PUBLISH; e.aux = (uint16_t)len; e.data = c->seq;
            c->bufseq[c->w_work] = c->seq;
            uint32_t old = c->latest;
            c->latest = c->w_work; c->w_work = old;
        } else {
            c->epoch += 1;
            e.kind = WEFT_TRACE_DROP; e.aux = (uint16_t)c->epoch; e.data = c->seq;
        }
    } else if (op < 12) {
        e.kind = WEFT_TRACE_CLAIM; e.data = c->bufseq[c->latest];
        uint32_t mine = c->latest;
        c->latest = c->r_work; c->r_work = mine;
    } else if (op == 12) {
        if (!c->revoked) { e.kind = WEFT_TRACE_REVOKE; e.data = c->epoch; c->revoked = 1; }
        else { e.kind = WEFT_TRACE_ACK; e.data = c->epoch; }
    } else if (op == 13) {
        if (c->revoked) { e.kind = WEFT_TRACE_ACK; e.data = c->epoch; c->revoked = 0; }
        else { e.kind = WEFT_TRACE_STALL; e.data = (*state >> 4) % 8u; }
    } else if (op == 14) {
        e.kind = WEFT_TRACE_TEAR; e.data = c->seq;
    } else {
        e.kind = WEFT_TRACE_CANARY_FAIL; e.data = c->seq;
    }
    return e;
}

static int mode_fold(long steps, uint32_t seed) {
    scen_t c;
    scen_init(&c);
    uint32_t state = seed;
    weft_replay_state_t s;
    weft_replay_init(&s);
    for (long i = 0; i < steps; i++) {
        weft_trace_event e = scen_next(&c, &state);
        if (weft_replay_step(&s, &e) != WEFT_REPLAY_OK) {
            fprintf(stderr, "replay-runner: fold disagreement at step %ld\n", i);
            return 1;
        }
        printf("%016llx", (unsigned long long)s.hash);
    }
    printf("\n");
    return 0;
}

static int mode_file(const char* path, long jump) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed\n");
        free(buf);
        fclose(f);
        return 1;
    }
    fclose(f);
    uint32_t count = 0;
    if (weft_trace_validate(buf, (size_t)len, &count) != 0) {
        fprintf(stderr, "replay-runner: %s is not a valid .weftrec v4\n", path);
        free(buf);
        return 1;
    }
    weft_replay_state_t s;
    weft_replay_init(&s);
    size_t cur = 32;
    weft_trace_event e;
    long step = 0;
    while (cur < (size_t)len && weft_trace_next(buf, (size_t)len, &cur, &e) == 1) {
        weft_replay_result_t rc = weft_replay_step(&s, &e);
        if (rc != WEFT_REPLAY_OK) {
            fprintf(stderr, "replay-runner: fold %s at step %ld (trace/model "
                            "disagreement — inspect the capture around this "
                            "decision)\n",
                    rc == WEFT_REPLAY_DISAGREE ? "DISAGREED" : "hit an unknown kind",
                    step);
            free(buf);
            return 1;
        }
        if (jump >= 0 && step == jump) {
            // the time-travel view: full shadow state at step K
            printf("step %ld: latest=%u epoch=%u w_work=%u r_work=%u "
                   "revoked=%u buf0=(%u,%u,v%u) buf1=(%u,%u,v%u) buf2=(%u,%u,v%u) "
                   "t_pub=%llu t_claim=%llu t_drop=%llu hash=%016llx\n",
                   step, s.latest, s.epoch, s.w_work, s.r_work, s.revoked,
                   s.buf[0].seq, s.buf[0].len, s.buf[0].ver,
                   s.buf[1].seq, s.buf[1].len, s.buf[1].ver,
                   s.buf[2].seq, s.buf[2].len, s.buf[2].ver,
                   (unsigned long long)s.t_publish,
                   (unsigned long long)s.t_claim,
                   (unsigned long long)s.t_drop,
                   (unsigned long long)s.hash);
        }
        step++;
    }
    free(buf);
    if (jump < 0) {
        printf("folded %ld events: latest=%u epoch=%u publish=%llu claim=%llu "
               "drop=%llu stall=%u tear=%u canary=%u final_hash=%016llx\n",
               step, s.latest, s.epoch,
               (unsigned long long)s.t_publish, (unsigned long long)s.t_claim,
               (unsigned long long)s.t_drop, s.t_stall, s.t_tear, s.t_canary,
               (unsigned long long)s.hash);
    }
    return 0;
}

int main(int argc, char** argv) {
    long jump = -1;
    const char* file = NULL;
    long steps = 10000;
    uint32_t seed = 0x00C0FFEE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) file = argv[++i];
        else if (strcmp(argv[i], "--jump") == 0 && i + 1 < argc) jump = atol(argv[++i]);
        else if (strcmp(argv[i], "--fold") == 0) { /* default mode */ }
        else if (i + 1 <= argc && file == NULL && jump < 0) {
            long v = atol(argv[i]);
            if (i == 1 || steps == 10000) {
                if (v > 0) steps = v;
                else seed = (uint32_t)strtoul(argv[i], NULL, 0);
            } else {
                seed = (uint32_t)strtoul(argv[i], NULL, 0);
            }
        }
    }
    if (file) return mode_file(file, jump);
    return mode_fold(steps, seed);
}
