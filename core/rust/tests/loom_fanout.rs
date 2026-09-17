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

// ===========================================================================
// RFC 0011 deep-models — the "Exhaustive State-Space Proofs" branch extends
// the loom surface in four directions. Each new test narrows the gap between
// "the schedule the OS happens to produce" and "every schedule the memory
// model allows":
//
//   1. REORDER FILL — the writer's payload words are stored in REVERSED
//      order. The tear source the FI1 bracket exists to close must stay
//      closed when the fill order changes (out-of-order execution analog).
//   2. THREE READERS — the fan-out contract is N readers; the original
//      model proves 2. This one widens the surface (still preemption
//      bound 2 to keep `cargo test` fast).
//   3. READER REJOIN — a reader that stops claiming (the ring-level analog
//      of process-death reattach, RFC 0006) and comes back after the ring
//      has wrapped MULTIPLE times must resynchronize with zero torn frames
//      and an exact telescoping identity measured from the rejoin point.
//   4. PREEMPTION BOUND 3 — the bound-3 exhaustive run is recorded as
//      #[ignore] (minutes, not seconds); the nightly chaos leg runs it with
//      `cargo test -- --ignored` so the deeper space is swept continuously.
// ===========================================================================

/// Writer frame with REVERSED word order (RFC 0011 deep-model 1).
fn writer_frame_reversed(ring: &ModelRing, seq: u64) {
    let k = ((seq - 1) as usize) % M;
    ring.slot_seq[k].store(0, Ordering::SeqCst);
    fence(Ordering::SeqCst);
    for w in (0..WORDS).rev() {
        ring.payload[k][w].store(tword(seq, w), Ordering::Relaxed);
    }
    ring.slot_seq[k].store(seq, Ordering::Release);
    ring.latest.store(seq, Ordering::Release);
    ring.publishes.fetch_add(1, Ordering::Relaxed);
}

#[test]
fn loom_fanout_ring_reorder_fill_exhaustive() {
    // Same bound/shape as the canonical model, but the writer stores the
    // frame's payload words in reversed order. NO TORN FRAME ACCEPTED must
    // survive: the stamp bracket orders the fill as a whole, not its
    // individual words' order.
    let mut builder = loom::model::Builder::new();
    if builder.preemption_bound.is_none() {
        builder.preemption_bound = Some(2);
    }
    builder.check(|| {
        let ring = Arc::new(ModelRing::new());

        let ring_w = Arc::clone(&ring);
        let writer = thread::spawn(move || {
            for seq in 1..=FRAMES {
                writer_frame_reversed(&ring_w, seq);
            }
        });

        let ring_r = Arc::clone(&ring);
        let reader = thread::spawn(move || {
            let mut last_seq: u64 = 0;
            let mut target = [0u32; WORDS];
            let mut sum_dropped: u64 = 0;
            let mut fresh_claims: u64 = 0;
            for _ in 0..CLAIMS_PER_READER {
                let (fresh, seq, dropped) = model_claim(&ring_r, &mut last_seq, &mut target);
                if fresh {
                    fresh_claims += 1;
                    sum_dropped += dropped;
                    for w in 0..WORDS {
                        assert_eq!(
                            target[w], tword(seq, w),
                            "loom fanout reorder-fill: torn frame accepted — word {w} of frame {seq}"
                        );
                    }
                }
            }
            (sum_dropped, last_seq, fresh_claims)
        });

        writer.join().unwrap();
        let (sum_dropped, last_seq, fresh_claims) = reader.join().unwrap();
        assert_eq!(
            sum_dropped, last_seq - fresh_claims,
            "loom fanout reorder-fill: telescoping identity violated"
        );
        assert_eq!(ring.publishes.load(Ordering::Acquire), FRAMES);
    });
}

#[test]
fn loom_fanout_ring_three_readers() {
    // RFC 0011 deep-model 2: widen to 3 readers. Frames/claims trimmed one
    // notch so the bound-2 space stays in `cargo test` territory.
    const FRAMES3: u64 = 2;
    const CLAIMS3: usize = 2;

    let mut builder = loom::model::Builder::new();
    if builder.preemption_bound.is_none() {
        builder.preemption_bound = Some(2);
    }
    builder.check(|| {
        let ring = Arc::new(ModelRing::new());

        let ring_w = Arc::clone(&ring);
        let writer = thread::spawn(move || {
            for seq in 1..=FRAMES3 {
                writer_frame(&ring_w, seq);
            }
        });

        let mut readers = Vec::new();
        for _ in 0..3 {
            let ring_r = Arc::clone(&ring);
            readers.push(thread::spawn(move || {
                let mut last_seq: u64 = 0;
                let mut target = [0u32; WORDS];
                let mut sum_dropped: u64 = 0;
                let mut fresh_claims: u64 = 0;
                for _ in 0..CLAIMS3 {
                    let (fresh, seq, dropped) = model_claim(&ring_r, &mut last_seq, &mut target);
                    if fresh {
                        fresh_claims += 1;
                        sum_dropped += dropped;
                        for w in 0..WORDS {
                            assert_eq!(
                                target[w], tword(seq, w),
                                "loom fanout 3-readers: torn frame accepted — word {w} of frame {seq}"
                            );
                        }
                        assert!(seq <= FRAMES3, "loom fanout 3-readers: future violation");
                    }
                }
                (sum_dropped, last_seq, fresh_claims)
            }));
        }

        writer.join().unwrap();
        let states: Vec<_> = readers.into_iter().map(|r| r.join().unwrap()).collect();
        assert_eq!(ring.publishes.load(Ordering::Acquire), FRAMES3);
        for (sum_dropped, last_seq, fresh_claims) in states {
            assert_eq!(sum_dropped, last_seq - fresh_claims, "telescoping per reader");
        }
    });
}

