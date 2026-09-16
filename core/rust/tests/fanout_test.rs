//! F-series conformance for the Rust fan-out ring — mirrors
//! `packages/core/test/fanout.test.ts` (TS) and `core/c/fanout_test.c`,
//! so all three ports are pinned by the same contract, plus the
//! multithreaded torture gate (1 writer × 4 readers, every fresh claim
//! word-validated).
//!
//! Frames for the torture run are overridable: `WEFT_FANOUT_FRAMES=1000000
//! cargo test --release` (the committed evidence log uses 10^6 in release;
//! the debug default is smaller).

use weft_core::fanout::{FanoutClaim, WeftFanout, ring_bytes};
use weft_core::mix32;

fn tword(seq: u32, w: u32) -> u32 {
    mix32(seq.wrapping_mul(2654435761).wrapping_add(w))
}

fn fill_frame(f: &mut WeftFanout, seq: u32, words: usize) {
    let mut buf = Vec::with_capacity(words * 4);
    for w in 0..words {
        buf.extend_from_slice(&tword(seq, w as u32).to_ne_bytes());
    }
    f.begin();
    assert_eq!(f.fill(&buf), Some(words));
}

fn expect_frame(view: &[u32], seq: u32) {
    for (w, v) in view.iter().enumerate() {
        assert_eq!(*v, tword(seq, w as u32), "word {w} of frame {seq} intact");
    }
}

#[test]
fn f1_geometry_validation() {
    assert!(ring_bytes(0, 4).is_none(), "payload_bytes=0 rejected");
    assert!(ring_bytes(6, 4).is_none(), "payload_bytes%4!=0 rejected");
    assert!(ring_bytes(256, 1).is_none(), "slot_count<2 rejected");
    assert!(ring_bytes(256, 65).is_none(), "slot_count>64 rejected");
    assert_eq!(ring_bytes(256, 4), Some(16 + 8 * 4 + 4 * 256), "ring_bytes formula (TS parity)");
    assert!(WeftFanout::new(256, 4).is_some(), "valid geometry accepted");
}

#[test]
fn f2_roundtrip() {
    let mut f = WeftFanout::new(256, 4).unwrap();
    let mut r = f.create_reader();
    let c = r.claim();
    assert_eq!(*c, FanoutClaim { fresh: false, seq: 0, dropped: 0 }, "claim before publish: null frame");
    fill_frame(&mut f, 1, 64);
    assert_eq!(f.publish(), 1);
    let c = r.claim();
    assert!(c.fresh && c.seq == 1 && c.dropped == 0, "first publish: fresh, dropped=0");
    expect_frame(r.view(), 1);
}

#[test]
fn f3_drop_accounting() {
    let mut f = WeftFanout::new(256, 4).unwrap();
    let mut r = f.create_reader();
    // Standalone ring: the FIRST claim ever, after four unseen publishes —
    // the null baseline (seq 0) telescopes to dropped = 4 - 0 - 1 = 3.
    for seq in 1..=4u32 {
        fill_frame(&mut f, seq, 64);
        f.publish();
    }
    let c = r.claim();
    assert!(c.fresh && c.seq == 4 && c.dropped == 3, "four unseen publishes from null: dropped=3");
    expect_frame(r.view(), 4);
}

#[test]
fn f4_telescoping_identity() {
    let mut f = WeftFanout::new(256, 4).unwrap();
    let mut r = f.create_reader();
    let mut sum_dropped = 0u64;
    let mut fresh_claims = 0u64;
    for seq in 1..=60u32 {
        fill_frame(&mut f, seq, 64);
        f.publish();
        if seq % 3 == 0 {
            let c = r.claim();
            if c.fresh {
                sum_dropped += c.dropped;
                fresh_claims += 1;
            }
        }
    }
    let c = r.claim();
    if c.fresh {
        sum_dropped += c.dropped;
        fresh_claims += 1;
    }
    assert_eq!(sum_dropped, r.last_seq() - fresh_claims, "telescoping identity exact");
    assert_eq!(r.last_seq(), 60, "converged to the last frame");
    assert_eq!(r.stats().fresh, fresh_claims, "stats agree with the loop");
}

#[test]
fn f5_graceful_skip() {
    let mut f = WeftFanout::new(256, 2).unwrap();
    let mut r = f.create_reader();
    fill_frame(&mut f, 1, 64);
    f.publish();
    let c = r.claim();
    assert!(c.fresh && c.seq == 1 && c.dropped == 0, "reader holds frame 1");
    fill_frame(&mut f, 2, 64);
    f.publish(); // slot 1
    fill_frame(&mut f, 3, 64);
    f.publish(); // slot 0 (latest)
    // Two begins without publish: the second invalidates frame 3's slot.
    f.begin(); // frame 4 -> slot 1
    f.begin(); // frame 5 -> slot 0 (frame 3's slot) — INVALIDATED
    let c = r.claim();
    assert!(!c.fresh && c.seq == 1, "mid-overwrite: keeps last consistent frame");
    assert_eq!(r.stats().skipped_mid_overwrite, 1, "skip counted, never silent");
    f.publish();
    let c = r.claim();
    assert!(c.fresh && c.seq == 5, "converges after the overwrite completes");
}

