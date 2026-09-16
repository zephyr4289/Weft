//! RFC-0009 FreshnessGovernor conformance (G-series), Rust.
//!
//! Mirrors `packages/core/test/governor.test.ts` and
//! `core/c/governor_test.c` (G1–G4 local; G5 is the shared-trace xlang
//! fixture in `fixtures/xlang-governor/`, driven by the
//! `governor_xlang` bin).

use weft_core::governor::{
    Governor, GovernorActionKind, GOVERNOR_DEFAULTS,
};

/// xorshift32 — 04-LITMUS §0.2, the repo's canonical deterministic RNG.
/// Identical step in C/Rust/TS (G5's trace generator).
fn xorshift32(mut x: u32) -> u32 {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x
}

#[test]
fn g1_ladder() {
    let mut gov = Governor::default();
    // now advances past the cooldown every step so Reseed is never
    // suppressed (G1 tests the LADDER, not the rate limit).
    for behind in 0u32..=64 {
        let a = gov.step(behind, behind as i64 * 1000);
        if behind <= GOVERNOR_DEFAULTS.fast_path_behind {
            assert_eq!(a.kind, GovernorActionKind::FastPath, "behind={behind}");
            assert_eq!(a.skip_n, 0);
        } else if behind <= GOVERNOR_DEFAULTS.skip_behind {
            assert_eq!(a.kind, GovernorActionKind::Skip, "behind={behind}");
            assert_eq!(a.skip_n, behind - GOVERNOR_DEFAULTS.fast_path_behind);
        } else if behind <= GOVERNOR_DEFAULTS.snapshot_behind {
            assert_eq!(a.kind, GovernorActionKind::Snapshot, "behind={behind}");
            assert_eq!(a.skip_n, 0);
        } else {
            assert_eq!(a.kind, GovernorActionKind::Reseed, "behind={behind}");
            assert_eq!(a.skip_n, 0);
        }
    }
}

#[test]
fn g2_monotone() {
    let mut gov = Governor::default();
    let class = |k| k as i32;
    let mut prev = -1;
    for behind in 0u32..=64 {
        let a = gov.step(behind, behind as i64 * 1000);
        assert!(class(a.kind) >= prev, "behind={behind}");
        prev = class(a.kind);
    }
}

#[test]
fn g3_reseed_flap_bound() {
    let mut gov = Governor::default();
    let steps = 10_000u32;
    let mut state = 0x00C0_FEEEu32;
    let mut reseeds = 0u32;
    let mut last_reseed_at: i64 = -1;
    for i in 0..steps as i64 {
        state = xorshift32(state);
        let behind = state % 128; // spikes well past 16
        let a = gov.step(behind, i); // 1 ms per step
        if a.kind == GovernorActionKind::Reseed {
            reseeds += 1;
            if last_reseed_at >= 0 {
                assert!(i - last_reseed_at >= 250, "cooldown spacing at t={i}");
            }
            last_reseed_at = i;
        }
    }
    let bound = (steps + 249) / 250; // ceil(10000/250) = 40
    assert!(reseeds <= bound, "reseeds={reseeds} bound={bound}");
    // A 10 s trace at ~50% spike probability must exercise the cooldown.
    assert!(reseeds > 0);
    assert_eq!(gov.reseeds, reseeds);
}

#[test]
fn g3b_suppressed_reseed_degrades_to_snapshot() {
    let mut gov = Governor::default();
    assert_eq!(gov.step(64, 1000).kind, GovernorActionKind::Reseed);
    // 50 ms later — inside the 250 ms cooldown: degraded, not flapped.
    assert_eq!(gov.step(64, 1050).kind, GovernorActionKind::Snapshot);
    // After the cooldown: Reseed again.
    assert_eq!(gov.step(64, 1300).kind, GovernorActionKind::Reseed);
    assert_eq!(gov.reseeds, 2);
}

#[test]
fn g4_zero_allocation_by_construction() {
    // Rust's zero-alloc proof is structural: `step` takes `&mut self`,
    // returns a `Copy` struct by value, and touches no heap type. A heap
    // allocation would have to be visible in the signature. The 1M-step
    // smoke run below pins the counter arithmetic (the TS twin measures
    // heapUsed; C proves by construction — the same statement as here).
    let mut gov = Governor::default();
    for i in 0..1_000_000u32 {
        let _ = gov.step(i % 32, i as i64);
    }
    assert_eq!(gov.steps, 1_000_000);
}

#[test]
fn law4_decided_drops() {
    let mut gov = Governor::default();
    let _ = gov.step(3, 0); // Skip(2)
    let _ = gov.step(4, 1); // Skip(3)
    let _ = gov.step(1, 2); // FastPath — no decided drops
    let _ = gov.step(9, 3); // Snapshot — none
    assert_eq!(gov.decided_drops, 2 + 3);
    assert_eq!(gov.steps, 4);
}

#[test]
fn custom_thresholds_and_reset() {
    let mut gov = Governor::new(weft_core::governor::GovernorConfig {
        fast_path_behind: 0,
        skip_behind: 8,
        snapshot_behind: 32,
        reseed_cooldown_ms: 10,
    });
    assert_eq!(gov.step(0, 0).kind, GovernorActionKind::FastPath);
    let a = gov.step(1, 0);
    assert_eq!(a.kind, GovernorActionKind::Skip);
    assert_eq!(a.skip_n, 1);
    assert_eq!(gov.step(33, 0).kind, GovernorActionKind::Reseed);
    gov.reset();
    assert_eq!((gov.steps, gov.decided_drops, gov.reseeds), (0, 0, 0));
    // After reset the cooldown clock starts fresh.
    assert_eq!(gov.step(33, 0).kind, GovernorActionKind::Reseed);
}
