//! # RFC 0004 fan-out ring — Rust driver layer
//!
//! Byte-compatible with `core/ts/fanout.ts` and `core/c/fanout.{h,c}`:
//! one ring layout, three languages, attachable across FFI/SAB boundaries
//! (see `fixtures/xlang-fanout/`). The Triad kernel stays 1:1 by design —
//! this ring composes BESIDE it, at zero kernel surface.
//!
//! ## Layout (the interop contract)
//! ```text
//! byte 0              latestSeq   AtomicU64   0 = no frame yet; frames from 1
//! byte 8              publishes   AtomicU64   telemetry
//! byte 16 + 8k        slotSeq[k]  AtomicU64   0 = INVALIDATED (fill in progress)
//! byte 16 + 8M        payload     M slots × payload_bytes
//! ```
//! `payload_bytes` must be a multiple of 4 (u32 word granularity — the TS
//! port's `payloadFloats` constraint). `ring_bytes = 16 + 8M + M*payload_bytes`.
//!
//! ## Protocol — identical to the TS/C ports
//! - Writer (single, by contract): `begin()` bumps the frame counter and
//!   invalidates the target slot's stamp BEFORE the fill (the FI1 bracket);
//!   `publish()` stamps the slot, then flips `latestSeq` (the publication
//!   point).
//! - Readers (N, independent): bounded (≤ 4) validate-copy-revalidate claim;
//!   a mid-overwrite skip is counted, never silent, never a spin (Law 1).
//!   `dropped` telescopes exactly: `sum(dropped) == lastSeq - freshClaims`.
//!
//! ## Memory ordering — mirrors `core/c/fanout.c` (the fenced acq/rel regime)
//! - `begin()`: SeqCst invalidate store + SeqCst fence before returning —
//!   no fill word may become visible before the invalidate stamp (P1).
//! - `publish()`: Release stamp + Release publication point — the fill is
//!   sequenced below the stamp (the kernel's envelope→canary→exchange stance).
//! - `claim()`: Acquire loads; one SeqCst fence between the copy and the
//!   revalidation load (P2); payload words are Relaxed atomic u32 accesses —
//!   race-free in the strict model at zero x86 cost, which is also what keeps
//!   the ring TSAN-clean.
//!
//! ## Law 2
//! `begin`/`fill`/`publish`/`claim` allocate nothing; `new`/`create_reader`
//! allocate once. The claim record is identity-stable and mutated in place.

use std::alloc::{alloc, dealloc, Layout};
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering, fence};

/// Maximum ring depth (RFC 0004 recommends 4–8).
pub const FANOUT_MAX_SLOTS: usize = 64;
/// Bounded claim attempts — same constant and rationale as the TS/C ports.
pub const FANOUT_MAX_CLAIM_ATTEMPTS: usize = 4;

const IDX_LATEST: usize = 0;
const IDX_PUBLISHES: usize = 1;
const IDX_SLOTSEQ: usize = 2;

/// Claim result record — reader-owned, identity-stable, mutated in place per
/// claim (zero allocation on the hot path). Read it synchronously after
/// `claim()`; do not retain it across claims expecting a snapshot.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FanoutClaim {
    /// A new consistent frame was claimed this tick.
    pub fresh: bool,
    /// Frame seq now held (last consistent if `!fresh`).
    pub seq: u64,
    /// Frames completed without this reader ever observing them.
    pub dropped: u64,
}

/// Advisory reader statistics (AXIOM T: advisory, never a correctness
/// reference). Mirrors the TS `FanoutReaderStats` / C `weft_fanout_stats_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FanoutReaderStats {
    /// Total `claim()` calls.
    pub reads: u64,
    /// Claims that returned a fresh frame.
    pub fresh: u64,
    /// Sum of `dropped` across fresh claims.
    pub drops: u64,
    /// Ticks skipped: target slot mid-overwrite, no newer frame.
    pub skipped_mid_overwrite: u64,
    /// Claims that exhausted the bounded retry.
    pub torn_exhausted: u64,
}

/// Advisory broadcaster state (cold path — allocates; AXIOM T applies).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FanoutDebugStats {
    /// Atomic load of `latestSeq` (advisory).
    pub latest_seq: u64,
    /// Atomic load of `publishes` (advisory).
    pub publishes: u64,
    /// Ring depth M.
    pub slot_count: usize,
    /// Per-slot payload capacity in bytes.
    pub payload_bytes: usize,
    /// Per-slot stamps (0 = invalidated; advisory).
    pub slot_stamps: Vec<u64>,
}

