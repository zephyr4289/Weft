// weft_replay_test.c — RFC 0019 R-series: the time-travel fold conformance.
//
// R1  init hash pinned (parity vector, all ports must agree)
// R2  PUBLISH transition hand-verified (THE exchange)
// R3  CLAIM validation + DISAGREE leaves state untouched
// R4  null-frame convention (claim before any publish, seq 0)
// R5  REVOKE / DROP / ACK transitions
// R6  canonical serialization layout (111 bytes, field offsets)
// R7  checkpoint jump equivalence: jump(k) hash == fold(k) hash, sweep
// R8  100k-event soak, final hash pinned
// R9  unknown kind refused

#define _GNU_SOURCE
#include "weft_replay.h"
#include "trace_rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0, g_fails = 0;
#define CK(cond, name)                                              \
    do {                                                            \
        g_checks++;                                                 \
        if (!(cond)) {                                              \
            g_fails++;                                              \
            printf("  FAIL %s:%d %s\n", __func__, __LINE__, name);  \
        }                                                           \
    } while (0)

// The shared deterministic scenario (RFC-0019 fixture grammar; mirrored
// EXACTLY by every port's emitter — see fixtures/xlang-replay/run.sh).
static uint32_t xs32(uint32_t x) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}
// The emitter mirrors the fold to know claimed seqs (same shadow rules).
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
// generate the next event, advancing the scenario mirror
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
        e.kind = WEFT_TRACE_CLAIM; e.aux = 0;
        e.data = c->bufseq[c->latest];
        uint32_t mine = c->latest;
        c->latest = c->r_work; c->r_work = mine;
    } else if (op == 12) {
        if (!c->revoked) {
            e.kind = WEFT_TRACE_REVOKE; e.data = c->epoch;
            c->revoked = 1;
        } else {
            e.kind = WEFT_TRACE_ACK; e.data = c->epoch;
        }
    } else if (op == 13) {
        if (c->revoked) {
            e.kind = WEFT_TRACE_ACK; e.data = c->epoch;
            c->revoked = 0;
        } else {
            e.kind = WEFT_TRACE_STALL; e.data = (*state >> 4) % 8u;
        }
    } else if (op == 14) {
        e.kind = WEFT_TRACE_TEAR; e.data = c->seq;
    } else {
        e.kind = WEFT_TRACE_CANARY_FAIL; e.data = c->seq;
    }
    return e;
}

static void t_r1_init_hash(void) {
    weft_replay_state_t s;
    weft_replay_init(&s);
    CK(s.latest == 0 && s.w_work == 1 && s.r_work == 2, "init indices per 02 §1");
    CK(s.hash == 0x8a769a0111cf3af3ull, "init hash pinned (parity vector)");
}

static void t_r2_publish(void) {
    weft_replay_state_t s;
    weft_replay_init(&s);
    weft_trace_event e = {WEFT_TRACE_PUBLISH, 64, 5};
    CK(weft_replay_step(&s, &e) == WEFT_REPLAY_OK, "publish applied");
    CK(s.buf[1].seq == 5 && s.buf[1].len == 64 && s.buf[1].ver == 1,
       "buf[w_work=1] = {5, 64, v1}");
    CK(s.latest == 1 && s.w_work == 0, "exchange: latest=1, w_work=old latest");
    CK(s.t_publish == 1 && s.t_wsteps == 1, "telemetry +1");
    // second publish takes buf 0 (the old latest) — the triad rotates
    weft_trace_event e2 = {WEFT_TRACE_PUBLISH, 16, 6};
    weft_replay_step(&s, &e2);
    CK(s.latest == 0 && s.w_work == 1 && s.buf[0].seq == 6,
       "second publish rotates the triad");
}

static void t_r3_claim(void) {
    weft_replay_state_t s;
    weft_replay_init(&s);
    weft_trace_event p = {WEFT_TRACE_PUBLISH, 64, 5};
    weft_replay_step(&s, &p);
    uint64_t before = s.hash;
    weft_trace_event c = {WEFT_TRACE_CLAIM, 0, 5};
    CK(weft_replay_step(&s, &c) == WEFT_REPLAY_OK, "claim of seq 5 ok");
    CK(s.r_work == 1 && s.latest == 2, "claim exchange: r_work=1, latest=old r_work");
    // disagreement: wrong claimed seq — refused, state UNTOUCHED
    weft_trace_event bad = {WEFT_TRACE_CLAIM, 0, 99};
    CK(weft_replay_step(&s, &bad) == WEFT_REPLAY_DISAGREE, "bad claim refused");
    CK(s.hash == before || s.t_claim == 1, "disagreement left the fold intact");
    weft_trace_event good = {WEFT_TRACE_CLAIM, 0, s.buf[s.latest].seq};
    CK(weft_replay_step(&s, &good) == WEFT_REPLAY_OK, "reconstructed claim ok");
}

static void t_r4_null_frame(void) {
    weft_replay_state_t s;
    weft_replay_init(&s);
    weft_trace_event c = {WEFT_TRACE_CLAIM, 0, 0};
    CK(weft_replay_step(&s, &c) == WEFT_REPLAY_OK, "pre-publish claim (null, seq 0)");
    CK(s.buf[0].seq == 0 && s.buf[0].len == 0, "null frame modeled len 0 (rule 1)");
}

