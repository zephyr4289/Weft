// weft_replay.c — RFC 0019: deterministic time-travel replay fold, C reference.
//
// The transition table and the canonical byte serialization are NORMATIVE
// (RFC-0019); every port mirrors them exactly. The kernel (weft.c/weft.h)
// is untouched — Law 3. Zero allocation — Law 2. No loops beyond the event
// stream and the 111-byte serialization — Law 1.

#include "weft_replay.h"

#include <string.h>

// ---------------------------------------------------------------------------
// FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3)
// ---------------------------------------------------------------------------

uint64_t weft_replay_fnv1a(const uint8_t* data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Serialization (canonical LE byte layout — RFC-0019, normative)
// ---------------------------------------------------------------------------

static inline void put_u32(uint8_t** p, uint32_t v) {
    (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16); (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}
static inline void put_u16(uint8_t** p, uint16_t v) {
    (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}
static inline void put_u64(uint8_t** p, uint64_t v) {
    for (int i = 0; i < 8; i++) (*p)[i] = (uint8_t)(v >> (8 * i));
    *p += 8;
}

size_t weft_replay_serialize(const weft_replay_state_t* s, uint8_t* dst,
                             size_t cap) {
    if (dst == NULL || cap < 111) return 0;
    uint8_t* p = dst;
    put_u32(&p, s->latest);
    put_u32(&p, s->epoch);
    put_u32(&p, s->w_work);
    put_u32(&p, s->r_work);
    *p++ = s->revoked;
    for (int i = 0; i < 3; i++) {
        put_u32(&p, s->buf[i].seq);
        put_u32(&p, s->buf[i].len);
        put_u16(&p, s->buf[i].ver);
    }
    put_u64(&p, s->t_publish);
    put_u64(&p, s->t_claim);
    put_u64(&p, s->t_drop);
    put_u64(&p, s->t_invalid);
    put_u64(&p, s->t_wsteps);
    put_u64(&p, s->t_rsteps);
    put_u32(&p, s->t_stall);
    put_u32(&p, s->t_tear);
    put_u32(&p, s->t_canary);
    put_u32(&p, s->step);
    return (size_t)(p - dst);  // 111
}

// ---------------------------------------------------------------------------
// Fold
// ---------------------------------------------------------------------------

void weft_replay_init(weft_replay_state_t* s) {
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    s->latest = 0;
    s->w_work = 1;
    s->r_work = 2;
    for (int i = 0; i < 3; i++) s->buf[i].ver = 1;
    // hash of step 0 is a pinned parity vector across all ports
    uint8_t ser[111];
    weft_replay_serialize(s, ser, sizeof(ser));
    s->hash = weft_replay_fnv1a(ser, 111);
}

weft_replay_result_t weft_replay_step(weft_replay_state_t* s,
                                      const weft_trace_event* e) {
    if (s == NULL || e == NULL) return WEFT_REPLAY_BAD_KIND;
    weft_replay_state_t next = *s;  // speculative copy: disagreement leaves
                                    // the caller's state untouched
    switch (e->kind) {
        case WEFT_TRACE_PUBLISH: {
            // buf[w_work] = {seq, aux, v1}; old = latest; latest = w_work;
            // w_work = old. THE exchange, exactly as weft.c's step 4-5.
            next.buf[next.w_work].seq = e->data;
            next.buf[next.w_work].len = e->aux;
            next.buf[next.w_work].ver = 1;
            uint32_t old = next.latest;
            next.latest = next.w_work;
            next.w_work = old;
            next.revoked = 0;  // modeling rule 2: publish implies rebind
            next.t_publish++;
            next.t_wsteps++;
            break;
        }
        case WEFT_TRACE_CLAIM: {
            // mine = latest; latest = r_work; r_work = mine.
            uint32_t mine = next.latest;
            next.latest = next.r_work;
            next.r_work = mine;
            if (next.buf[mine].seq != e->data) {
                return WEFT_REPLAY_DISAGREE;  // rule 3: never silent
            }
            next.t_claim++;
            next.t_rsteps++;
            break;
        }
        case WEFT_TRACE_DROP:
            next.epoch = e->aux;  // epoch at ACK
            next.t_drop++;
            break;
        case WEFT_TRACE_REVOKE:
            next.revoked = 1;
            break;
        case WEFT_TRACE_ACK:
            next.epoch = e->data;  // epoch after ACK
            break;
        case WEFT_TRACE_STALL:
            next.t_stall++;
            break;
        case WEFT_TRACE_TEAR:
            next.t_tear++;
            break;
        case WEFT_TRACE_CANARY_FAIL:
            next.t_canary++;
            break;
        default:
            return WEFT_REPLAY_BAD_KIND;
    }
    next.step++;
    uint8_t ser[111];
    weft_replay_serialize(&next, ser, sizeof(ser));
    next.hash = weft_replay_fnv1a(ser, 111);
    *s = next;
    return WEFT_REPLAY_OK;
}

weft_replay_result_t weft_replay_fold(weft_replay_state_t* s,
                                      const weft_trace_event* evs, size_t n,
                                      uint64_t* hashes_out) {
    weft_replay_result_t rc = WEFT_REPLAY_OK;
    for (size_t i = 0; i < n; i++) {
        rc = weft_replay_step(s, &evs[i]);
        if (rc != WEFT_REPLAY_OK) return rc;
        if (hashes_out) hashes_out[i] = s->hash;
    }
    return rc;
}
