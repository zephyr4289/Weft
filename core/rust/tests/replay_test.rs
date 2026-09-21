//! RFC-0019 replay fold conformance (R-series), Rust.
//!
//! Mirrors `packages/core/test/replay.test.ts` and
//! `core/c/weft_replay_test.c` (R1–R6, R7, R8, R9 local; the xlang
//! hash-log surface is fixtures/xlang-replay/, driven by the
//! `replay_xlang` bin). The pinned parity vectors are the CONTRACT:
//! init 0x8a769a0111cf3af3 and the 100k soak 0x26beb484733ecde0 must
//! match the C reference exactly.

use weft_core::replay::{
    replay_fnv1a, replay_fold, replay_init, replay_serialize, replay_step, ReplayResult,
    WeftReplayState, WEFT_REPLAY_CHECKPOINT,
};

/// xorshift32 — 04-LITMUS §0.2, the repo's canonical deterministic RNG
/// (the fixture scenario's generator).
fn xorshift32(mut x: u32) -> u32 {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x
}

/// The shared deterministic scenario (RFC-0019 fixture grammar — mirrored
/// EXACTLY by core/c/replay_runner.c and every other port's emitter).
struct Scen {
    latest: u32,
    w_work: u32,
    r_work: u32,
    epoch: u32,
    revoked: bool,
    seq: u32,
    bufseq: [u32; 3],
}

fn scen_init() -> Scen {
    Scen {
        latest: 0,
        w_work: 1,
        r_work: 2,
        epoch: 0,
        revoked: false,
        seq: 0,
        bufseq: [0; 3],
    }
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
            (1, len as u16, c.seq)
        } else {
            c.epoch = c.epoch.wrapping_add(1);
            (3, (c.epoch & 0xffff) as u16, c.seq)
        }
    } else if op < 12 {
        let data = c.bufseq[c.latest as usize];
        let mine = c.latest;
        c.latest = c.r_work;
        c.r_work = mine;
        (2, 0, data)
    } else if op == 12 {
        if !c.revoked {
            c.revoked = true;
            (4, 0, c.epoch)
        } else {
            (5, 0, c.epoch)
        }
    } else if op == 13 {
        if c.revoked {
            c.revoked = false;
            (5, 0, c.epoch)
        } else {
            (6, 0, (u >> 4) % 8)
        }
    } else if op == 14 {
        (7, 0, c.seq)
    } else {
        (8, 0, c.seq)
    }
}

#[test]
fn r1_init_state_and_pinned_hash() {
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    assert_eq!(s.latest, 0, "init indices per 02 §1");
    assert_eq!(s.w_work, 1);
    assert_eq!(s.r_work, 2);
    assert_eq!(s.hash, 0x8a769a0111cf3af3, "init hash pinned (parity vector)");
}

#[test]
fn r2_publish_exchange() {
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    assert_eq!(replay_step(&mut s, 1, 64, 5), ReplayResult::Ok, "publish applied");
    assert_eq!(
        (s.buf[1].seq, s.buf[1].len, s.buf[1].ver),
        (5, 64, 1),
        "buf[w_work=1] = (5, 64, v1)"
    );
    assert_eq!(s.latest, 1, "exchange: latest = w_work");
    assert_eq!(s.w_work, 0, "exchange: w_work = old latest");
    assert_eq!(s.t_publish, 1, "telemetry +1");
    assert_eq!(s.t_wsteps, 1);
    // second publish takes buf 0 (the old latest) — the triad rotates
    assert_eq!(replay_step(&mut s, 1, 16, 6), ReplayResult::Ok);
    assert_eq!(s.latest, 0);
    assert_eq!(s.w_work, 1);
    assert_eq!(s.buf[0].seq, 6, "second publish rotates the triad");
}

#[test]
fn r3_claim_disagreement_leaves_state_untouched() {
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    replay_step(&mut s, 1, 64, 5);
    assert_eq!(replay_step(&mut s, 2, 0, 5), ReplayResult::Ok, "claim of seq 5 ok");
    assert_eq!(s.r_work, 1, "claim exchange: r_work = old latest");
    assert_eq!(s.latest, 2);
    let before = s.hash; // AFTER the good claim — the baseline to defend
    assert_eq!(replay_step(&mut s, 2, 0, 99), ReplayResult::Disagree, "bad claim refused");
    assert_eq!(s.hash, before, "disagreement left the fold intact");
    assert_eq!(s.t_claim, 1, "the refused claim was not counted");
}

#[test]
fn r4_null_frame_claim() {
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    // claim before any publish: the null frame (rule 1) has seq 0
    assert_eq!(replay_step(&mut s, 2, 0, 0), ReplayResult::Ok);
    assert_eq!(s.buf[0].seq, 0, "null frame modeled len 0 (rule 1)");
    assert_eq!(s.buf[0].len, 0);
}

