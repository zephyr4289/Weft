// fanout_chaos.h — RFC 0011: Deterministic Chaos & Race-Condition Fuzzing
//                     Engine for the RFC 0004 fan-out ring (C reference).
//
// WHY EXISTS (the "Exhaustive State-Space Proofs, Chaos Injection & Crash
// Resilience" branch): the torture gates (fanout_runner, F10) explore
// wall-clock interleavings the OS happens to produce. That is exactly the
// interleavings production produces — but it is NOT adversarial, and it is
// NOT reproducible: a torn-frame bug that fires once in 10M frames cannot
// be re-run, because the schedule that triggered it is gone. The chaos
// engine closes both gaps:
//
//   1. STEPPED mode — a deterministic scheduler drives every protocol
//      participant as a micro-step state machine (writer: invalidate ->
//      fill -> stamp -> publish; readers: the exact bounded claim loop).
//      A seeded xorshift128 PRNG picks which thread advances each step and,
//      at a configurable per-mille rate, injects one of four fault classes:
//        PREEMPT   — thread frozen 1 step (adversarial preemption)
//        STALL     — thread frozen 2 steps (memory-bus stall analog)
//        THROTTLE  — thread frozen 3 steps (CPU throttling analog)
//        REORDER   — thread's next fill/copy walks payload words in
//                    reversed order (out-of-order execution analog)
//      Given the same config, EVERY port (C, Rust, JVM, TS, Dart, Swift)
//      executes the BIT-IDENTICAL schedule and must emit the byte-identical
//      verdict JSON. The C engine is the reference oracle: `git grep` for
//      "chaos contract" in the other ports before touching anything here.
//
//   2. FREE mode — real OS threads over the production weft_fanout_t ring
//      with the same four fault classes injected at chaos points inside the
//      writer/reader loops (yield / bounded spin-stall / bounded pause /
//      reversed word order). This is the 10,000,000+-iteration tier: the
//      schedule is wall-clock, but the PRNG stream and every injection are
//      seed-deterministic, so the FAULT SEQUENCE replays exactly.
//
// PROPERTIES (the ledger — identical in every port, identical wording):
//   L-C1  NO TORN FRAME ACCEPTED  — every fresh claim's payload words match
//         tword(seq, w) word-for-word (the mix32 pattern, 04-LITMUS §0.1).
//   L-C2  NO FUTURE               — a claimed seq is <= the writer's frames.
//   L-C3  EXACT TELESCOPING       — per reader, sum(dropped) ==
//         lastSeq - freshClaims, and lastSeq == frames at drain end.
//   L-C4  PUBLISH COMPLETION      — publishes == frames at drain end.
//   L-C5  BOUNDED, NEVER SILENT   — every claim resolves to accept/skip/
//         exhausted within 4 attempts; every resolution is counted.
//   L-C6  STAMP BRACKET           — a slot's stamp is 0 only between its
//         invalidate and its stamp step (FI1 observable at the SM level).
//   L-C7  (v2, faultMask != 0) CONTROL-PLANE CORRUPTION IS DETECTED,
//         HARMLESS, OR ATTRIBUTED — see the v2 contract below.
//   A violation of any L-C* property fails the run and the verdict JSON
//   records the counters — a red chaos gate is a protocol bug, not flake.
//
// DETERMINISM CONTRACT (normative; mirrored in every port):
//   - PRNG: Marsaglia xorshift128 over four u32 words (a, b, c, d):
//       t = d; s = a; d = c; c = b; b = s;
//       t ^= t << 11;  t ^= t >>> 8;  a = t ^ s ^ (s >>> 10);  next = a
//     (all arithmetic mod 2^32; shifts are LOGICAL right).
//   - Seed derivation from a master u32 seed:
//       a = mix32(seed ^ 0xA341316C);  b = mix32(seed ^ 0xC8013EA4);
//       c = a ^ 0x9E3779B9;            d = b ^ 0x85EBCA6B;
//   - mix32 (04-LITMUS §0.1): x ^= x>>16; x *= 0x7FEB352D; x ^= x>>15;
//       x *= 0x846CA68B; x ^= x>>16.
//   - tword(seq, w) = mix32(seq * 2654435761 + w)   (u32 wrap).
//   - Scheduler, per step:
//       tid  = next() % (readers + 1)          // 0 = writer, 1..R readers
//       if next() % 1000 < chaosRate:          // fault draw
//         victim = next() % (readers + 1)
//         kind   = next() % 4                  // preempt, stall, throttle, reorder
//         freeze[victim] += {preempt:1, stall:2, throttle:3, reorder:0}
//         if kind == reorder: reorderFlag[victim] = !reorderFlag[victim]
//         injections[kind]++
//       advance = freeze[tid] > 0 ? (freeze[tid]--, no progress) : one SM step
//     A frozen thread burns its freeze only when the scheduler CHOOSES it —
//     identical rule in every port.
//   - Drain: after the step budget (or early completion), advance all SMs
//     round-robin (writer, reader 1..R), no faults, until every SM is DONE.
//     The drain is deterministic and must terminate; a non-terminating drain
//     is a protocol bug (L-C5) and fails the run.
//
// V2 CONTRACT (Issue #16 Tier 1 Task 3 — four NEW fault classes, gated by
// fault_mask; fault_mask == 0 reproduces the v1 contract BIT-EXACTLY: same
// PRNG stream, same schedule, same JSON — the committed golden fixtures and
// every port's legacy parity are untouched evidence):
//
//   Extended fault classes (fault_mask bits, per-mille-gated like v1):
//     BIT_FLIP           (bit 0) — XOR 1 << (rng % 64) into a RANDOM slot's
//                                  u64 stamp (the control plane; see below).
//     CACHE_POISON       (bit 1) — 8 consecutive u64 stamps (one 64-byte
//                                  "cache line" of the ctrl block) :=
//                                  0xDEDEDEDEDEDEDEDE.
//     STORE_TEARING      (bit 2) — the writer's NEXT stamp store executes
//                                  as TWO half-stores with a preemptible gap.
//     DELAYED_VISIBILITY  (bit 3) — the writer's next ctrl store is followed
//                                  by an N-step freeze (store-buffer drain
//                                  stall analog; N = 1..8 from the stream).
//
//   Extended draw order (normative — mirrors in every port; runs ONLY when
//   fault_mask != 0, AFTER the legacy fault draw of the same step):
//     if next() % 1000 < chaos_rate:
//       kind = next() % 4
//       if (fault_mask >> kind) & 1:
//         BIT_FLIP:           cell = next() % slots; bit = next() % 64
//         CACHE_POISON:       cell = next() % slots  (poison wraps, clamped)
//         STORE_TEARING:      victim = next() % (readers + 1)
//         DELAYED_VISIBILITY: victim = next() % (readers + 1); n = next()%8+1
//       (a kind whose mask bit is clear consumes NO further draws — the
//        parameters are drawn only when the fault fires)
//
//   TARGETING DISCIPLINE (why stamps, not payload): the fanout bracket
//   detects CONCURRENCY, not data corruption — a flipped payload word would
//   be a SILENT wrong-accept by construction (undetectable, unattributable;
//   payload integrity is the application checksum's surface: w6Checksum,
//   kernel canary L12). The chaos engine therefore corrupts the CONTROL
//   PLANE (u64 slot stamps), where the protocol HAS detection machinery:
//   stamp corruption manifests as graceful skip/retry, self-heals when the
//   writer re-stamps the slot on wrap, and is counted. latestSeq and
//   publishes are never corrupted (a garbage latestSeq would strand the
//   drain: readers chase it forever — L-C5 non-termination — with no
//   self-heal path; the real hazard is documented, not injected).
//
//   THE ALIASING HAZARD — PREDICTED, HUNTED, AND STRUCTURALLY CLOSED (the
//   honest record, Law 4): a bit flip CAN transform stamp A into exactly the
//   seq a reader chases (3 ^ 4 == 7), which naively reads as a forged frame.
//   It is NOT reachable, and the reason is the protocol's own ordering:
//   (a) a MID-FILL slot's stamp is only compared by readers chasing latest,
//       and latest only advances to frame L AFTER the writer's real stamp
//       overwrote any forged value in that slot (FI1: invalidate-before-fill,
//       stamp-before-publish) — the forgery is always overwritten before it
//       becomes chaseable; (b) a PUBLISHED slot's stamp IS the chaseable
//       value, so there is no stale-stamp surface to forge. Empirical
//   confirmation: 2,400+ maximum-corruption probe runs (mask=BIT_FLIP,
//   rate 400-999/1000, the tightest W=1 copy windows) produced ZERO torn
//   accepts — torn_accepted stays 0 across every corruption density the
//   engine can express. The attributed_torn counter (a torn accept on a
//   stamp-dirty slot) is therefore PROPHYLACTIC: the detector machinery
//   exists and reports 0 by protocol design — if a future fault class (or a
//   protocol change) ever opens a real forgery window, L-C7 will attribute
//   it instead of mislabeling it a spontaneous protocol bug. L-C7:
//   torn_accepted == attributed_torn — zero UNATTRIBUTED tears; with
//   stamp-only corruption this reduces to the v1 L-C1 and is expected to
//   hold with attribution pinned at 0.
//   Two more measured results this tier records:
//   - STORE_TEARING is VALUE-RANGE BENIGN within config bounds (stamps <
//     2^32 for any legal frame count, so the torn mid-store equals the
//     completed store; the split-store model demonstrates this
//     mechanically — tear on/off runs differ only in injected counters and
//     scheduling, never in verdicts).
//   - The CORRUPTION OVERLOAD THRESHOLD: slots >= 8 with chaos_rate >= 100
//     whole-ring-poisons faster than the writer heals (poison clamps to M
//     stamps), and the scheduler budget starves before drain — the engine
//     reports drained=false (L-C5) rather than papering over it. Gate
//     configs stay below the threshold; the threshold itself is the
//     evidence-filed measurement (litmus/evidence/chaos-v2/).
//
// LAW 2: the stepped engine allocates its fixed model state once; the free
// engine follows fanout_runner's discipline (buffers once per run, nothing
// in the loops). LAW 1: every resolution path is counted, never silent.
// LAW 4: the stepped engine models the RFC 0004 protocol; it does not
// replace the loom exhaustive models or the TLA+ specifications — it makes
// their properties executable at production scale, reproducibly.