/// Total ring size in bytes for the given geometry, or `None` on bad
/// geometry. Identical to `core/ts/fanout.ts` and `weft_fanout_ring_bytes`.
pub fn ring_bytes(payload_bytes: usize, slot_count: usize) -> Option<usize> {
    if payload_bytes == 0 || payload_bytes % 4 != 0 || !(2..=FANOUT_MAX_SLOTS).contains(&slot_count) {
        return None;
    }
    Some(payload_base(slot_count) + slot_count * payload_bytes)
}

fn payload_base(slot_count: usize) -> usize {
    16 + 8 * slot_count
}

// ---------------------------------------------------------------------------
// RawRing — one allocation, Arc-shared, atomics throughout
// ---------------------------------------------------------------------------

/// The ring allocation: ctrl block + payload arena in ONE region (the
/// byte-compat contract). Accessed exclusively through atomic views; never
/// aliased mutably after construction.
struct RawRing {
    base: *mut u8,
    layout: Layout,
    total_bytes: usize,
    ctrl_words: usize, // 2 + slot_count
    payload_bytes: usize,
    slot_count: usize,
    owned: bool,
}

// SAFETY: all cross-thread access to the ring goes through atomic operations
// with explicit orderings (see the module docs); the raw base pointer is
// stable for the RawRing's lifetime and never handed out as a mutable
// reference. The seqlock payload discipline is carried by Relaxed ATOMIC
// word accesses (not plain ones), so there is no data race by construction.
unsafe impl Send for RawRing {}
unsafe impl Sync for RawRing {}

impl Drop for RawRing {
    fn drop(&mut self) {
        if self.owned {
            // SAFETY: allocated with the identical Layout in `alloc_ring`.
            unsafe { dealloc(self.base, self.layout) };
        }
    }
}

impl RawRing {
    fn alloc(payload_bytes: usize, slot_count: usize) -> Option<Arc<Self>> {
        let total = ring_bytes(payload_bytes, slot_count)?;
        // SAFETY: total > 0; alignment 64 is a power of two; the allocation
        // is immediately zero-initialized so every ctrl word reads 0
        // (latestSeq = "no frame yet", all slots invalidated) — the same
        // invariants a fresh SAB gives the TS port.
        let layout = unsafe { Layout::from_size_align_unchecked(total, 64) };
        let base = unsafe { alloc(layout) };
        if base.is_null() {
            return None;
        }
        unsafe { std::ptr::write_bytes(base, 0, total) };
        Some(Arc::new(Self {
            base,
            layout,
            total_bytes: total,
            ctrl_words: 2 + slot_count,
            payload_bytes,
            slot_count,
            owned: true,
        }))
    }

    /// Attach to FOREIGN ring memory (another port's bytes behind an FFI
    /// boundary). Geometry is validated; the caller owns the memory — the
    /// RawRing will not free it.
    ///
    /// # Safety
    /// - `base` must be 8-byte aligned, readable AND (for a broadcaster
    ///   attach) writable for `ring_bytes(payload_bytes, slot_count)` bytes,
    ///   for the lifetime of every handle derived from the returned ring.
    /// - `base` must not be aliased by any `&mut` while any handle lives.
    /// - The bytes must be a valid ring per the layout contract (produced by
    ///   a Weft fan-out implementation in any language).
    unsafe fn from_raw(base: *const u8, ring_len: usize, payload_bytes: usize, slot_count: usize) -> Option<Arc<Self>> {
        let total = ring_bytes(payload_bytes, slot_count)?;
        if ring_len != total {
            return None;
        }
        Some(Arc::new(Self {
            base: base as *mut u8,
            // Never deallocated (owned = false); layout recorded for symmetry.
            layout: unsafe { Layout::from_size_align_unchecked(total, 64) },
            total_bytes: total,
            ctrl_words: 2 + slot_count,
            payload_bytes,
            slot_count,
            owned: false,
        }))
    }

    /// Ctrl block as atomic u64s (latest, publishes, slot stamps).
    fn ctrl(&self) -> &[AtomicU64] {
        // SAFETY: base is 64-aligned (>= 8); ctrl_words * 8 <= total_bytes;
        // the region is only ever accessed atomically (see the Send/Sync note).
        unsafe { std::slice::from_raw_parts(self.base as *const AtomicU64, self.ctrl_words) }
    }

