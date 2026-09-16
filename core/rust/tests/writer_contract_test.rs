//! Writer-cursor contract tests — pin the C-parity publish() semantics.
//!
//! 2026-09-16: the Rust `publish()` previously re-filled the payload region
//! with the litmus pattern, silently discarding bytes written through
//! `w_write_payload` (the writer-cursor contract, RFC-0001 §4 — canonical C
//! `weft_publish` writes envelope + canary + exchange and nothing else).
//! These tests pin the restored contract: what the writer writes is what the
//! reader claims.

use weft_core::Weft;

#[test]
fn w1_publish_preserves_writer_payload() {
    let weft = Weft::new(64).unwrap();
    let payload: Vec<u8> = (0..64u32).map(|i| (0xC0 ^ i) as u8).collect();
    unsafe {
        weft.w_write_payload(payload.as_ptr(), 64).unwrap();
        weft.publish(1, 64);
    }
    weft.claim();
    let mut out = [0u8; 64];
    unsafe { weft.r_read_slice(out.as_mut_ptr(), 16, 64); }
    assert_eq!(&out[..], &payload[..], "publish must not touch writer payload");
}

#[test]
fn w2_publish_does_not_fill_untouched_tail() {
    // The stale-tail contract (same as C): bytes beyond what the writer wrote
    // keep their PREVIOUS contents across publish — no pattern fill-in.
    // Triad rotation: frame N+3 reuses frame N's buffer, so we prime THREE
    // buffers with full 0x11 writes, then write a 16-byte head into the
    // recycled buffer and assert its tail survived untouched.
    let weft = Weft::new(128).unwrap();
    let full = [0x11u8; 128];
    for seq in 1..=3u32 {
        unsafe {
            weft.w_write_payload(full.as_ptr(), 128).unwrap();
            weft.publish(seq, 128);
        }
        weft.claim();
    }
    // Frame 4 recycles frame 1's buffer (all 0x11); write only the head.
    let head = [0x22u8; 16];
    unsafe {
        weft.w_write_payload(head.as_ptr(), 16).unwrap();
        weft.publish(4, 16);
    }
    weft.claim();
    let mut out = [0u8; 128];
    unsafe { weft.r_read_slice(out.as_mut_ptr(), 16, 128); }
    assert_eq!(&out[..16], &[0x22u8; 16][..], "head = writer bytes");
    assert_eq!(&out[16..], &[0x11u8; 112][..], "tail = previous contents (C stale-tail contract)");
}

#[test]
fn w3_envelope_and_canary_still_written() {
    let weft = Weft::new(64).unwrap();
    let payload = [0xABu8; 64];
    unsafe {
        weft.w_write_payload(payload.as_ptr(), 64).unwrap();
        weft.publish(42, 64);
    }
    weft.claim();
    // Envelope: magic + version + header + seq + payload_len (little-endian).
    let mut env = [0u8; 16];
    unsafe { weft.r_read_slice(env.as_mut_ptr(), 0, 16); }
    assert_eq!(&env[0..4], b"WEFT", "magic");
    assert_eq!(u16::from_le_bytes([env[4], env[5]]), 1, "envelope version");
    assert_eq!(u16::from_le_bytes([env[6], env[7]]), 16, "header size");
    assert_eq!(u32::from_le_bytes([env[8], env[9], env[10], env[11]]), 42, "seq");
    assert_eq!(u32::from_le_bytes([env[12], env[13], env[14], env[15]]), 64, "payload_len");
    // Canary: seq as u64 LE at buf_size - 8 (read via the live pointer, the
    // same path the kernel's own litmus uses).
    let canary_ptr = weft.r_live_ptr(weft.buf_size - 8);
    let canary = unsafe { std::ptr::read_unaligned(canary_ptr as *const u64) }.to_le();
    assert_eq!(canary, 42, "canary = seq");
}
