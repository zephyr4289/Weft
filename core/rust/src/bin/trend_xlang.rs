//! trend_xlang — RFC 0020 cross-language verdict-stream emitter.
//!
//! Drives the trend estimator with the deterministic behind trace
//! (behind = xorshift32(state) % 64, seed 0x00C0FFEE) and prints the
//! packed verdict stream (`verdict << 6 | min(skip_n, 63)`, hex, one
//! line) — byte-identical to core/c/trend_runner.c,
//! fixtures/xlang-trend/trend_emitter.mjs, and the Kotlin/Swift/Dart VM
//! emitters. fixtures/xlang-trend/run.sh byte-compares them all.
//!
//! Usage: trend_xlang [STEPS] [SEED]

use weft_core::trend::{trend_observe, trend_pack, WeftTrend};

fn xorshift32(mut x: u32) -> u32 {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let steps: u64 = args
        .get(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(10_000);
    let seed: u32 = args
        .get(2)
        .and_then(|s| u32::from_str_radix(s.trim_start_matches("0x"), 16).ok())
        .unwrap_or(0x00C0_FEEE);

    let mut t = WeftTrend::default();
    let mut state = seed;
    let mut out = String::with_capacity(steps as usize * 2 + 1);
    for _ in 0..steps {
        state = xorshift32(state);
        let behind = state % 64;
        let o = trend_observe(&mut t, behind);
        out.push_str(&format!("{:02x}", trend_pack(&o)));
    }
    out.push('\n');
    print!("{out}");
}