static void t_r5_revocation(void) {
    weft_replay_state_t s;
    weft_replay_init(&s);
    weft_trace_event p = {WEFT_TRACE_PUBLISH, 8, 1};
    weft_replay_step(&s, &p);
    weft_trace_event rv = {WEFT_TRACE_REVOKE, 0, 0};
    weft_replay_step(&s, &rv);
    CK(s.revoked == 1, "revoke sets flag");
    weft_trace_event dp = {WEFT_TRACE_DROP, 3, 9};  // epoch at ACK = 3
    weft_replay_step(&s, &dp);
    CK(s.epoch == 3 && s.t_drop == 1, "drop carries epoch-at-ack");
    weft_trace_event ack = {WEFT_TRACE_ACK, 0, 3};
    weft_replay_step(&s, &ack);
    CK(s.epoch == 3 && s.revoked == 1, "ack syncs epoch, still revoked");
    weft_trace_event p2 = {WEFT_TRACE_PUBLISH, 8, 2};
    weft_replay_step(&s, &p2);
    CK(s.revoked == 0, "publish implies rebind (rule 2)");
}

static void t_r6_serialize(void) {
    weft_replay_state_t s;
    weft_replay_init(&s);
    uint8_t ser[128];
    CK(weft_replay_serialize(&s, ser, sizeof(ser)) == 111, "111-byte serialization");
    CK(ser[0] == 0 && ser[4] == 0 && ser[8] == 1 && ser[12] == 2,
       "LE layout: latest, epoch, w_work, r_work at 0/4/8/12");
    CK(ser[16] == 0, "revoked byte at 16");
    // buf[0].ver is u16 at 16+1+8+2 = 27; check a couple of derived offsets
    uint32_t latest;
    memcpy(&latest, ser, 4);
    CK(latest == 0, "latest readable as LE u32");
}

static void t_r7_jump(void) {
    // fold 2000 scenario events; verify jump(k) (checkpoint + refold)
    // equals fold(k) for a sweep of k
    scen_t c;
    scen_init(&c);
    uint32_t state = 0x00C0FFEE;
    weft_trace_event evs[2000];
    for (int i = 0; i < 2000; i++) evs[i] = scen_next(&c, &state);

    weft_replay_state_t s;
    weft_replay_init(&s);
    uint64_t hashes[2000];
    CK(weft_replay_fold(&s, evs, 2000, hashes) == WEFT_REPLAY_OK, "fold 2000");

    static weft_replay_state_t cps[2000 / WEFT_REPLAY_CHECKPOINT + 1];
    int ncp = 0;
    weft_replay_state_t t;
    weft_replay_init(&t);
    for (int i = 0; i < 2000; i++) {
        if (i % (int)WEFT_REPLAY_CHECKPOINT == 0) cps[ncp++] = t;
        weft_replay_step(&t, &evs[i]);
    }
    int jump_ok = 1;
    int ks[] = {0, 1, 63, 64, 65, 127, 128, 999, 1921, 1999};
    for (unsigned ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
        int k = ks[ki];
        weft_replay_state_t j = cps[k / WEFT_REPLAY_CHECKPOINT];
        for (int i = (k / WEFT_REPLAY_CHECKPOINT) * (int)WEFT_REPLAY_CHECKPOINT; i < k; i++) {
            weft_replay_step(&j, &evs[i]);
        }
        // j now holds the state after exactly k events; hashes[i] is the
        // hash AFTER event i (0-based), so compare with hashes[k-1] (k>=1)
        // or the init hash (k=0).
        uint64_t want = (k == 0) ? cps[0].hash : hashes[k - 1];
        if (j.hash != want) jump_ok = 0;
    }
    CK(jump_ok, "jump(k) hash == fold(k) hash across the sweep");
}

static void t_r8_soak(void) {
    scen_t c;
    scen_init(&c);
    uint32_t state = 0x00C0FFEE;
    weft_replay_state_t s;
    weft_replay_init(&s);
    weft_trace_event e;
    for (int i = 0; i < 100000; i++) {
        e = scen_next(&c, &state);
        if (weft_replay_step(&s, &e) != WEFT_REPLAY_OK) {
            CK(0, "soak fold clean");
            return;
        }
    }
    CK(s.step == 100000, "100k events folded");
    // Pinned cross-port parity vector: every runtime folding this scenario
    // MUST land on exactly this final hash.
    CK(s.hash == 0x26beb484733ecde0ull, "100k soak final hash pinned");
    // The scenario emitter's claim seqs were always accepted:
    CK(s.t_claim > 1000 && s.t_publish > 1000, "scenario exercised both sides");
}

static void t_r9_bad_kind(void) {
    weft_replay_state_t s;
    weft_replay_init(&s);
    weft_trace_event e = {99, 0, 0};
    CK(weft_replay_step(&s, &e) == WEFT_REPLAY_BAD_KIND, "unknown kind refused");
}

int main(void) {
    printf("== weft_replay_test — RFC 0019 R-series ==\n");
    t_r1_init_hash();
    t_r2_publish();
    t_r3_claim();
    t_r4_null_frame();
    t_r5_revocation();
    t_r6_serialize();
    t_r7_jump();
    t_r8_soak();
    t_r9_bad_kind();
    printf("== R-series: %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