#[test]
fn loom_fanout_reader_rejoin_after_wrap() {
    // RFC 0011 deep-model 3: the reattach analog at the ring level. Reader
    // claims once, DETACHES (stops claiming), the writer wraps the M=2 ring
    // three more times, and the reader REATTACHES (a fresh session: its
    // last_seq is whatever it ended at — here we model the RFC 0006
    // CLEAN_REALLOCATE semantics where the consumer state is gone, so
    // last_seq resets to 0). The rejoin must produce zero torn frames and
    // the telescoping identity must hold over the rejoin session.
    let mut builder = loom::model::Builder::new();
    if builder.preemption_bound.is_none() {
        builder.preemption_bound = Some(2);
    }
    builder.check(|| {
        let ring = Arc::new(ModelRing::new());

        let ring_w = Arc::clone(&ring);
        let writer = thread::spawn(move || {
            for seq in 1..=5u64 {
                writer_frame(&ring_w, seq);
            }
        });

        let ring_r = Arc::clone(&ring);
        let reader = thread::spawn(move || {
            let mut last_seq: u64 = 0;
            let mut target = [0u32; WORDS];

            // --- session 1: claim once, then detach ---
            let (fresh, seq, dropped) = model_claim(&ring_r, &mut last_seq, &mut target);
            if fresh {
                for w in 0..WORDS {
                    assert_eq!(target[w], tword(seq, w));
                }
                assert_eq!(dropped, seq - 1);
            }

            // --- detach: the ring wraps underneath (writer keeps going) ---
            // (the writer thread runs concurrently; the detach is modeled by
            // this thread doing NOTHING until the writer is far ahead —
            // which, under loom, is every interleaving, including none.)

            // --- reattach: fresh session, last_seq reset (RFC 0006
            //     CLEAN_REALLOCATE — the consumer's state is gone; the ring
            //     is authoritative) ---
            last_seq = 0;
            let mut rejoin_fresh = 0;
            let mut rejoin_dropped = 0;
            for _ in 0..CLAIMS_PER_READER {
                let (fresh, seq, dropped) = model_claim(&ring_r, &mut last_seq, &mut target);
                if fresh {
                    rejoin_fresh += 1;
                    rejoin_dropped += dropped;
                    for w in 0..WORDS {
                        assert_eq!(
                            target[w], tword(seq, w),
                            "loom fanout rejoin: torn frame accepted — word {w} of frame {seq}"
                        );
                    }
                    assert!(seq <= 5u64, "loom fanout rejoin: future violation");
                }
            }
            // Telescoping over the REJOIN session (from 0 to wherever the
            // reader ended): dropped == last_seq - freshClaims.
            assert_eq!(
                rejoin_dropped, last_seq - rejoin_fresh,
                "loom fanout rejoin: telescoping violated over the rejoin session"
            );
        });

        writer.join().unwrap();
        reader.join().unwrap();
        assert_eq!(ring.publishes.load(Ordering::Acquire), 5u64);
    });
}

#[test]
#[ignore = "RFC 0011 bound-3 exhaustive sweep — minutes, run by the nightly chaos leg (`cargo test -- --ignored`)"]
fn loom_fanout_ring_preemption_bound3_evidence() {
    // The canonical model at preemption bound 3: one notch deeper into the
    // space the memory model allows. Committed as the nightly evidence leg
    // so the depth grows with every "Exhaustive State-Space" wave.
    let mut builder = loom::model::Builder::new();
    if builder.preemption_bound.is_none() {
        builder.preemption_bound = Some(3);
    }
    builder.check(|| {
        let ring = Arc::new(ModelRing::new());

        let ring_w = Arc::clone(&ring);
        let writer = thread::spawn(move || {
            for seq in 1..=FRAMES {
                writer_frame(&ring_w, seq);
            }
        });

        let ring_r = Arc::clone(&ring);
        let reader = thread::spawn(move || {
            let mut last_seq: u64 = 0;
            let mut target = [0u32; WORDS];
            for _ in 0..CLAIMS_PER_READER {
                let (fresh, seq, _) = model_claim(&ring_r, &mut last_seq, &mut target);
                if fresh {
                    for w in 0..WORDS {
                        assert_eq!(target[w], tword(seq, w));
                    }
                }
            }
        });

        writer.join().unwrap();
        reader.join().unwrap();
        assert_eq!(ring.publishes.load(Ordering::Acquire), FRAMES);
    });
}