#ifndef WEFT_FANOUT_CHAOS_H
#define WEFT_FANOUT_CHAOS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Chaos fault classes (the four v1 axes + the four Issue #16 v2 axes).
typedef enum {
    WEFT_CHAOS_PREEMPT = 0,   ///< thread frozen 1 step
    WEFT_CHAOS_STALL   = 1,   ///< thread frozen 2 steps (bus-stall analog)
    WEFT_CHAOS_THROTTLE = 2,  ///< thread frozen 3 steps (CPU-throttle analog)
    WEFT_CHAOS_REORDER = 3,   ///< reversed word order on next fill/copy
    WEFT_CHAOS_KINDS   = 4,
    // v2 extended classes (Issue #16 Tier 1) — indexed 0..3 in the EXT
    // namespace and gated by fault_mask bits of the same index:
    WEFT_CHAOS_EXT_BIT_FLIP = 0,           ///< v2: XOR a bit of a slot stamp
    WEFT_CHAOS_EXT_CACHE_POISON = 1,       ///< v2: 64B of ctrl = 0xDE..DE
    WEFT_CHAOS_EXT_STORE_TEARING = 2,      ///< v2: split stamp store (preemptible)
    WEFT_CHAOS_EXT_DELAYED_VISIBILITY = 3, ///< v2: post-store freeze (drain stall)
    WEFT_CHAOS_EXT_KINDS = 4
} weft_chaos_kind_t;