    /// Slot k's payload as atomic u32 words.
    fn slot_words(&self, k: usize) -> &[AtomicU32] {
        // SAFETY: payload_base + k*payload_bytes is 4-aligned (payload_base is
        // 8-aligned and payload_bytes % 4 == 0); the slice stays in-bounds by
        // construction; accessed only atomically.
        unsafe {
            std::slice::from_raw_parts(
                self.base.add(payload_base(self.slot_count) + k * self.payload_bytes) as *const AtomicU32,
                self.payload_bytes / 4,
            )
        }
    }

    /// The whole ring as bytes — the interop export (serialize me, hand me
    /// to another port).
    fn as_bytes(&self) -> &[u8] {
        // SAFETY: total_bytes bytes owned/valid for the RawRing's lifetime.
        unsafe { std::slice::from_raw_parts(self.base, self.total_bytes) }
    }
}

// ---------------------------------------------------------------------------
// Broadcaster — the writer side (single writer by contract)
// ---------------------------------------------------------------------------

/// The fan-out broadcaster. `begin`/`fill`/`publish` take `&mut self`: the
/// single-writer contract is encoded in the type. Zero allocation per frame.
pub struct WeftFanout {
    ring: Arc<RawRing>,
    w_seq: u64,
    w_slot: usize,
    /// True after the first begin() — the C port's w_cursor != NULL guard
    /// (fill before any begin is rejected, including on an attached ring).
    begun: bool,
}

impl WeftFanout {
    /// Allocate a fan-out ring. `payload_bytes` must be a multiple of 4;
    /// `slot_count` in `[2, 64]` (RFC 0004 recommends 4–8). Returns `None`
    /// on bad geometry or allocation failure (the kernel's `new` stance).
    pub fn new(payload_bytes: usize, slot_count: usize) -> Option<Self> {
        let ring = RawRing::alloc(payload_bytes, slot_count)?;
        Some(Self { ring, w_seq: 0, w_slot: 0, begun: false })
    }

    /// Begin the next frame: bumps the frame counter, INVALIDATES the target
    /// slot's stamp (SeqCst store + SeqCst fence — property P1) and selects
    /// the fill target. Follow with `fill`/`fill_f32`, then `publish`.
    pub fn begin(&mut self) {
        self.w_seq += 1;
        let k = ((self.w_seq - 1) as usize) % self.ring.slot_count;
        // FI1 bracket, first half: invalidate BEFORE the fill.
        self.ring.ctrl()[IDX_SLOTSEQ + k].store(0, Ordering::SeqCst);
        fence(Ordering::SeqCst);
        self.w_slot = k;
        self.begun = true;
    }

    /// Fill the begun slot from `src` via Relaxed atomic u32 word stores
    /// (`src.len()` must be a multiple of 4 and <= payload_bytes). Returns
    /// the words written, or `None` on a bad length or a missing `begin`.
    /// Zero allocation; race-free against concurrent reader copies.
    pub fn fill(&mut self, src: &[u8]) -> Option<usize> {
        if !self.begun || src.len() > self.ring.payload_bytes || src.len() % 4 != 0 {
            return None;
        }
        let words = self.ring.slot_words(self.w_slot);
        let n = src.len() / 4;
        for (w, chunk) in src.chunks_exact(4).enumerate() {
            let v = u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
            words[w].store(v, Ordering::Relaxed);
        }
        Some(n)
    }

    /// `fill` for `&[f32]` payloads (the TS port's Float32 views) — the same
    /// Relaxed atomic word stores over the f32 bits.
    pub fn fill_f32(&mut self, src: &[f32]) -> Option<usize> {
        if !self.begun || src.is_empty() || src.len() * 4 > self.ring.payload_bytes {
            return None;
        }
        let words = self.ring.slot_words(self.w_slot);
        for (w, f) in src.iter().enumerate() {
            words[w].store(f.to_bits(), Ordering::Relaxed);
        }
        Some(src.len())
    }

    /// Publish the begun frame: stamp the slot (Release), flip the
    /// publication point (Release), bump `publishes` (Relaxed telemetry).
    /// Returns the frame seq, or 0 if no `begin()` ever ran (a detectable
    /// no-op — TS parity). Zero allocation.
    pub fn publish(&mut self) -> u64 {
        if !self.begun {
            return 0;
        }
        let k = self.w_slot;
        let ctrl = self.ring.ctrl();
        ctrl[IDX_SLOTSEQ + k].store(self.w_seq, Ordering::Release);
        ctrl[IDX_LATEST].store(self.w_seq, Ordering::Release);
        ctrl[IDX_PUBLISHES].fetch_add(1, Ordering::Relaxed);
        self.w_seq
    }

