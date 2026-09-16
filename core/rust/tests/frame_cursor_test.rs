//! FrameCursor tests (RFC-0008) — mirrors `packages/core/test/cursor.test.ts`
//! and `core/c/fanout_test.c` FC1–FC6, so the Rust port is pinned by the
//! same semantics as the TS port (including the kernel's recycled-hold
//! behavior on back-to-back claims against a quiet writer).

use weft_core::frame_cursor::FrameCursor;
use weft_core::Weft;

/// Publish one frame through the kernel's writer API (unsafe raw-pointer
/// surface — the Rust kernel's documented stance, 06 §3).
fn publish(weft: &Weft, seq: u32) {
    let payload = [(seq as u8).wrapping_add(7); 64];
    unsafe {
        weft.w_write_payload(payload.as_ptr(), 64).unwrap();
        let _ = weft.publish(seq, 64);
    }
}

#[test]
fn fc1_first_claim_is_the_baseline() {
    let weft = Weft::new(64).unwrap();
    let mut cursor = FrameCursor::new();
    let c = cursor.claim(&weft);
    assert_eq!(c.seq, 0);
    assert_eq!(c.frames_behind, 0);
    assert!(c.first, "null frame baseline: first, behind 0");
}

#[test]
fn fc2_paced_reader_sees_zero_drops() {
    let weft = Weft::new(64).unwrap();
    let mut cursor = FrameCursor::new();
    for seq in 1..=100u32 {
        publish(&weft, seq);
        let c = cursor.claim(&weft);
        assert_eq!(c.frames_behind, 0);
        assert_eq!(c.seq, seq);
    }
    assert_eq!(cursor.total_dropped, 0);
    assert_eq!(cursor.claims, 100);
}

#[test]
fn fc3_counts_frames_never_seen() {
    let weft = Weft::new(64).unwrap();
    let mut cursor = FrameCursor::new();
    publish(&weft, 1);
    let first = cursor.claim(&weft);
    assert_eq!((first.seq, first.frames_behind), (1, 0));
    for seq in 2..=10u32 {
        publish(&weft, seq);
    }
    let second = cursor.claim(&weft);
    assert_eq!((second.seq, second.frames_behind), (10, 8));
    assert_eq!(cursor.total_dropped, 8);
    // Back-to-back claim against a quiet writer: the kernel hands the reader
    // its recycled previous hold; nothing new was published, so nothing is
    // counted as dropped; the decreasing seq hits the reset rule (mirrors
    // cursor.test.ts).
    let third = cursor.claim(&weft);
    assert_eq!(third.frames_behind, 0);
    assert_eq!(cursor.total_dropped, 8);
}

#[test]
fn fc4_decreasing_seq_is_a_writer_reset() {
    let weft = Weft::new(64).unwrap();
    let mut cursor = FrameCursor::new();
    publish(&weft, 10);
    cursor.claim(&weft); // baseline: seq 10
    publish(&weft, 1); // writer reset (new epoch semantics)
    let c = cursor.claim(&weft);
    assert_eq!(c.seq, 1);
    assert_eq!(c.frames_behind, 0);
    assert_eq!(cursor.total_dropped, 0);
}

#[test]
fn fc5_reset_then_first_again() {
    let weft = Weft::new(64).unwrap();
    let mut cursor = FrameCursor::new();
    publish(&weft, 1);
    cursor.claim(&weft);
    publish(&weft, 5);
    let c = cursor.claim(&weft);
    assert_eq!(c.frames_behind, 3);
    assert_eq!(cursor.total_dropped, 3);
    cursor.reset();
    assert_eq!(cursor.claims, 0);
    let c = cursor.claim(&weft);
    assert!(c.first, "reset: next claim is first again");
    assert_eq!(c.frames_behind, 0);
}