/// fault_mask bits (v2, Issue #16). Bit k enables extended class k.
/// Default 0 = the v1 contract, bit-exactly (same PRNG stream, same JSON).
#define WEFT_CHAOS_MASK_BIT_FLIP           0x1u
#define WEFT_CHAOS_MASK_CACHE_POISON       0x2u
#define WEFT_CHAOS_MASK_STORE_TEARING      0x4u
#define WEFT_CHAOS_MASK_DELAYED_VISIBILITY 0x8u
#define WEFT_CHAOS_MASK_ALL_EXT            0xFu

/// Chaos engine configuration (RFC 0011 §Chaos contract — all u32 so the
/// JSON surface is stable across 32/64-bit ports).
typedef struct {
    uint32_t seed;       ///< master seed (seedA/B derived per contract)
    uint32_t steps;      ///< stepped mode: scheduler step budget
    uint32_t slots;      ///< M — ring depth (2..64; models use 2..8)
    uint32_t words;      ///< W — payload words per slot (1..64; the F10 torture shape is 64)
    uint32_t readers;    ///< R — concurrent reader SMs (1..4)
    uint32_t frames;     ///< F — frames the writer publishes
    uint32_t chaos_rate; ///< per-mille fault probability at each step (0..1000)
    uint32_t fault_mask; ///< v2 (Issue #16): extended-class enable bits (0 = v1 contract)
} weft_chaos_config_t;

