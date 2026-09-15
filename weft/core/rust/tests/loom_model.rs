//! # Loom model of the Triad Protocol (v2 — fused watermark via Mutex)
//!
//! v2 changes (WO-P3 T1, WO-P1-CLOSURE §2):
//! - `published_wm` is fused into the SAME loom step as the writer's release.
//!   Implemented via a `loom::sync::Mutex<(u32, u32)>` holding `(latest_idx, published_wm)`.
//!   The writer's step: lock, write (w_work, seq), unlock — both effects in one step.
//!   The reader's step: lock, read (idx, wm), write (r_work, wm_preserved), unlock —
//!   the reader preserves the watermark, only swapping the buffer index.
//! - Assertion (c): `claimed_seq <= published_wm` at claim time (strong form).
//! - Assertion (d): renamed to "join-quiescence" (WO-P3 T2).

use loom::sync::atomic::{AtomicU32, Ordering};
use loom::sync::Mutex;
use loom::thread;

const BUF_SIZE: usize = 64;
const SEQ_OFFSET: usize = 8;
const CANARY_OFFSET: usize = BUF_SIZE - 8;
const PAYLOAD_LEN: usize = BUF_SIZE - 16 - 8;
const MAX_PUBLISHED_SEQ: u32 = 3;

fn loom_pat(seq: u32, i: u32) -> u8 {
    let x = seq.wrapping_mul(2654435761).wrapping_add(i.wrapping_mul(2246822519));
    let mut y = x ^ (x >> 16);
    y = y.wrapping_mul(0x7FEB352D);
    y ^= y >> 15;
    y = y.wrapping_mul(0x846CA68B);
    y ^= y >> 16;
    (y & 0xFF) as u8
}

fn write_frame(buf: &mut [u8], seq: u32) {
    buf[SEQ_OFFSET] = (seq & 0xFF) as u8;
    buf[SEQ_OFFSET + 1] = ((seq >> 8) & 0xFF) as u8;
    buf[SEQ_OFFSET + 2] = ((seq >> 16) & 0xFF) as u8;
    buf[SEQ_OFFSET + 3] = ((seq >> 24) & 0xFF) as u8;
    for i in 0..PAYLOAD_LEN { buf[16 + i] = loom_pat(seq, i as u32); }
    buf[CANARY_OFFSET] = (seq & 0xFF) as u8;
    buf[CANARY_OFFSET + 1] = ((seq >> 8) & 0xFF) as u8;
    buf[CANARY_OFFSET + 2] = ((seq >> 16) & 0xFF) as u8;
    buf[CANARY_OFFSET + 3] = ((seq >> 24) & 0xFF) as u8;
}

fn verify_frame(buf: &[u8], expected_seq: u32) -> bool {
    let seq = (buf[SEQ_OFFSET] as u32)
        | ((buf[SEQ_OFFSET + 1] as u32) << 8)
        | ((buf[SEQ_OFFSET + 2] as u32) << 16)
        | ((buf[SEQ_OFFSET + 3] as u32) << 24);
    if seq != expected_seq { return false; }
    for i in 0..PAYLOAD_LEN {
        if buf[16 + i] != loom_pat(expected_seq, i as u32) { return false; }
    }
    let canary = (buf[CANARY_OFFSET] as u32)
        | ((buf[CANARY_OFFSET + 1] as u32) << 8)
        | ((buf[CANARY_OFFSET + 2] as u32) << 16)
        | ((buf[CANARY_OFFSET + 3] as u32) << 24);
    if canary != expected_seq { return false; }
    true
}

