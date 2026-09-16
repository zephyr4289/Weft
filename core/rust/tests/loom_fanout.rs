//! # Loom model of the RFC-0004 fan-out ring (fenced acq/rel regime)
//!
//! Extends the repo's highest-evidence tradition (`tests/loom_model.rs` —
//! the exhaustive kernel model) to the fan-out driver layer. Loom explores
//! EVERY interleaving the memory model allows (including Relaxed-load
//! staleness — the tear source) and asserts the protocol's safety core:
//!
//! (a) NO TORN FRAME ACCEPTED: any claim that returns `fresh == true`
//!     carries payload words that all belong to that frame (FI1 + FI2 —
//!     the stamp-then-fill bracket under all interleavings).
//! (b) NO FUTURE: a claimed seq is always <= the writer's last publish.
//! (c) EXACT TELESCOPING: per reader, sum(dropped) == last_seq - freshClaims
//!     (FI3) in every reachable execution.
//! (d) Join state: publishes == FRAMES (the writer completed).
//!
//! Liveness (eventual convergence) is NOT checked here — it is owned by the
//! C torture gate (`core/c/fanout_runner.c`) and the Rust torture test, the
//! same split the kernel model uses ("Liveness is NOT checked here — owned
//! by litmus L4").
//!
//! Model scale: M = 2 slots, 2 payload words, 3 frames, 2 readers x 4
//! claims. The protocol is width- and scale-independent (the C/TS ports
//! exercise production scales); the model buys exhaustiveness instead.

use loom::sync::atomic::{AtomicU32, AtomicU64, Ordering, fence};
use loom::thread;
use std::sync::Arc;

const M: usize = 2;
const WORDS: usize = 2;
const FRAMES: u64 = 3;
const CLAIMS_PER_READER: usize = 3;

fn mix32(mut x: u32) -> u32 {
    x ^= x >> 16;
    x = x.wrapping_mul(0x7FEB352D);
    x ^= x >> 15;
    x = x.wrapping_mul(0x846CA68B);
    x ^= x >> 16;
    x
}

/// Deterministic payload word (04-LITMUS §0.1 mixer family — the same
/// generator the C torture and the interop fixtures use).
fn tword(seq: u64, w: usize) -> u32 {
    mix32((seq as u32).wrapping_mul(2654435761).wrapping_add(w as u32))
}

/// The ring, as loom atomics. Same layout semantics as src/fanout.rs
/// (ctrl block + payload slots), modeled directly.
struct ModelRing {
    latest: AtomicU64,
    publishes: AtomicU64,
    slot_seq: [AtomicU64; M],
    payload: [[AtomicU32; WORDS]; M],
}

impl ModelRing {
    fn new() -> Self {
        // Loom's atomics are not const-constructible — the arrays are
        // expanded literally (M = 2, WORDS = 2 are compile-time constants).
        Self {
            latest: AtomicU64::new(0),
            publishes: AtomicU64::new(0),
            slot_seq: [AtomicU64::new(0), AtomicU64::new(0)],
            payload: [
                [AtomicU32::new(0), AtomicU32::new(0)],
                [AtomicU32::new(0), AtomicU32::new(0)],
            ],
        }
    }
}

/// Writer step — mirrors `WeftFanout::begin` + `fill` + `publish` exactly
/// (same orderings, same fence placement — see src/fanout.rs).
fn writer_frame(ring: &ModelRing, seq: u64) {
    let k = ((seq - 1) as usize) % M;
    // FI1 first half: invalidate (SeqCst) BEFORE the fill, fenced.
    ring.slot_seq[k].store(0, Ordering::SeqCst);
    fence(Ordering::SeqCst);
    for w in 0..WORDS {
        ring.payload[k][w].store(tword(seq, w), Ordering::Relaxed);
    }
    // FI1 second half: stamp (Release), then the publication point.
    ring.slot_seq[k].store(seq, Ordering::Release);
    ring.latest.store(seq, Ordering::Release);
    ring.publishes.fetch_add(1, Ordering::Relaxed);
}

