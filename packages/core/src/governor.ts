// governor.ts — FreshnessGovernor: adaptive consumer strategy from framesBehind
//
// WHY EXISTS: RFC-0009. RFC-0008's FrameCursor *measures* staleness
// (framesBehind); the governor *acts* on it, per consumer, without touching
// the kernel. Every real consumer hand-rolls the same ladder — "if I'm
// behind, skip the minimap; if I'm very behind, re-snapshot" — duplicated per
// app, tuned by vibes, untestable. The governor makes the ladder a spec'd,
// testable driver-layer object with a CLOSED action set:
//
//     consumer ──framesBehind──> GOVERNOR ──{FastPath | Skip(n) | Snapshot | Reseed}──> consumer
//
// The input is ANY monotonic per-consumer staleness count: FrameCursor's
// framesBehind on the triad kernel, or WeftFanoutReader's claim.dropped on
// the fan-out ring (same semantics: frames published between this
// consumer's claims that it never saw).
//
// ACTIONS (closed set — a fifth action is a new RFC):
//   FastPath  — behind <= fastPathBehind (default 1). Draw live.
//   Skip(n)   — behind <= skipBehind (default 4). Draw newest only;
//               n = behind - fastPathBehind intermediates are dropped BY
//               DECISION and counted in `decidedDrops` (Law 4: decided drops
//               are decisions, not accidents — kept distinct from the ring's
//               own counters).
//   Snapshot  — behind <= snapshotBehind (default 16). Render one frame from
//               a fresh claim, then jump the consumer's expectations to
//               latest. Advisory only: the governor never touches a Triad.
//   Reseed    — behind > snapshotBehind. The consumer cannot recover by
//               skipping; rebuild it. Rate-limited to one per
//               reseedCooldownMs (default 250) to prevent flap; a
//               rate-limited Reseed degrades to Snapshot for that step — the
//               consumer still draws the newest frame, and the rebuild
//               recommendation returns after the cooldown.
//
// HYSTERESIS: none needed — the ladder is stateless per step (actions depend
// only on the current `behind`); Reseed is the only stateful action, gated
// by the cooldown clock.
//
// Zero allocation per step (Law 2, G4): state is four u32 counters + one
// timestamp; `step()` returns the governor's OWN identity-stable action
// record, mutated in place — the same pattern as the C kernel's
// weft_frame_cursor_t sample record. Read it synchronously; do not retain it
// across steps.
//
// TIME INJECTION: `nowMs` is a parameter, not a clock read — `step()` is a
// pure function of (behind, nowMs) and internal state. That is what makes
// G3 (flap bound) and G5 (cross-language trace parity) deterministic: the
// same (behind, nowMs) trace MUST produce the identical action sequence in
// TS, C, and Rust. Callers pass any monotonic millisecond clock
// (performance.now(), a paced counter, a replayed trace).

/// The closed action set (plain const object, house style). Numeric values
/// are PROTOCOL (G5 packs them into trace bytes; do not renumber).
export const GovernorActionKind = {
  FastPath: 0,
  Skip: 1,
  Snapshot: 2,
  Reseed: 3,
} as const;
export type GovernorActionKind =
  (typeof GovernorActionKind)[keyof typeof GovernorActionKind];

/// Identity-stable action record — mutated in place by step(), never
/// allocated per call (G4). `skipN` is valid only for kind === Skip.
export interface GovernorAction {
  kind: GovernorActionKind;
  skipN: number;
}

/// Ladder thresholds + cooldown, with RFC-0009's published defaults.
export interface GovernorConfig {
  /// behind <= fastPathBehind: draw live. Default 1.
  fastPathBehind: number;
  /// behind <= skipBehind: draw newest, skip intermediates. Default 4.
  skipBehind: number;
  /// behind <= snapshotBehind: draw once, re-sync. Default 16.
  snapshotBehind: number;
  /// Minimum spacing between emitted Reseeds. Default 250 ms.
  reseedCooldownMs: number;
}

export const GOVERNOR_DEFAULTS: Readonly<GovernorConfig> = {
  fastPathBehind: 1,
  skipBehind: 4,
  snapshotBehind: 16,
  reseedCooldownMs: 250,
};

const NEVER: number = -1;

export class FreshnessGovernor {
  private readonly cfg: GovernorConfig;
  private lastReseedMs: number = NEVER;
  /// Law 4: frames dropped BY DECISION (Skip(n) intermediates), distinct
  /// from any ring-level drop counter.
  decidedDrops = 0;
  /// Emitted Reseeds (post-cooldown only).
  reseeds = 0;
  /// Total step() calls (advisory).
  steps = 0;
  /// The identity-stable action record step() returns.
  readonly act: GovernorAction = { kind: GovernorActionKind.FastPath, skipN: 0 };

  constructor(config: Partial<GovernorConfig> = {}) {
    this.cfg = { ...GOVERNOR_DEFAULTS, ...config };
  }

  get config(): Readonly<GovernorConfig> {
    return this.cfg;
  }

  /// One decision. Pure except the Reseed cooldown bookkeeping; zero
  /// allocation (G4). `nowMs` is caller-injected monotonic milliseconds.
  step(framesBehind: number, nowMs: number): GovernorAction {
    this.steps++;
    const behind = framesBehind >>> 0;
    const a = this.act;
    if (behind <= this.cfg.fastPathBehind) {
      a.kind = GovernorActionKind.FastPath;
      a.skipN = 0;
    } else if (behind <= this.cfg.skipBehind) {
      const n = behind - this.cfg.fastPathBehind;
      this.decidedDrops += n;
      a.kind = GovernorActionKind.Skip;
      a.skipN = n;
    } else if (behind <= this.cfg.snapshotBehind) {
      a.kind = GovernorActionKind.Snapshot;
      a.skipN = 0;
    } else {
      // behind > snapshotBehind: Reseed, rate-limited by the cooldown.
      if (
        this.lastReseedMs === NEVER ||
        nowMs - this.lastReseedMs >= this.cfg.reseedCooldownMs
      ) {
        this.lastReseedMs = nowMs;
        this.reseeds++;
        a.kind = GovernorActionKind.Reseed;
        a.skipN = 0;
      } else {
        // Rate-limited: degrade to the best non-rebuild action. The consumer
        // still draws the newest frame; the rebuild returns after cooldown.
        a.kind = GovernorActionKind.Snapshot;
        a.skipN = 0;
      }
    }
    return a;
  }

  /// Reset the cooldown state (a rebuilt consumer starts fresh). Advisory
  /// accumulators are zeroed; decidedDrops/reseeds/steps restart.
  reset(): void {
    this.lastReseedMs = NEVER;
    this.decidedDrops = 0;
    this.reseeds = 0;
    this.steps = 0;
    this.act.kind = GovernorActionKind.FastPath;
    this.act.skipN = 0;
  }
}
