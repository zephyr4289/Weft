// governor.h — RFC 0009: FreshnessGovernor, C driver layer
//
// WHY EXISTS: RFC 0009. RFC 0008's frame cursor *measures* staleness
// (framesBehind); the governor *acts* on it, per consumer, without touching
// the kernel. Every real consumer hand-rolls the same ladder — skip the
// minimap when behind, re-snapshot when very behind — duplicated per app,
// tuned by vibes, untestable. The governor makes the ladder a spec'd,
// testable driver-layer object with a CLOSED action set:
//
//     consumer ──framesBehind──> GOVERNOR ──{FastPath | Skip(n) | Snapshot | Reseed}──> consumer
//
// The input is ANY monotonic per-consumer staleness count:
// weft_frame_cursor_update()'s frames_behind on the triad kernel, or a
// fan-out reader's per-view dropped count (same semantics).
//
// ACTIONS (closed set — a fifth action is a new RFC):
//   FastPath  — behind <= fast_path_behind (default 1). Draw live.
//   Skip(n)   — behind <= skip_behind (default 4). Draw newest only;
//               n = behind - fast_path_behind intermediates are dropped BY
//               DECISION and counted in decided_drops (Law 4: decided drops
//               are decisions, not accidents — distinct from ring counters).
//   Snapshot  — behind <= snapshot_behind (default 16). Render one frame
//               from a fresh claim, then jump expectations to latest.
//               Advisory only: the governor never touches a weft_t.
//   Reseed    — behind > snapshot_behind. The consumer cannot recover by
//               skipping; rebuild it. Rate-limited to one per
//               reseed_cooldown_ms (default 250) to prevent flap; a
//               rate-limited Reseed degrades to Snapshot for that step.
//
// HYSTERESIS: none needed — the ladder is stateless per step (actions depend
// only on the current behind); Reseed is the only stateful action, gated by
// the cooldown clock.
//
// LAW 2 (G4): step() performs zero allocation — no malloc anywhere in the
// path; all state is four u32 counters + one i64 timestamp, and the returned
// action is the governor's OWN identity-stable record, mutated in place
// (the same pattern as weft_frame_cursor_t's sample record). Read it
// synchronously; do not retain it across steps.
//
// TIME INJECTION: now_ms is a parameter, not a clock read — step() is a pure
// function of (behind, now_ms) and internal state. That is what makes G3
// (flap bound) and G5 (cross-language trace parity) deterministic: the same
// (behind, now_ms) trace MUST produce the identical action sequence in C,
// Rust, and TS. Callers pass any monotonic millisecond clock.
//
// Kind values are PROTOCOL (G5 packs them into trace bytes; do not renumber).

#ifndef WEFT_GOVERNOR_H
#define WEFT_GOVERNOR_H

#include <stdint.h>

/// The closed action set. Numeric values are protocol (see header comment).
typedef enum {
    WEFT_GOV_FAST_PATH = 0,
    WEFT_GOV_SKIP      = 1,
    WEFT_GOV_SNAPSHOT  = 2,
    WEFT_GOV_RESEED    = 3,
} weft_gov_kind_t;

/// Identity-stable action record — mutated in place by weft_governor_step(),
/// never allocated per call. skip_n is valid only for kind == WEFT_GOV_SKIP.
typedef struct {
    weft_gov_kind_t kind;
    uint32_t skip_n;
} weft_gov_action_t;

/// Governor state + config. Zero-alloc by construction; initialize with
/// weft_governor_init() or WEFT_GOVERNOR_DEFAULTS.
typedef struct {
    // Config (the ladder + cooldown — RFC-0009 published defaults in macros)
    uint32_t fast_path_behind;
    uint32_t skip_behind;
    uint32_t snapshot_behind;
    uint32_t reseed_cooldown_ms;
    // State
    int64_t  last_reseed_ms;  ///< -1 = never reseeded
    // Counters (advisory, AXIOM T)
    uint32_t decided_drops;   ///< Law 4: frames dropped BY DECISION (Skip(n) intermediates)
    uint32_t reseeds;         ///< Emitted Reseeds (post-cooldown only)
    uint32_t steps;           ///< Total step() calls
    // Identity-stable action record
    weft_gov_action_t act;
} weft_governor_t;

/// RFC-0009 published defaults: FastPath<=1, Skip<=4, Snapshot<=16, Reseed
/// cooldown 250 ms. Mirrors GOVERNOR_DEFAULTS in TS and Governor::defaults()
/// in Rust.
#define WEFT_GOV_FAST_PATH_BEHIND_DEFAULT  1u
#define WEFT_GOV_SKIP_BEHIND_DEFAULT       4u
#define WEFT_GOV_SNAPSHOT_BEHIND_DEFAULT   16u
#define WEFT_GOV_RESEED_COOLDOWN_MS_DEFAULT 250u

/// Initialize with the published defaults. Zero allocation.
void weft_governor_init_default(weft_governor_t* g);

/// Initialize with a custom ladder. Zero allocation. Threshold order is NOT
/// validated here (fast<=skip<=snapshot) — G1 pins the default ladder; a
/// caller who passes an inverted ladder owns the (documented) consequences.
void weft_governor_init(weft_governor_t* g,
                        uint32_t fast_path_behind,
                        uint32_t skip_behind,
                        uint32_t snapshot_behind,
                        uint32_t reseed_cooldown_ms);

/// One decision. Pure except the Reseed cooldown bookkeeping; zero
/// allocation (G4 — no malloc in the path). `now_ms` is caller-injected
/// monotonic milliseconds (any clock; the same trace must yield the same
/// actions across languages — G5).
///
/// Returns the governor-owned action record (identity-stable, mutated in
/// place — read it synchronously, do not retain across steps).
const weft_gov_action_t* weft_governor_step(weft_governor_t* g,
                                            uint32_t frames_behind,
                                            int64_t now_ms);

/// Reset the cooldown state and advisory counters (a rebuilt consumer
/// starts fresh).
void weft_governor_reset(weft_governor_t* g);

#endif // WEFT_GOVERNOR_H