/// The loom model (v2). Uses a `Mutex<(u32, u32)>` to fuse the watermark update
/// into the same loom step as the writer's release.
///
/// Assertions:
/// (a) Ownership: implicit in the exchange protocol.
/// (b) No torn observation: `verify_frame` checks seq+payload+canary.
/// (c) No future: `claimed_seq <= published_wm` at claim time. The watermark
///     is fused into the same Mutex step as the writer's release — no
///     interleaving point between "release lands" and "watermark advances."
/// (d) Join-quiescence: at join, `published_wm == MAX_PUBLISHED_SEQ`.
///     (Renamed from "eventual-final" per WO-P3 T2; liveness stays with L4.)
#[test]
fn loom_triad_protocol_exhaustive_v2() {
    loom::model(|| {
        let buffers = std::sync::Arc::new(Mutex::new(vec![
            { let mut b = vec![0u8; BUF_SIZE]; write_frame(&mut b, 0); b },
            { let mut b = vec![0u8; BUF_SIZE]; write_frame(&mut b, 0); b },
            { let mut b = vec![0u8; BUF_SIZE]; write_frame(&mut b, 0); b },
        ]));

        // Fused state: (latest_idx, published_wm). Init: (0, 0).
        // The writer's step: lock, write (w_work, seq), unlock — both effects
        // in one Mutex step. The reader's step: lock, read (idx, wm), write
        // (r_work, wm_preserved), unlock — the reader preserves the watermark.
        let fused = std::sync::Arc::new(Mutex::new((0u32, 0u32))); // (latest, wm)

        let torn_count = std::sync::Arc::new(AtomicU32::new(0));

        // --- Writer: 3 publishes (seq 1, 2, 3) ---
        let buffers_w = buffers.clone();
        let fused_w = fused.clone();
        let writer = thread::spawn(move || {
            let mut w_work: u32 = 1;
            for seq in 1..=MAX_PUBLISHED_SEQ {
                // 1. Write frame to buf[w_work] (writer-private)
                {
                    let mut bufs = buffers_w.lock().unwrap();
                    write_frame(&mut bufs[w_work as usize], seq);
                }
                // 2. FUSED step: lock fused, update (latest=w_work, wm=seq), unlock.
                //    This is ONE loom step — no interleaving point between
                //    "release lands" and "watermark advances."
                //    The old (latest, wm) is returned for w_work update.
                let old_latest = {
                    let mut state = fused_w.lock().unwrap();
                    let old = state.0;
                    *state = (w_work, seq); // both effects in one step
                    old
                };
                w_work = old_latest;
            }
        });

        // --- Reader: 3 claims ---
        let buffers_r = buffers.clone();
        let fused_r = fused.clone();
        let torn_r = torn_count.clone();
        let reader = thread::spawn(move || {
            let mut r_work: u32 = 2;
            for _ in 0..3u32 {
                // 1. Claim: lock fused, read (latest, wm), write (r_work, wm_preserved).
                //    The reader preserves the watermark — it only swaps the buffer index.
                let (mine, wm_at_claim) = {
                    let mut state = fused_r.lock().unwrap();
                    let (latest, wm) = *state;
                    *state = (r_work, wm); // preserve wm, swap index only
                    (latest, wm)
                };
                r_work = mine;

                // 2. Read seq from the claimed buffer.
                let (seq, ok) = {
                    let bufs = buffers_r.lock().unwrap();
                    let buf = &bufs[mine as usize];
                    let seq = (buf[SEQ_OFFSET] as u32)
                        | ((buf[SEQ_OFFSET + 1] as u32) << 8)
                        | ((buf[SEQ_OFFSET + 2] as u32) << 16)
                        | ((buf[SEQ_OFFSET + 3] as u32) << 24);
                    let ok = verify_frame(buf, seq);
                    (seq, ok)
                };

                // Assertion (c) — STRONG form (v2): claimed_seq <= published_wm.
                // The watermark is fused into the same Mutex step as the writer's
                // release. The reader can never observe a seq the writer has not
                // yet released, across every interleaving.
                assert!(
                    seq <= wm_at_claim,
                    "loom v2: future violation — claimed seq {} > published_wm {} at claim time",
                    seq, wm_at_claim
                );

                // Sub-assertion: range check.
                assert!(
                    seq <= MAX_PUBLISHED_SEQ,
                    "loom v2: range violation — claimed seq {} > MAX {}",
                    seq, MAX_PUBLISHED_SEQ
                );

                // Assertion (b): no torn observation.
                if !ok {
                    torn_r.fetch_add(1, Ordering::Relaxed);
                }
            }
        });

        writer.join().unwrap();
        reader.join().unwrap();

        // (b) No torn observations
        assert_eq!(torn_count.load(Ordering::Acquire), 0,
            "loom v2: torn observation detected");

        // (d) Join-quiescence (WO-P3 T2): at join, published_wm must equal
        // MAX_PUBLISHED_SEQ — all publishes completed. The watermark is
        // preserved by the reader's step (never zeroed), so it can only
        // advance via the writer's fused step.
        let final_state = fused.lock().unwrap();
        let final_wm = final_state.1;
        assert_eq!(final_wm, MAX_PUBLISHED_SEQ,
            "loom v2: join-quiescence — published_wm={} (expected {}) after join",
            final_wm, MAX_PUBLISHED_SEQ);

        // Liveness (eventual-final) is NOT checked here — owned by litmus L4.
    });
}
