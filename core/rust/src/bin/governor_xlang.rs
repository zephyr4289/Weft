//! governor_xlang — G5 cross-language trace dump (RFC-0009).
//!
//! Emits the packed action log for a deterministic (behind, now_ms) trace:
//!
//! ```text
//! state = SEED (default 0x00C0FFEE)
//! for i in 0..N: state = xorshift32(state);
//!                behind = state % 128; now_ms = i;
//!                action = governor.step(behind, now_ms)
//!                emit byte (kind << 6) | min(skip_n, 63)
//! ```
//!
//! Hex-encoded (lowercase, no separators, one trailing newline) — the exact
//! format `core/c/governor_test.c xlang-dump` and the TS fixture emit.
//! `fixtures/xlang-governor/run.sh` byte-compares all three.
//!
//! Usage: governor_xlang [STEPS [SEED]]

use weft_core::governor::Governor;

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
        .unwrap_or(0x00C0_FFEE);

    let mut gov = Governor::default();
    let mut state = seed;
    let mut out = String::with_capacity(steps as usize * 2 + 1);
    for i in 0..steps {
        state = xorshift32(state);
        let behind = state % 128;
        let a = gov.step(behind, i as i64);
        let packed: u8 = (((a.kind as u32) << 6) | a.skip_n.min(63)) as u8;
        out.push_str(&format!("{packed:02x}"));
    }
    out.push('\n');
    print!("{out}");
}