/// Per-run property ledger (L-C1..L-C7). Every counter is observable in the
/// verdict JSON; a port that cannot account for a path has a bug.
typedef struct {
    uint64_t fresh_claims[4];   ///< per reader (L-C1 accepts)
    uint64_t sum_dropped[4];    ///< per reader (L-C3 telescoping lhs)
    uint64_t last_seq[4];       ///< per reader (L-C3 telescoping rhs anchor)
    uint64_t skips[4];          ///< graceful mid-overwrite skips (L-C5)
    uint64_t exhausted[4];      ///< bounded-attempt exhaustions (L-C5)
    uint64_t publishes;         ///< writer publishes at drain end (L-C4)
    uint64_t torn_accepted;     ///< L-C1 (v1: MUST be 0; v2: == attributed_torn, L-C7)
    uint64_t future_claims;     ///< L-C2 violations (MUST be 0)
    uint64_t bracket_violations;///< L-C6 violations (MUST be 0)
    uint64_t steps_executed;    ///< scheduler steps consumed
    uint64_t injected[WEFT_CHAOS_KINDS]; ///< per v1 fault class
    // v2 (Issue #16 Tier 1) — all zero when fault_mask == 0:
    uint64_t injected_ext[WEFT_CHAOS_EXT_KINDS]; ///< per extended class
    uint64_t attributed_torn;  ///< L-C7: tears on corruption-dirty slots
    uint64_t ctrl_corruptions; ///< stamp cells hit by flip/poison injections
    bool     drained;           ///< drain completed within its bound
} weft_chaos_ledger_t;

/// Run verdict.
typedef struct {
    weft_chaos_ledger_t ledger;
    bool     pass;       ///< all L-C* properties hold
    char     engine[24]; ///< "stepped" or "free" (JSON engine field)
    uint64_t elapsed_ns; ///< free mode only; stepped mode 0
} weft_chaos_verdict_t;

/// Run the deterministic stepped engine. Pure function of (config); no OS
/// threading, no wall-clock dependence. Returns 0 on protocol PASS, 1 on
/// any violation, 2 on contract misuse (bad config). The verdict is always
/// populated.
int weft_chaos_run_stepped(const weft_chaos_config_t* cfg,
                           weft_chaos_verdict_t* out);

/// Run the free-running engine (real threads over weft_fanout_t, chaos
/// points inside the loops, seed-deterministic fault sequence). Same
/// return-code contract.
int weft_chaos_run_free(const weft_chaos_config_t* cfg,
                        weft_chaos_verdict_t* out);

/// Serialize a verdict to the CONTRACT JSON (byte-identical across ports —
/// compact separators, fixed field order, integers only). Writes at most
/// cap-1 bytes plus NUL. Returns the number of bytes written (excluding
/// NUL), or -1 if cap was too small (the caller grows and retries).
int weft_chaos_verdict_json(const weft_chaos_config_t* cfg,
                            const weft_chaos_verdict_t* v,
                            char* buf, size_t cap);

/// Self-test: PRNG stream against committed vectors, mix32/tword vectors,
/// and a tiny stepped run with a known-good ledger. Exit code conveys the
/// verdict (0 clean). Used by the chaos shard before any long run.
int weft_chaos_selftest(void);

#ifdef __cplusplus
}
#endif

#endif // WEFT_FANOUT_CHAOS_H