#[test]
fn f6_publish_without_begin_is_a_noop() {
    let mut f = WeftFanout::new(256, 4).unwrap();
    assert_eq!(f.publish(), 0, "publish with no begin returns 0");
    fill_frame(&mut f, 1, 64);
    assert_eq!(f.publish(), 1);
    assert_eq!(f.fill(&[0u8, 0, 0]), None, "fill len%4!=0 rejected");
    assert_eq!(f.fill_f32(&[1.0f32, 2.0]), Some(2), "fill_f32 writes f32 bits");
}

#[test]
fn f7_ring_handoff_and_bytes() {
    let mut f = WeftFanout::new(256, 4).unwrap();
    let n = 97u32;
    for seq in 1..=n {
        fill_frame(&mut f, seq, 64);
        f.publish();
    }
    // as_bytes() is the interop export: hand the bytes to another port (the
    // xlang fixture does exactly this through a file).
    let bytes = f.as_bytes().to_vec();
    let mut r = unsafe {
        weft_core::fanout::WeftFanoutReader::attach_raw(bytes.as_ptr(), bytes.len(), 256, 4).unwrap()
    };
    let c = r.claim();
    assert!(c.fresh && c.seq == n as u64 && c.dropped == (n - 1) as u64, "handoff accounting");
    expect_frame(r.view(), n);
}

#[test]
fn f8_debug_stats() {
    let mut f = WeftFanout::new(256, 4).unwrap();
    for seq in 1..=4u32 {
        fill_frame(&mut f, seq, 64);
        f.publish();
    }
    let d = f.debug_stats();
    assert_eq!((d.latest_seq, d.publishes, d.slot_count), (4, 4, 4));
    assert!(d.slot_stamps.iter().all(|&s| s > 0 && s <= 4), "slot stamps populated and in range");
}

// ---------------------------------------------------------------------------
// Torture — the concurrency gate (mirrors core/c/fanout_runner.c)
// ---------------------------------------------------------------------------

#[test]
fn torture_concurrent_readers_zero_torn_claims() {
    let frames: u64 = std::env::var("WEFT_FANOUT_FRAMES")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(200_000);
    const WORDS: usize = 64; // 256-byte payloads
    let words = WORDS as u32;
    let mut f = WeftFanout::new(WORDS * 4, 4).unwrap();
    let mut readers: Vec<_> = (0..4).map(|_| f.create_reader()).collect();

    let writer = std::thread::spawn(move || {
        let mut buf = vec![0u8; WORDS * 4];
        for seq in 1..=frames as u32 {
            for w in 0..words as usize {
                buf[w * 4..w * 4 + 4].copy_from_slice(&tword(seq, w as u32).to_ne_bytes());
            }
            f.begin();
            f.fill(&buf);
            f.publish();
        }
        f
    });

    let handles: Vec<_> = readers
        .into_iter()
        .enumerate()
        .map(|(i, mut r)| {
            std::thread::spawn(move || {
                let mut violations = 0u64;
                let mut sum_dropped = 0u64;
                let mut fresh_claims = 0u64;
                let mut claims = 0u64;
                while r.last_seq() != frames as u64 && claims < 2_000_000_000 {
                    claims += 1;
                    let c = *r.claim();
                    if c.fresh {
                        fresh_claims += 1;
                        sum_dropped += c.dropped;
                        let seq = c.seq as u32;
                        if r.view().iter().enumerate().any(|(w, v)| *v != tword(seq, w as u32)) {
                            violations += 1;
                        }
                    }
                }
                let converged = r.last_seq() == frames as u64;
                let st = r.stats();
                (i, violations, sum_dropped, fresh_claims, claims, converged, st.skipped_mid_overwrite, st.torn_exhausted, r.last_seq())
            })
        })
        .collect();

    let f = writer.join().unwrap();
    let results: Vec<_> = handles.into_iter().map(|h| h.join().unwrap()).collect();

    let d = f.debug_stats();
    assert_eq!(d.publishes, frames, "every frame published exactly once");
    for (i, violations, sum_dropped, fresh_claims, _claims, converged, _skips, _exhausted, last_seq) in results {
        assert_eq!(violations, 0, "reader {i}: zero payload integrity violations");
        assert_eq!(sum_dropped, last_seq - fresh_claims, "reader {i}: telescoping identity exact");
        assert!(converged && last_seq == frames, "reader {i}: converged to the final frame");
        println!(
            "reader {i}: fresh={fresh_claims} dropped={sum_dropped} last_seq={last_seq} identity=OK"
        );
    }
    println!("torture: frames={frames} slots=4 words={words} readers=4 verdict=PASS");
}
