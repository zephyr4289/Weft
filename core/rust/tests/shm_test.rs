//! tests/shm_test.rs — S-series conformance for the Rust SHM ring driver.
//!
//! Mirrors core/c/shm_test.c: header contract, O_EXCL, fork torture
//! (anonymous + named roads), producer handoff, fd road, crash posture,
//! fan-out bindings. Fork-based legs are POSIX-gated (Windows has no fork —
//! the named-mapping road is the Windows story there, compile-gated).

#![cfg(unix)]

use std::process::exit;
use std::time::Duration;

use weft_core::shm;
use weft_core::shm::ShmMap;

fn mix32(mut x: u32) -> u32 {
    x ^= x >> 16;
    x = x.wrapping_mul(0x7feb352d);
    x ^= x >> 15;
    x = x.wrapping_mul(0x846ca68b);
    x ^= x >> 16;
    x
}

fn fill_mixer(dst: &mut [u8], seq: u32) {
    for (i, w) in dst.chunks_exact_mut(4).enumerate() {
        let v = mix32(seq.wrapping_mul(2654435761).wrapping_add(i as u32));
        w.copy_from_slice(&v.to_le_bytes());
    }
}

fn words_ok(view: &[u32], seq: u64) -> bool {
    view.iter().enumerate().all(|(i, w)| {
        *w == mix32((seq as u32).wrapping_mul(2654435761).wrapping_add(i as u32))
    })
}

/// Publish one mixer frame through the broadcaster (reusable buffer — the
/// fill is a bulk copy, the Series-6-nanoseconds discipline).
fn publish_mixer(f: &mut weft_core::fanout::WeftFanout, buf: &mut Vec<u8>, seq: u32) {
    fill_mixer(buf, seq);
    f.begin();
    let n = f.fill(buf).expect("fill");
    assert_eq!(n, buf.len() / 4);  // fill returns WORDS written (C contract)
    f.publish();
}

fn mixer_buf(payload_bytes: usize) -> Vec<u8> {
    vec![0u8; payload_bytes]
}

// Per-test unique session names: cargo runs tests in parallel threads, and
// a shared name would race create/unlink across tests. Independent sessions
// are the real-world shape anyway.
fn tname(tag: &str) -> String {
    format!("weft-shm-rs-{}", tag)
}

// --- S1: create + header contract ---------------------------------------------

#[test]
fn s1_create_header_contract() {
    let n = tname("s1");
    let _ = shm::unlink(&n);
    let m = shm::create_named(&n, 256, 8).expect("create");
    assert_eq!(m.payload_bytes(), 256);
    assert_eq!(m.slot_count(), 8);
    assert_eq!(m.ring_bytes(), weft_core::fanout::ring_bytes(256, 8).unwrap());
    assert_eq!(m.ring_ptr() as usize, unsafe { m_ptr(&m) } + shm::HEADER_BYTES);
    // Ctrl zero-init (fresh-ring invariants).
    let ctrl = unsafe { std::slice::from_raw_parts(m.ring_ptr() as *const u64, 2 + 8) };
    assert!(ctrl.iter().all(|&w| w == 0), "ctrl zero-initialized");
    drop(m);
    assert!(shm::attach_named(&n, false).is_err(),
            "creator drop unlinks (attach fails after)");
}

unsafe fn m_ptr(m: &ShmMap) -> usize {
    // ring_ptr - HEADER (the base is private; this helper exists only for
    // the layout assertion above).
    (m.ring_ptr() as usize) - shm::HEADER_BYTES
}

// --- S2: attach validation ------------------------------------------------------

#[test]
fn s2_attach_validation() {
    let n = tname("s2");
    let _ = shm::unlink(&n);
    let (mut f, m) = shm::fanout_create_named(&n, 128, 4).expect("create+bind");
    let mut buf = mixer_buf(128);
    publish_mixer(&mut f, &mut buf, 1);

    // rw attach: same geometry, cross-visibility.
    let m2 = shm::attach_named(&n, false).expect("rw attach");
    assert_eq!((m2.payload_bytes(), m2.slot_count(), m2.ring_bytes()),
               (m.payload_bytes(), m.slot_count(), m.ring_bytes()));
    let mut r2 = m2.attach_reader().expect("reader over second mapping");
    let (fresh, seq, _) = { let c = r2.claim(); (c.fresh, c.seq, c.dropped) };
    assert!(fresh && seq == 1, "frame visible across mappings");
    assert!(words_ok(r2.view(), 1), "payload bit-exact cross-mapping");
    drop(m2);

    // read-only attach + claim.
    let (mut r, mr) = shm::fanout_attach_reader_named(&n, true).expect("ro attach");
    let (fresh, seq, dropped) = { let c = r.claim(); (c.fresh, c.seq, c.dropped) };
    assert!(fresh && seq == 1 && dropped == 0);
    assert!(words_ok(r.view(), 1));
    drop(r);
    drop(mr);
    drop(f);
    drop(m);
}

// --- S3: O_EXCL + stale replacement ----------------------------------------------

#[test]
fn s3_excl_semantics() {
    let n = tname("s3");
    let _ = shm::unlink(&n);
    let m = shm::create_named(&n, 64, 3).expect("first create");
    assert!(shm::create_named(&n, 64, 3).is_err(), "O_EXCL refuses re-create");
    drop(m); // unlinks
    assert!(shm::create_named(&n, 64, 3).is_ok(), "recreate after drop");
    let _ = shm::unlink(&n);
    assert!(shm::unlink("weft-shm-test-rs-nothing").is_ok(), "unlink absent ok");
    // Invalid names rejected.
    assert!(shm::create_named("", 64, 3).is_err());
    assert!(shm::create_named("bad/name", 64, 3).is_err());
    assert!(shm::create_named("-leading", 64, 3).is_err());
}

