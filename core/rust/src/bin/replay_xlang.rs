//! replay_xlang — RFC 0019 cross-language replay hash-log emitter.
//!
//! Folds the deterministic RFC-0019 fixture scenario (the normative
//! grammar in fixtures/xlang-replay/run.sh — xorshift32-seeded, the
//! repo's canonical 04-LITMUS §0.2 generator) and prints the per-step
//! u64 state hashes as one lowercase-hex line (16 hex digits per step,
//! no separators, one trailing newline) — byte-identical to
//! core/c/replay_runner.c, fixtures/xlang-replay/replay_emitter.mjs, and
//! the Kotlin/Swift/Dart VM emitters. fixtures/xlang-replay/run.sh
//! byte-compares them all.
//!
//! The scenario emitter below mirrors the fold (same shadow rules) so
//! every CLAIM carries the seq the model will reconstruct — a mismatch
//! would be a Disagree, which is a hard failure (rule 3: never silent).
//!
//! Usage: replay_xlang [STEPS] [SEED]

use weft_core::replay::{replay_init, replay_step, ReplayResult, WeftReplayState};

fn xorshift32(mut x: u32) -> u32 {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x
}

/// Scenario mirror state (RFC-0019 fixture grammar): the shadow
/// bookkeeping that decides each event's kind and data.
struct Scen {
    latest: u32,
    w_work: u32,
    r_work: u32,
    epoch: u32,
    revoked: bool,
    seq: u32,
    bufseq: [u32; 3],
}

/// Generate the next event `(kind, aux, data)`, advancing the mirror.
fn scen_next(c: &mut Scen, state: &mut u32) -> (u16, u16, u32) {
    *state = xorshift32(*state);
    let u = *state;
    let op = u & 15;
    if op < 7 {
        c.seq = c.seq.wrapping_add(1);
        let len = (u >> 4) % 1024;
        if !c.revoked {
            c.bufseq[c.w_work as usize] = c.seq;
            let old = c.latest;
            c.latest = c.w_work;
            c.w_work = old;
            (1, len as u16, c.seq) // PUBLISH: aux = payload_len
        } else {
            c.epoch = c.epoch.wrapping_add(1);
            (3, (c.epoch & 0xffff) as u16, c.seq) // DROP: aux = epoch at ACK
        }
    } else if op < 12 {
        let data = c.bufseq[c.latest as usize];
        let mine = c.latest;
        c.latest = c.r_work;
        c.r_work = mine;
        (2, 0, data) // CLAIM
    } else if op == 12 {
        if !c.revoked {
            c.revoked = true;
            (4, 0, c.epoch) // REVOKE: data = pre-revoke epoch
        } else {
            (5, 0, c.epoch) // ACK
        }
    } else if op == 13 {
        if c.revoked {
            c.revoked = false;
            (5, 0, c.epoch) // ACK
        } else {
            (6, 0, (u >> 4) % 8) // STALL: data = attempts
        }
    } else if op == 14 {
        (7, 0, c.seq) // TEAR
    } else {
        (8, 0, c.seq) // CANARY_FAIL
    }
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

    let mut scen = Scen {
        latest: 0,
        w_work: 1,
        r_work: 2,
        epoch: 0,
        revoked: false,
        seq: 0,
        bufseq: [0; 3],
    };
    let mut state = seed;
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    let mut out = String::with_capacity(steps as usize * 16 + 1);
    for i in 0..steps {
        let (kind, aux, data) = scen_next(&mut scen, &mut state);
        if replay_step(&mut s, kind, aux, data) != ReplayResult::Ok {
            eprintln!("replay_xlang: fold disagreement at step {i}");
            std::process::exit(1);
        }
        out.push_str(&format!("{:016x}", s.hash));
    }
    out.push('\n');
    print!("{out}");
}
