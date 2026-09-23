// weft_replay.h — RFC 0019: deterministic time-travel replay fold, C reference.
//
// A pure state machine over a .weftrec v4 event stream: after every event
// it reconstructs the full shadow kernel state and reduces it to a 64-bit
// FNV-1a hash. Every port implements EXACTLY the transition table and the
// canonical byte serialization in RFC-0019 — cross-runtime hash logs are
// byte-compared (fixtures/xlang-replay/). Read that RFC's "modeling
// contract" before changing ANY line here: the three declared abstractions
// (null-frame len 0, PUBLISH-implies-unrevoked, claim validation) are what
// make the fold deterministic without payload bytes.

#ifndef WEFT_REPLAY_H
#define WEFT_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "trace_rec.h"  // weft_trace_event

#define WEFT_REPLAY_CHECKPOINT 64u  // checkpoint interval (jump cost bound)

/// Fold verdicts.
typedef enum {
    WEFT_REPLAY_OK        = 0,   // step applied, hash updated
    WEFT_REPLAY_DISAGREE  = -1,  // trace/model disagreement (claim seq
                                 // mismatch — the class of bug this
                                 // debugger exists to surface; NEVER silent)
    WEFT_REPLAY_BAD_KIND  = -2,  // unknown event kind in the stream
} weft_replay_result_t;

/// Shadow kernel state (RFC-0019 fold state). POD — checkpointing is a
/// plain struct copy, no allocator involved (Law 2).
typedef struct {
    uint32_t latest;
    uint32_t epoch;
    uint32_t w_work;
    uint32_t r_work;
    uint8_t  revoked;
    struct {
        uint32_t seq;
        uint32_t len;
        uint16_t ver;
    } buf[3];
    uint64_t t_publish;
    uint64_t t_claim;
    uint64_t t_drop;
    uint64_t t_invalid;
    uint64_t t_wsteps;
    uint64_t t_rsteps;
    uint32_t t_stall;
    uint32_t t_tear;
    uint32_t t_canary;
    uint32_t step;      // events consumed
    uint64_t hash;      // FNV-1a over the canonical serialization
} weft_replay_state_t;

/// Reset to the RFC-0019 initial state (including the initial hash — the
/// hash of step 0 is a pinned parity vector).
void weft_replay_init(weft_replay_state_t* s);

/// Apply ONE event. Returns WEFT_REPLAY_OK and updates s->hash, or a
/// verdict above WITHOUT mutating state (a disagreement leaves the fold at
/// the last good step — the debugger can inspect it).
weft_replay_result_t weft_replay_step(weft_replay_state_t* s,
                                      const weft_trace_event* e);

/// Canonical little-endian serialization (RFC-0019) into dst.
/// Returns bytes written (111). Exposed for cross-port byte tests.
size_t weft_replay_serialize(const weft_replay_state_t* s, uint8_t* dst,
                             size_t cap);

/// FNV-1a 64 over a byte range — exported so ports share one definition.
uint64_t weft_replay_fnv1a(const uint8_t* data, size_t len);

/// Convenience: fold `n` events, writing per-step hashes to hashes_out
/// (caller storage, cap >= n; may be NULL). Returns the last verdict.
weft_replay_result_t weft_replay_fold(weft_replay_state_t* s,
                                      const weft_trace_event* evs, size_t n,
                                      uint64_t* hashes_out);

#endif  // WEFT_REPLAY_H