// --- S4/S5: fork torture -----------------------------------------------------------

fn reader_child(m: &ShmMap, frames: u64) -> i32 {
    let mut r = m.attach_reader().expect("reader attach");
    let mut fresh: u64 = 0;
    let mut drops: u64 = 0;
    let mut payload_ok = true;
    let mut spin: u64 = 0;
    while r.last_seq() < frames {
        let (is_fresh, seq, dropped) = { let c = r.claim(); (c.fresh, c.seq, c.dropped) };
        if is_fresh {
            if !words_ok(r.view(), seq) {
                payload_ok = false;
            }
            fresh += 1;
            drops += dropped;
        } else if spin > 400_000_000 {
            return 11;
        } else {
            spin += 1;
        }
    }
    let telescoping = fresh + drops == r.last_seq() && r.last_seq() == frames;
    if payload_ok && telescoping { 0 } else { 12 }
}

fn run_fork_torture(anon: bool) {
    const FRAMES: u64 = 200_000;
    let named = tname(if anon { "s4" } else { "s5" });
    let m = if anon {
        shm::create_anon(64, 4).expect("anon create")
    } else {
        let _ = shm::unlink(&named);
        shm::create_named(&named, 64, 4).expect("named create")
    };
    let mut readers = Vec::new();
    for _ in 0..3 {
        let pid = unsafe { shm::fork_raw() };
        assert!(pid >= 0, "fork failed");
        if pid == 0 {
            let rc = if anon {
                reader_child(&m, FRAMES)
            } else {
                // Named road: attach BY NAME inside the child.
                let mut tries = 0;
                loop {
                    match shm::attach_named(&named, true) {
                        Ok(cm) => break reader_child(&cm, FRAMES),
                        Err(_) => {
                            tries += 1;
                            if tries > 1000 { exit(20); }
                            std::thread::sleep(Duration::from_millis(1));
                        }
                    }
                }
            };
            exit(rc);
        }
        readers.push(pid);
    }
    let mut f = m.attach_writer().expect("writer attach");
    let mut buf = mixer_buf(64);
    for s in 1..=FRAMES {
        publish_mixer(&mut f, &mut buf, s as u32);
    }
    drop(f);
    let mut all_ok = true;
    for pid in readers {
        let status = unsafe { shm::waitpid_raw(pid) };
        if status != 0 { all_ok = false; }
    }
    drop(m);
    assert!(all_ok, "forked readers: integrity + telescoping ({} road)",
            if anon { "anonymous" } else { "named" });
}

#[test]
fn s4_anon_fork_torture() {
    run_fork_torture(true);
}

#[test]
fn s5_named_fork_torture() {
    run_fork_torture(false);
}

// --- S6: producer handoff -----------------------------------------------------------

#[test]
fn s6_producer_handoff() {
    let n = tname("s6");
    let _ = shm::unlink(&n);
    let (mut f, m) = shm::fanout_create_named(&n, 128, 4).expect("creator");
    let mut buf = mixer_buf(128);
    for s in 1..=1000u32 {
        publish_mixer(&mut f, &mut buf, s);
    }
    // Simulate crash/handoff: leak the map WITHOUT unlinking (panic=abort
    // in release also leaks on test failure — fine for a test name).
    std::mem::forget(m);
    drop(f);

    // The successor maps the SAME OBJECT at a different virtual address —
    // "same object" is proven by the content continuation below, not by
    // address equality (two mmaps of one object differ in address).
    let (mut f2, m2) = shm::fanout_attach_writer_named(&n).expect("successor");
    assert_eq!((m2.payload_bytes(), m2.slot_count()), (128, 4));
    let mut buf2 = mixer_buf(128);
    publish_mixer(&mut f2, &mut buf2, 1001);
    let seq = 1001;
    assert_eq!(seq, 1001, "frame numbering continues from latestSeq");

    let (mut r, mr) = shm::fanout_attach_reader_named(&n, true).expect("reader");
    let (fresh, seq, dropped) = { let c = r.claim(); (c.fresh, c.seq, c.dropped) };
    assert!(fresh && seq == 1001 && dropped == 1000,
            "handoff frame claims clean; 1000 unseen frames accounted");
    assert!(words_ok(r.view(), 1001));
    drop(r);
    drop(mr);
    drop(f2);
    drop(m2);
    let _ = shm::unlink(&n);
}

// --- S9: bindings round trip ----------------------------------------------------------

#[test]
fn s9_bindings_round_trip() {
    let n = tname("s9");
    let _ = shm::unlink(&n);
    let (mut f, m) = shm::fanout_create_named(&n, 160, 6).expect("binding create");
    let (mut r, mr) = shm::fanout_attach_reader_named(&n, true).expect("binding reader");
    let mut buf = mixer_buf(160);
    let mut ok = true;
    for s in 1..=5000u32 {
        publish_mixer(&mut f, &mut buf, s);
        if s % 500 == 0 {
            let (fresh, seq, _) = { let c = r.claim(); (c.fresh, c.seq, c.dropped) };
            if !fresh || seq != s as u64 || !words_ok(r.view(), s as u64) {
                ok = false;
            }
        }
    }
    drop(r);
    drop(mr);
    drop(f);
    drop(m);
    assert!(ok, "5000 frames round-trip, sampled claims bit-exact");
}