#[test]
fn r5_revocation_window() {
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    replay_step(&mut s, 1, 8, 1);
    replay_step(&mut s, 4, 0, 0);
    assert_eq!(s.revoked, 1, "revoke sets flag");
    replay_step(&mut s, 3, 3, 9); // epoch at ACK = 3
    assert_eq!(s.epoch, 3, "drop carries epoch-at-ack");
    assert_eq!(s.t_drop, 1);
    replay_step(&mut s, 5, 0, 3);
    assert_eq!(s.epoch, 3, "ack syncs epoch");
    assert_eq!(s.revoked, 1, "ack does not rebind");
    replay_step(&mut s, 1, 8, 2);
    assert_eq!(s.revoked, 0, "publish implies rebind (rule 2)");
}

#[test]
fn r6_serialize_layout() {
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    let mut ser = [0u8; 111];
    replay_serialize(&s, &mut ser);
    assert_eq!(ser.len(), 111, "111-byte serialization");
    assert_eq!(ser[0], 0, "latest at 0");
    assert_eq!(ser[4], 0, "epoch at 4");
    assert_eq!(ser[8], 1, "w_work LE at 8");
    assert_eq!(ser[12], 2, "r_work LE at 12");
    assert_eq!(ser[16], 0, "revoked byte at 16");
    // buf[i].ver = 1, a LE u16 at 25 + 10*i
    assert_eq!(ser[25], 1);
    assert_eq!(ser[26], 0);
    assert_eq!(ser[35], 1);
    assert_eq!(ser[45], 1);
    assert_eq!(replay_fnv1a(&ser), s.hash, "hash is FNV-1a over the serialization");
}

#[test]
fn r7_checkpoint_jump_equivalence() {
    // fold 2000 scenario events; jump(k) (checkpoint struct-copy + refold)
    // must equal fold(k) across the sweep.
    let mut c = scen_init();
    let mut state: u32 = 0x00C0_FFEE;
    let mut evs: Vec<(u16, u16, u32)> = Vec::with_capacity(2000);
    for _ in 0..2000 {
        evs.push(scen_next(&mut c, &mut state));
    }
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    let mut hashes: Vec<u64> = Vec::with_capacity(2000);
    assert_eq!(replay_fold(&mut s, &evs, Some(&mut hashes)), ReplayResult::Ok, "fold 2000");

    let ckpt = WEFT_REPLAY_CHECKPOINT as usize;
    let mut cps: Vec<WeftReplayState> = Vec::new();
    let mut t = WeftReplayState::default();
    replay_init(&mut t);
    for i in 0..2000 {
        if i % ckpt == 0 {
            cps.push(t.clone()); // plain struct copy — Law 2
        }
        let (kind, aux, data) = evs[i];
        replay_step(&mut t, kind, aux, data);
    }
    for k in [0usize, 1, 63, 64, 65, 127, 128, 999, 1921, 1999] {
        let mut j = cps[k / ckpt].clone();
        let start = (k / ckpt) * ckpt;
        for i in start..k {
            let (kind, aux, data) = evs[i];
            replay_step(&mut j, kind, aux, data);
        }
        // hashes[i] is the hash AFTER event i (0-based): compare with
        // hashes[k-1], or the init hash for k=0.
        let want = if k == 0 { cps[0].hash } else { hashes[k - 1] };
        assert_eq!(j.hash, want, "jump({k}) hash == fold({k}) hash");
    }
}

#[test]
fn r8_soak_100k_pinned() {
    let mut c = scen_init();
    let mut state: u32 = 0x00C0_FFEE;
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    for i in 0..100_000u32 {
        let (kind, aux, data) = scen_next(&mut c, &mut state);
        assert_eq!(
            replay_step(&mut s, kind, aux, data),
            ReplayResult::Ok,
            "soak fold clean at step {i}"
        );
    }
    assert_eq!(s.step, 100_000, "100k events folded");
    // Pinned cross-port parity vector: every runtime folding this scenario
    // MUST land on exactly this final hash.
    assert_eq!(s.hash, 0x26beb484733ecde0, "100k soak final hash pinned");
    assert!(s.t_claim > 1000, "scenario exercised the reader side");
    assert!(s.t_publish > 1000, "scenario exercised the writer side");
}

#[test]
fn r9_unknown_kind_refused() {
    let mut s = WeftReplayState::default();
    replay_init(&mut s);
    assert_eq!(replay_step(&mut s, 99, 0, 0), ReplayResult::BadKind);
}

#[test]
fn fnv1a_reference_vectors() {
    let empty: [u8; 0] = [];
    assert_eq!(replay_fnv1a(&empty), 0xcbf29ce484222325);
    assert_eq!(replay_fnv1a(b"a"), 0xaf63dc4c8601ec8c);
}
