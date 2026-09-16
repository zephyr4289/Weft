//! # RFC 0009 FreshnessGovernor — adaptive consumer strategy, Rust driver layer
//!
//! Mirrors `packages/core/src/governor.ts` and `core/c/governor.{h,c}` —
//! the action ladder, the Reseed cooldown, and the counter semantics are
//! pinned identically in all three (G5 proves it with a shared trace).
//!
//! RFC 0008's `FrameCursor` *measures* staleness (`frames_behind`); the
//! governor *acts* on it, per consumer, without touching the kernel:
//!
//! ```text
//! consumer ──framesBehind──> GOVERNOR ──{FastPath | Skip(n) | Snapshot | Reseed}──> consumer
//! ```
//!
//! The input is any monotonic per-consumer staleness count:
//! [`FrameCursor`](crate::frame_cursor::FrameCursor)'s `frames_behind` on
//! the triad kernel, or a fan-out reader's per-view dropped count.
//!
//! ACTIONS (closed set — a fifth action is a new RFC):
//! - `FastPath` — behind <= `fast_path_behind` (default 1). Draw live.
//! - `Skip(n)` — behind <= `skip_behind` (default 4). Draw newest only;
//!   `n = behind - fast_path_behind` intermediates are dropped **by
//!   decision** and counted in `decided_drops` (Law 4).
//! - `Snapshot` — behind <= `snapshot_behind` (default 16). Render one
//!   frame from a fresh claim, then jump expectations to latest. Advisory
//!   only — the governor never touches a [`Weft`](crate::Weft).
//! - `Reseed` — behind > `snapshot_behind`. The consumer cannot recover by
//!   skipping; rebuild it. Rate-limited to one per `reseed_cooldown_ms`
//!   (default 250) to prevent flap; a rate-limited Reseed degrades to
//!   `Snapshot` for that step.
//!
//! HYSTERESIS: none needed — the ladder is stateless per step; `Reseed` is
//! the only stateful action, gated by the cooldown clock.
//!
//! Law 2 (G4): `step` allocates nothing — state is four u32 counters + one
//! i64 timestamp, and `Action` is a `Copy` struct returned by value.
//!
//! TIME INJECTION: `now_ms` is a parameter, not a clock read — `step` is a
//! pure function of `(behind, now_ms)` and internal state. That is what
//! makes G3 (flap bound) and G5 (cross-language trace parity)
//! deterministic. Kind values are PROTOCOL (G5 packs them into trace
//! bytes; do not renumber).

/// The closed action set. Discriminants are protocol — do not renumber.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum GovernorActionKind {
    /// Draw live (behind <= fast_path_behind).
    FastPath = 0,
    /// Draw newest only; `skip_n` intermediates dropped by decision.
    Skip = 1,
    /// Render one frame from a fresh claim, then re-sync expectations.
    Snapshot = 2,
    /// The consumer cannot recover by skipping; rebuild it (cooldown-gated).
    Reseed = 3,
}

/// One decision's output. `Copy` — returned by value, zero allocation.
/// `skip_n` is valid only for `kind == Skip`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GovernorAction {
    /// Which branch of the ladder fired this step.
    pub kind: GovernorActionKind,
    /// Intermediates dropped by decision (valid only for `Skip`).
    pub skip_n: u32,
}

/// Ladder thresholds + cooldown.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GovernorConfig {
    /// behind <= fast_path_behind: draw live.
    pub fast_path_behind: u32,
    /// behind <= skip_behind: draw newest, skip intermediates.
    pub skip_behind: u32,
    /// behind <= snapshot_behind: draw once, re-sync.
    pub snapshot_behind: u32,
    /// Minimum spacing between emitted Reseeds (ms).
    pub reseed_cooldown_ms: u32,
}

/// RFC-0009 published defaults: FastPath<=1, Skip<=4, Snapshot<=16, Reseed
/// cooldown 250 ms. Mirrors `GOVERNOR_DEFAULTS` (TS) and the
/// `WEFT_GOV_*_DEFAULT` macros (C).
pub const GOVERNOR_DEFAULTS: GovernorConfig = GovernorConfig {
    fast_path_behind: 1,
    skip_behind: 4,
    snapshot_behind: 16,
    reseed_cooldown_ms: 250,
};

const NEVER: i64 = -1;

/// Per-consumer freshness governor. Driver-layer state only — it neither
/// retains nor touches any kernel object.
#[derive(Debug)]
pub struct Governor {
    cfg: GovernorConfig,
    last_reseed_ms: i64,
    /// Law 4: frames dropped BY DECISION (`Skip(n)` intermediates), distinct
    /// from any ring-level drop counter.
    pub decided_drops: u32,
    /// Emitted Reseeds (post-cooldown only).
    pub reseeds: u32,
    /// Total `step` calls (advisory).
    pub steps: u32,
}

impl Default for Governor {
    fn default() -> Self {
        Self::new(GOVERNOR_DEFAULTS)
    }
}

impl Governor {
    /// A fresh governor with the published default ladder.
    pub fn new(cfg: GovernorConfig) -> Self {
        Self {
            cfg,
            last_reseed_ms: NEVER,
            decided_drops: 0,
            reseeds: 0,
            steps: 0,
        }
    }

    /// The active ladder config.
    pub fn config(&self) -> &GovernorConfig {
        &self.cfg
    }

    /// One decision. Pure except the Reseed cooldown bookkeeping; zero
    /// allocation (G4). `now_ms` is caller-injected monotonic milliseconds.
    pub fn step(&mut self, frames_behind: u32, now_ms: i64) -> GovernorAction {
        self.steps += 1;
        if frames_behind <= self.cfg.fast_path_behind {
            GovernorAction { kind: GovernorActionKind::FastPath, skip_n: 0 }
        } else if frames_behind <= self.cfg.skip_behind {
            let n = frames_behind - self.cfg.fast_path_behind;
            self.decided_drops += n; // Law 4: decided drops are decisions
            GovernorAction { kind: GovernorActionKind::Skip, skip_n: n }
        } else if frames_behind <= self.cfg.snapshot_behind {
            GovernorAction { kind: GovernorActionKind::Snapshot, skip_n: 0 }
        } else {
            // behind > snapshot_behind: Reseed, rate-limited by the cooldown.
            if self.last_reseed_ms == NEVER
                || now_ms - self.last_reseed_ms >= self.cfg.reseed_cooldown_ms as i64
            {
                self.last_reseed_ms = now_ms;
                self.reseeds += 1;
                GovernorAction { kind: GovernorActionKind::Reseed, skip_n: 0 }
            } else {
                // Rate-limited: degrade to the best non-rebuild action. The
                // consumer still draws the newest frame; the rebuild returns
                // after the cooldown.
                GovernorAction { kind: GovernorActionKind::Snapshot, skip_n: 0 }
            }
        }
    }

    /// Reset the cooldown state and advisory counters (a rebuilt consumer
    /// starts fresh).
    pub fn reset(&mut self) {
        self.last_reseed_ms = NEVER;
        self.decided_drops = 0;
        self.reseeds = 0;
        self.steps = 0;
    }
}