    /// Create a reader bound to this ring (shares the allocation via Arc).
    /// N readers, one per consumer, each fully independent.
    pub fn create_reader(&self) -> WeftFanoutReader {
        WeftFanoutReader {
            ring: Arc::clone(&self.ring),
            target: vec![0u32; self.ring.payload_bytes / 4].into_boxed_slice(),
            last_seq: 0,
            rec: FanoutClaim { fresh: false, seq: 0, dropped: 0 },
            n_reads: 0,
            n_fresh: 0,
            n_drops: 0,
            n_skip: 0,
            n_exhausted: 0,
        }
    }

    /// The ring bytes — the interop export (hand to another port).
    pub fn as_bytes(&self) -> &[u8] {
        self.ring.as_bytes()
    }

    /// Ring geometry: `(payload_bytes, slot_count)`.
    pub fn geometry(&self) -> (usize, usize) {
        (self.ring.payload_bytes, self.ring.slot_count)
    }

    /// Attach a WRITER to foreign ring memory (e.g. a C/TS-produced ring
    /// behind FFI). Continues the frame numbering from the ring's
    /// `latestSeq` so per-slot stamps stay monotonic across the producer
    /// handoff. SINGLE WRITER BY CONTRACT: attaching a second live
    /// broadcaster to the same ring is a caller error — document, don't
    /// defend (the same stance as the C port).
    ///
    /// # Safety
    /// See `RawRing::from_raw` — plus: no other writer may be active on this
    /// ring, now or later.
    pub unsafe fn attach_raw(base: *mut u8, ring_len: usize, payload_bytes: usize, slot_count: usize) -> Option<Self> {
        // SAFETY: the caller upholds RawRing::from_raw's contract (see the
        // # Safety section above); no other writer is active on this ring.
        let ring = unsafe { RawRing::from_raw(base, ring_len, payload_bytes, slot_count) }?;
        let w_seq = ring.ctrl()[IDX_LATEST].load(Ordering::Acquire);
        Some(Self { ring, w_seq, w_slot: 0, begun: false })
    }

    /// Advisory state snapshot (cold path — allocates; AXIOM T).
    pub fn debug_stats(&self) -> FanoutDebugStats {
        let ctrl = self.ring.ctrl();
        let mut slot_stamps = Vec::with_capacity(self.ring.slot_count);
        for k in 0..self.ring.slot_count {
            slot_stamps.push(ctrl[IDX_SLOTSEQ + k].load(Ordering::Acquire));
        }
        FanoutDebugStats {
            latest_seq: ctrl[IDX_LATEST].load(Ordering::Acquire),
            publishes: ctrl[IDX_PUBLISHES].load(Ordering::Acquire),
            slot_count: self.ring.slot_count,
            payload_bytes: self.ring.payload_bytes,
            slot_stamps,
        }
    }
}

// ---------------------------------------------------------------------------
// Reader — the consumer side (N per ring, each fully independent)
// ---------------------------------------------------------------------------

/// A reader bound to a ring. Owns its pre-allocated copy buffer and its
/// identity-stable claim record. `claim(&mut self)` encodes the
/// one-reader-thread-at-a-time contract (same discipline as the kernel's
/// reader-private r_work).
pub struct WeftFanoutReader {
    ring: Arc<RawRing>,
    target: Box<[u32]>,
    last_seq: u64,
    rec: FanoutClaim,
    n_reads: u64,
    n_fresh: u64,
    n_drops: u64,
    n_skip: u64,
    n_exhausted: u64,
}

impl WeftFanoutReader {
    /// Attach a reader to FOREIGN ring memory (any port's bytes — the
    /// cross-language path). Geometry is validated against `ring_len` — a
    /// mismatched pair returns `None` instead of tearing.
    ///
    /// # Safety
    /// See `RawRing::from_raw`: the memory must stay valid and unaliased by
    /// `&mut` for the lifetime of this reader (and of every clone of its
    /// internal Arc).
    pub unsafe fn attach_raw(base: *const u8, ring_len: usize, payload_bytes: usize, slot_count: usize) -> Option<Self> {
        // SAFETY: the caller upholds RawRing::from_raw's contract (see the
        // # Safety section above).
        let ring = unsafe { RawRing::from_raw(base, ring_len, payload_bytes, slot_count) }?;
        Some(WeftFanoutReader {
            ring,
            target: vec![0u32; payload_bytes / 4].into_boxed_slice(),
            last_seq: 0,
            rec: FanoutClaim { fresh: false, seq: 0, dropped: 0 },
            n_reads: 0,
            n_fresh: 0,
            n_drops: 0,
            n_skip: 0,
            n_exhausted: 0,
        })
    }