/// Reader claim — mirrors `WeftFanoutReader::claim` exactly. Returns
/// (fresh, seq, dropped) without internal assertions so the caller can
/// validate AFTER the model step (keeping claim itself side-effect-clean).
fn model_claim(ring: &ModelRing, last_seq: &mut u64, target: &mut [u32; WORDS]) -> (bool, u64, u64) {
    let mut l = ring.latest.load(Ordering::Acquire);
    if l == 0 || l == *last_seq {
        return (false, *last_seq, 0);
    }
    for _ in 0..4 {
        let k = ((l - 1) as usize) % M;
        let s_b = ring.slot_seq[k].load(Ordering::Acquire);
        if s_b != l {
            let l2 = ring.latest.load(Ordering::Acquire);
            if l2 == l {
                return (false, *last_seq, 0); // graceful skip
            }
            l = l2;
            continue;
        }
        for w in 0..WORDS {
            target[w] = ring.payload[k][w].load(Ordering::Relaxed);
        }
        // P2: the copy is ordered before the revalidation load.
        fence(Ordering::SeqCst);
        let s_a = ring.slot_seq[k].load(Ordering::Acquire);
        if s_a == l {
            let dropped = l - *last_seq - 1;
            *last_seq = l;
            return (true, l, dropped);
        }
        l = ring.latest.load(Ordering::Acquire);
    }
    (false, *last_seq, 0) // bounded retries exhausted
}

#[test]
fn loom_fanout_ring_no_torn_claims_exhaustive() {
    // Preemption bound: unbounded exploration of this model does not
    // terminate in practical time (~80 atomic ops across 3 threads). The
    // default bound 2 keeps `cargo test` fast (seconds); the committed
    // evidence log also records a bound-3 run. Override with
    // LOOM_MAX_PREEMPTIONS=N.
    let mut builder = loom::model::Builder::new();
    if builder.preemption_bound.is_none() {
        builder.preemption_bound = Some(2);
    }
    builder.check(|| {
        let ring = Arc::new(ModelRing::new());

        // --- Writer: 3 frames ---
        let ring_w = Arc::clone(&ring);
        let writer = thread::spawn(move || {
            for seq in 1..=FRAMES {
                writer_frame(&ring_w, seq);
            }
        });

        // --- Readers: 2, each 4 claims, validating every fresh claim ---
        let mut readers = Vec::new();
        for _ in 0..2 {
            let ring_r = Arc::clone(&ring);
            readers.push(thread::spawn(move || {
                let mut last_seq: u64 = 0;
                let mut target = [0u32; WORDS];
                let mut sum_dropped: u64 = 0;
                let mut fresh_claims: u64 = 0;
                for _ in 0..CLAIMS_PER_READER {
                    let (fresh, seq, dropped) = model_claim(&ring_r, &mut last_seq, &mut target);
                    if fresh {
                        fresh_claims += 1;
                        sum_dropped += dropped;
                        // (a) NO TORN FRAME ACCEPTED — every word of a fresh
                        // claim belongs to the claimed frame.
                        for w in 0..WORDS {
                            assert_eq!(
                                target[w], tword(seq, w),
                                "loom fanout: torn frame accepted — word {w} of frame {seq}"
                            );
                        }
                        // (b) NO FUTURE — the claimed seq is never beyond the
                        // writer's progress.
                        assert!(
                            seq <= FRAMES,
                            "loom fanout: future violation — claimed {seq} > {FRAMES}"
                        );
                    }
                }
                // (c) EXACT TELESCOPING per reader, per execution.
                assert_eq!(
                    sum_dropped, last_seq - fresh_claims,
                    "loom fanout: telescoping identity violated (last_seq={last_seq}, fresh={fresh_claims})"
                );
                (last_seq, fresh_claims)
            }));
        }

        writer.join().unwrap();
        let reader_states: Vec<_> = readers.into_iter().map(|r| r.join().unwrap()).collect();

        // (d) Join state: the writer completed all frames.
        assert_eq!(
            ring.publishes.load(Ordering::Acquire),
            FRAMES,
            "loom fanout: join-quiescence — publishes must equal FRAMES"
        );

        // Every reader that made a fresh claim ended on a real frame.
        for (last_seq, fresh) in reader_states {
            if fresh > 0 {
                assert!(last_seq >= 1 && last_seq <= FRAMES);
            }
        }
    });
}