    /// Claim the freshest completed frame into this reader's buffer. Never
    /// blocks, never spins unboundedly, never fails: a tick with no
    /// consistent newer frame returns `fresh: false` and the reader keeps
    /// its last consistent frame. Returns the identity-stable claim record.
    pub fn claim(&mut self) -> &FanoutClaim {
        self.n_reads += 1;
        let ctrl = self.ring.ctrl();
        let mut l = ctrl[IDX_LATEST].load(Ordering::Acquire);
        if l == 0 || l == self.last_seq {
            self.rec = FanoutClaim { fresh: false, seq: self.last_seq, dropped: 0 };
            return &self.rec;
        }
        for _ in 0..FANOUT_MAX_CLAIM_ATTEMPTS {
            let k = ((l - 1) as usize) % self.ring.slot_count;
            let s_b = ctrl[IDX_SLOTSEQ + k].load(Ordering::Acquire);
            if s_b != l {
                // Slot mid-overwrite (stamp 0) or re-stamped by a newer frame.
                let l2 = ctrl[IDX_LATEST].load(Ordering::Acquire);
                if l2 == l {
                    // Graceful skip (Law 1): counted, never silent.
                    self.n_skip += 1;
                    self.rec = FanoutClaim { fresh: false, seq: self.last_seq, dropped: 0 };
                    return &self.rec;
                }
                l = l2;
                continue;
            }
            // Stamp matches frame L: copy, then re-validate.
            let words = self.ring.slot_words(k);
            for (w, word) in words.iter().enumerate() {
                self.target[w] = word.load(Ordering::Relaxed);
            }
            // P2: the copy is ordered before the revalidation load.
            fence(Ordering::SeqCst);
            let s_a = ctrl[IDX_SLOTSEQ + k].load(Ordering::Acquire);
            if s_a == l {
                let dropped = l - self.last_seq - 1;
                self.n_drops += dropped;
                self.last_seq = l;
                self.n_fresh += 1;
                self.rec = FanoutClaim { fresh: true, seq: self.last_seq, dropped };
                return &self.rec;
            }
            // Torn copy detected — retry on the newest completed frame.
            l = ctrl[IDX_LATEST].load(Ordering::Acquire);
        }
        // Bounded retries exhausted: keep the last consistent frame (Law 1).
        self.n_exhausted += 1;
        self.rec = FanoutClaim { fresh: false, seq: self.last_seq, dropped: 0 };
        &self.rec
    }

    /// The reader's pre-allocated copy buffer as u32 words (stable identity;
    /// meaningful after a fresh claim — read it live before the next claim,
    /// the same discipline as the kernel's `r_read_slice` (A3)).
    pub fn view(&self) -> &[u32] {
        &self.target
    }

    /// `view()` as raw bytes.
    pub fn view_bytes(&self) -> &[u8] {
        // SAFETY: target is a Box<[u32]> of payload_bytes/4 words — the u8
        // view has exactly payload_bytes bytes and the same lifetime.
        unsafe { std::slice::from_raw_parts(self.target.as_ptr() as *const u8, self.target.len() * 4) }
    }

    /// `view()` as f32 (the draw-side interpretation of the TS port's
    /// Float32 views), copied into `dst` with zero allocation.
    pub fn view_f32_into(&self, dst: &mut [f32]) {
        let n = dst.len().min(self.target.len());
        for i in 0..n {
            dst[i] = f32::from_bits(self.target[i]);
        }
    }

    /// The last frame seq this reader has held consistent (0 = none yet).
    pub fn last_seq(&self) -> u64 {
        self.last_seq
    }

    /// Advisory statistics snapshot (cold path; AXIOM T).
    pub fn stats(&self) -> FanoutReaderStats {
        FanoutReaderStats {
            reads: self.n_reads,
            fresh: self.n_fresh,
            drops: self.n_drops,
            skipped_mid_overwrite: self.n_skip,
            torn_exhausted: self.n_exhausted,
        }
    }
}
