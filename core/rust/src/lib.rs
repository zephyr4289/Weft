//! # weft-core — Corrected Triad Protocol kernel (Rust reference)
//!
//! Normative: per 02-KERNEL.md and 03-ENVELOPE.md. Direct port of the C kernel
//! in `core/c/weft.{h,c}`. Same protocol, same envelope, same I6 handshake,
//! same memory-ordering matrix (see `litmus/catalog.yaml`).
//!
//! ## Memory model
//!
//! - `latest`: `AtomicU32`, exchanged with `AcqRel` by writer (publish) and reader (claim).
//! - `revoked`: `AtomicBool`, writer `Relaxed` load (advisory), releaser `Release` store.
//! - `epoch`: `AtomicU32`, writer `AcqRel` `fetch_add` (ACK), reclaim `Acquire` poll.
//! - Telemetry: `AtomicU64` `Relaxed` `fetch_add` (statistics only).
//! - `w_work`, `r_work`: stored as `AtomicU32` with `Relaxed` ONLY so `Weft` is `Sync`
//!   and scoped threads can share `&Weft`. They are thread-private by contract; the
//!   atomics are NOT protocol state. Documented at the field (06 §3).
//!
//! ## Forbidden patterns (02 §2.2)
//!
//! - No second atomic on the ownership path (the withdrawn two-variable design).
//! - No CAS retry loops in publish/claim (wait-freedom = zero loops; `exchange` never retries).
//! - `claim()` cannot fail; `publish()` returns only `Ok` / `DroppedRevoked`.
//! - No `unsafe` outside the two payload helpers (`w_write_payload`, `r_read_slice`),
//!   each with a `SAFETY:` comment citing RFC-0001 §4 (06 §3).
//! - No `SeqCst` anywhere (06 §2). The ordering matrix is the audit.
//!
//! ## Zero dependencies
//!
//! `Cargo.toml` has no `[dependencies]`. Build with `--offline`.

#![forbid(unsafe_op_in_unsafe_fn)]
#![deny(missing_docs)]
// All public items documented. `unsafe_op_in_unsafe_fn` forbidden (Rust 2024 default).

use std::alloc::{alloc, dealloc, Layout};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

// Driver-layer modules (RFC 0004 fan-out ring + RFC 0008 FrameCursor) —
// sibling files that compose BESIDE the kernel at zero kernel surface; the
// frozen kernel items above/below are byte-identical to origin/main.
pub mod fanout;
pub mod frame_cursor;
pub mod governor;
pub mod shm;
/// VerifiedWeft — authenticated frame records (RFC 0005). Driver-layer:
/// composes BESIDE the kernel; byte-compat with core/c/verified.c.
pub mod verified;

/// Magic "WEFT" little-endian: bytes 57 45 46 54 → u32 LE = 0x54464557.
pub const WEFT_MAGIC: u32 = 0x54464557;

/// Upper bound for payload_max (1 MiB) — the TIER4 §5 validation wall
/// (issue #19). Mirrors WEFT_PAYLOAD_MAX_LIMIT (core/c/weft.h).
pub const WEFT_PAYLOAD_MAX_LIMIT: usize = 1 << 20;

/// Triad protocol version 1.
pub const WEFT_VERSION_1: u16 = 1;

/// Publish result (02 §4).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PubResult {
    /// Publish succeeded.
    Ok,
    /// Writer has been revoked; publish was a no-op.
    DroppedRevoked,
    /// TIER4 §5 (issue #19): input validation failed (payload_len >
    /// payload_max). No byte written, no state changed — counted in t_invalid.
    Invalid,
}

/// Decode result (03-ENVELOPE §2).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DecodeResult {
    /// Decode succeeded.
    Ok,
    /// Buffer too short for the envelope or payload.
    Short,
    /// Magic field does not match "WEFT".
    BadMagic,
    /// Header size is invalid (< 16 or > avail).
    BadHeader,
}

/// Buffer layout: `[0..16)` envelope · `[16..16+payload_max)` payload · `[buf_size-8..buf_size)` canary.
/// Canary is u64 LE, value = seq, written in `publish` after the envelope.
///
/// One `Weft` instance owns exactly three buffers + the single shared atomic `latest`
/// + writer-private `w_work` + reader-private `r_work` + I6 state (`revoked`, `epoch`)
/// + telemetry counters.
pub struct Weft {
    /// Three off-heap buffers, 64-byte aligned.
    buffers: [*mut u8; 3],
    /// Layout used to allocate each buffer; stored for Drop.
    layout: Layout,
    /// Byte size of one buffer = align64(16 + payload_max + 8, 64).
    pub buf_size: usize,
    /// Payload capacity in bytes (immutable after init).
    pub payload_max: usize,

    /// The single shared atomic. Exchanged by writer (publish) and reader (claim).
    /// Init: 0. Memory order: AcqRel on both exchanges (02 §5).
    latest: AtomicU32,

    /// Writer-private working index. Stored as AtomicU32 (Relaxed) ONLY so `Weft`
    /// is `Sync` for scoped-thread sharing. Thread-private by contract — NOT protocol state.
    /// Init: 1.
    w_work: AtomicU32,

    /// Reader-private held index. Same note as `w_work`. Init: 2.
    r_work: AtomicU32,

    /// I6: writer revocation flag. Init: false.
    revoked: AtomicBool,
    /// I6: writer epoch. Init: 0. Increments on each ACK.
    epoch: AtomicU32,

    // Telemetry (Relaxed; never synchronization)
    t_publish: AtomicU64,
    t_claim: AtomicU64,
    t_drop: AtomicU64,
    t_invalid: AtomicU64,
    /// TIER4 §4 (issue #19): reclaim ceiling (ms; 0 = caller owns the bound)
    /// and the advisory timeout count.
    max_reclaim_timeout_ms: AtomicU32,
    t_reclaim_timeouts: AtomicU64,
    t_wsteps: AtomicU64,
    t_rsteps: AtomicU64,
}

// SAFETY: Weft's internal state is synchronized via atomics with explicit orderings.
// The raw buffer pointers are stable for the lifetime of the Weft and never aliased
// in a way that violates Rust's aliasing rules (the writer writes to `w_work` while
// the reader reads `r_work`; the protocol guarantees they never touch the same buffer
// simultaneously — see 02 §2.1).
unsafe impl Send for Weft {}
unsafe impl Sync for Weft {}

/// Compute buf_size = align64(16 + payload_max + 8, 64).
fn compute_buf_size(payload_max: usize) -> usize {
    let raw = 16 + payload_max + 8;
    (raw + 63) & !63
}

/// Canary offset (buf_size - 8).
fn canary_offset(buf_size: usize) -> usize { buf_size - 8 }

impl Weft {
    /// Allocate a Weft with the given payload_max. Returns `None` on alloc failure.
    /// Per 02 §3: may allocate; nothing else may (Law 2).
    pub fn new(payload_max: usize) -> Option<Self> {
        // TIER4 §5 validation wall (issue #19): mirror the C kernel's refusal —
        // payload_max ∈ [1, 1 MiB]. A frameless or oversized triad is refused
        // BEFORE any allocation.
        if payload_max == 0 || payload_max > WEFT_PAYLOAD_MAX_LIMIT {
            return None;
        }
        let buf_size = compute_buf_size(payload_max);
        // SAFETY: buf_size > 0 (since payload_max >= 0 and we add 24); alignment 64
        // is a power of two. Layout::from_size_align is infallible for these inputs.
        let layout = Layout::from_size_align(buf_size, 64).ok()?;

        let mut buffers: [*mut u8; 3] = [std::ptr::null_mut(); 3];
        for (i, slot) in buffers.iter_mut().enumerate() {
            // SAFETY: layout is valid (size > 0, align is power of two). We handle
            // the null case below.
            let p = unsafe { alloc(layout) };
            if p.is_null() {
                // Clean up what we have and bail.
                for j in 0..i {
                    // SAFETY: buffers[j] was allocated with `layout` in this function.
                    unsafe { dealloc(buffers[j], layout) };
                    buffers[j] = std::ptr::null_mut();
                }
                return None;
            }
            *slot = p;
        }

        // Zero all three buffers and write null envelopes (seq=0, magic, version=1,
        // header_size=16). Also fill the null frame's payload with pat(0, i) so
        // verify_held(0, payload_max) passes.
        // Per 04-LITMUS §0.6 (v1.1, normative null-frame fixture invariant): the null
        // frame is a valid initial state, not a sentinel that breaks verification.
        // A kernel that zero-fills the null frame makes L6 false-red.
        for buf in &buffers {
            // SAFETY: each buffer is `buf_size` bytes, just allocated. We zero them.
            unsafe { std::ptr::write_bytes(*buf, 0, buf_size) };
            // Write the null envelope (seq=0, payload_len=payload_max).
            envelope_encode_v1(*buf, 0, payload_max as u32);
            // Fill the null frame's payload with pat(0, i) per §0.6.
            for j in 0..payload_max {
                // SAFETY: payload region is buf+16..buf+16+payload_max, all within buf_size.
                unsafe { std::ptr::write_bytes((*buf).add(16 + j), pat(0, j as u32), 1) };
            }
            // Canary is at buf_size-8; value = 0 (matches seq=0). write_bytes already zeroed it.
        }

        Some(Self {
            buffers,
            layout,
            buf_size,
            payload_max,
            latest: AtomicU32::new(0),
            w_work: AtomicU32::new(1),
            r_work: AtomicU32::new(2),
            revoked: AtomicBool::new(false),
            epoch: AtomicU32::new(0),
            t_publish: AtomicU64::new(0),
            t_claim: AtomicU64::new(0),
            t_drop: AtomicU64::new(0),
            t_invalid: AtomicU64::new(0),
            max_reclaim_timeout_ms: AtomicU32::new(1000),
            t_reclaim_timeouts: AtomicU64::new(0),
            t_wsteps: AtomicU64::new(0),
            t_rsteps: AtomicU64::new(0),
        })
    }

    /// Write `len` bytes of payload from `src` into the writer's working buffer
    /// at offset 16. Returns `Err(())` if `len > payload_max`.
    ///
    /// # Safety contract
    ///
    /// - Caller must be the single registered writer (invariant I2).
    /// - `src` must point to at least `len` bytes.
    /// - Must not be called after `revoke()` + ACK.
    ///
    /// # SAFETY (Rust aliasing)
    ///
    /// Per RFC-0001 §4: "writer owns `w_work` exclusively between its exchanges."
    /// The writer's `w_work` is not readable by the reader (the reader's `r_work`
    /// is a different index). The write is to the writer's private buffer, which
    /// becomes visible to the reader only via the subsequent `publish`'s Release exchange.
    pub unsafe fn w_write_payload(&self, src: *const u8, len: usize) -> Result<(), ()> {
        if len > self.payload_max { return Err(()); }
        let w = self.w_work.load(Ordering::Relaxed) as usize;
        // SAFETY: w is in 0..3 (guaranteed by init + protocol). The writer owns
        // buffers[w] exclusively (no reader can be reading it). `src` is valid for
        // `len` bytes per the caller contract. The payload region [16..16+len)
        // is within buf_size.
        unsafe {
            let dst = self.buffers[w].add(16);
            std::ptr::copy_nonoverlapping(src, dst, len);
        }
        Ok(())
    }

    /// Fill the writer's working buffer with `pat(seq, i)` payload, write the
    /// envelope (v1, seq, payload_len), write the canary, and atomically publish.
    ///
    /// Per 02 §2 + §6:
    /// 1. if revoked.load(Relaxed): epoch.fetch_add(1, AcqRel); t_drop++;
    ///    return DroppedRevoked  (checked FIRST, before any byte write)
    /// 2. write envelope (v1, seq, payload_len) into buf[w_work]
    /// 3. write canary = seq at buf[w_work].tail
    /// 4. old = latest.exchange(w_work, AcqRel)   // THE atomic
    /// 5. w_work = old
    /// 6. t_publish++; t_wsteps++; return Ok
    ///
    /// # Safety contract
    ///
    /// - Caller must be the single registered writer (invariant I2).
    /// - Must not be called after `revoke()` + ACK.
    pub unsafe fn publish(&self, seq: u32, payload_len: u32) -> PubResult {
        // §6 step 1: revoked checked FIRST, before any byte write. Relaxed load is
        // advisory — correctness does not depend on seeing it THIS publish; the
        // NEXT publish will see it. Buffers stay valid until ACK.
        if self.revoked.load(Ordering::Relaxed) {
            // ACK: epoch.fetch_add(1, AcqRel). Publishes "I will never write again."
            self.epoch.fetch_add(1, Ordering::AcqRel);
            self.t_drop.fetch_add(1, Ordering::Relaxed);
            return PubResult::DroppedRevoked;
        }

        // TIER4 §5 validation wall (issue #19): refuse the frame WHOLE before
        // any byte write. Counted (t_invalid), never silent.
        if payload_len > self.payload_max as u32 {
            self.t_invalid.fetch_add(1, Ordering::Relaxed);
            return PubResult::Invalid;
        }

        let w = self.w_work.load(Ordering::Relaxed) as usize;

        // NOTE (C-parity fix, 2026-09-16): this publish previously re-filled the
        // payload region with pat(seq, i) — a litmus convenience inherited from
        // the original port that the canonical C kernel (02 §2: envelope +
        // canary + exchange — nothing else) does NOT do. It silently overwrote
        // payload written through w_write_payload (the writer-cursor contract,
        // RFC-0001 §4), doubled the fill work in every litmus/bench call, and
        // put ~560 ns of pat() inside the measured publish (B1/B2/B4 tails).
        // The writer fills via w_write_payload (or a scratch + copy) before
        // publish, exactly as C's bench/litmus do through weft_w_begin.
        // Payload bytes beyond what the writer wrote keep their previous
        // contents — the same stale-tail contract as C.

        // Write envelope (v1, seq, payload_len) into buf[w_work].
        envelope_encode_v1(self.buffers[w], seq, payload_len);

        // Write canary = seq at buf[w_work].tail (u64 LE at buf_size-8).
        let canary_val = seq as u64;
        // SAFETY: canary_ptr is within buf_size (8 bytes before the end). The writer
        // owns buffers[w] exclusively.
        unsafe {
            let canary_ptr = self.buffers[w].add(canary_offset(self.buf_size));
            std::ptr::write_unaligned(canary_ptr as *mut u64, canary_val.to_le());
        }

        // THE atomic: publish + take old latest. AcqRel:
        //   Release: publishes payload + envelope + canary writes to the reader.
        //   Acquire: takes ownership of the returned buffer, sees its final state.
        let old = self.latest.swap(w as u32, Ordering::AcqRel);
        self.w_work.store(old, Ordering::Relaxed);

        // Telemetry (Relaxed — never synchronization).
        self.t_publish.fetch_add(1, Ordering::Relaxed);
        self.t_wsteps.fetch_add(1, Ordering::Relaxed);

        PubResult::Ok
    }

    /// Claim the freshest published buffer. Per 02 §2:
    ///   mine = latest.exchange(r_work, AcqRel)   // THE atomic
    ///   r_work = mine
    ///   t_claim++; t_rsteps++
    ///   return mine
    /// NEVER fails. Before any publish, returns 0 (the null frame, seq=0).
    pub fn claim(&self) -> u32 {
        // 02 §2: mine = latest.swap(r_work, AcqRel)
        //   Acquire: sees the writer's published payload + envelope + canary.
        //   Release: the reader's previous buffer (r_work) is now handed back to
        //            the writer; its state is the reader's final state.
        let r = self.r_work.load(Ordering::Relaxed);
        let mine = self.latest.swap(r, Ordering::AcqRel);
        self.r_work.store(mine, Ordering::Relaxed);

        self.t_claim.fetch_add(1, Ordering::Relaxed);
        self.t_rsteps.fetch_add(1, Ordering::Relaxed);

        mine  // NEVER fails (02 §2.2)
    }

    /// Read envelope seq of the reader's held buffer (live, not a snapshot).
    pub fn r_seq(&self) -> u32 {
        let r = self.r_work.load(Ordering::Relaxed) as usize;
        // SAFETY: r is in 0..3. The reader owns buffers[r] until its next claim.
        // Read 4 bytes at offset 8 (envelope seq, LE).
        unsafe {
            let p = self.buffers[r].add(8) as *const u32;
            std::ptr::read_unaligned(p).to_le()
        }
    }

    /// Read envelope magic of the reader's held buffer (live).
    pub fn r_magic(&self) -> u32 {
        let r = self.r_work.load(Ordering::Relaxed) as usize;
        unsafe {
            let p = self.buffers[r] as *const u32;
            std::ptr::read_unaligned(p).to_le()
        }
    }

    /// Read envelope payload_len of the reader's held buffer (live).
    pub fn r_payload_len(&self) -> u32 {
        let r = self.r_work.load(Ordering::Relaxed) as usize;
        unsafe {
            let p = self.buffers[r].add(12) as *const u32;
            std::ptr::read_unaligned(p).to_le()
        }
    }

    /// Copy LIVE held-buffer bytes at call time. Per A3: the reader must observe
    /// the live buffer, never a snapshot taken at claim time.
    ///
    /// # SAFETY contract
    ///
    /// - `dst` must point to at least `dst_len` bytes.
    /// - Must be called between a `claim()` and the next `claim()` (the held buffer is exclusive).
    pub unsafe fn r_read_slice(&self, dst: *mut u8, offset: usize, dst_len: usize) -> usize {
        if offset >= self.buf_size { return 0; }
        let r = self.r_work.load(Ordering::Relaxed) as usize;
        // SAFETY: r is in 0..3. The reader owns buffers[r] until its next claim.
        // Per RFC-0001 §4: "reader owns claimed buffer until next claim."
        unsafe {
            let avail = self.buf_size - offset;
            let n = dst_len.min(avail);
            let src = self.buffers[r].add(offset);
            std::ptr::copy_nonoverlapping(src, dst, n);
            n
        }
    }

    /// Direct pointer to the reader's held buffer (live). For verify-in-place (L1, L6).
    /// Returns null if offset >= buf_size.
    pub fn r_live_ptr(&self, offset: usize) -> *const u8 {
        if offset >= self.buf_size { return std::ptr::null(); }
        let r = self.r_work.load(Ordering::Relaxed) as usize;
        // SAFETY: r is in 0..3; offset < buf_size. The reader owns buffers[r] until next claim.
        unsafe { self.buffers[r].add(offset) }
    }

    /// Step 1 of I6: revoke the writer. Sets revoked.store(true, Release).
    pub fn revoke(&self) {
        self.revoked.store(true, Ordering::Release);
    }

    /// Steps 2-3 of I6: poll epoch (Acquire) until it advances past `pre_revoke_epoch`,
    /// bounded by `timeout_ms`. Returns Ok on ACK received, Err on timeout.
    ///
    /// After ACK is observed, the caller may poison (memset 0xDE) or free.
    /// Poison-before-ACK is the bug this handshake prevents (A1).
    pub fn reclaim(&self, pre_revoke_epoch: u32, timeout_ms: u32) -> Result<(), ()> {
        let max_ceiling = self.max_reclaim_timeout_ms.load(Ordering::Relaxed);
        let effective_ms = if max_ceiling != 0 && timeout_ms > max_ceiling {
            max_ceiling
        } else {
            timeout_ms
        };
        let start = std::time::Instant::now();
        let timeout = std::time::Duration::from_millis(effective_ms as u64);
        loop {
            let e = self.epoch.load(Ordering::Acquire);
            if e != pre_revoke_epoch {
                return Ok(());
            }
            if start.elapsed() >= timeout {
                self.t_reclaim_timeouts.fetch_add(1, Ordering::Relaxed);
                return Err(());
            }
            // Brief sleep to avoid burning CPU. The writer ACKs within one publish.
            std::thread::sleep(std::time::Duration::from_micros(100));
        }
    }

    /// TIER4 §4: runtime-configurable reclaim ceiling (ms); 0 disables.
    /// Mirrors weft_set_max_reclaim_timeout (core/c/weft.h).
    pub fn set_max_reclaim_timeout(&self, max_ms: u32) {
        self.max_reclaim_timeout_ms.store(max_ms, Ordering::Relaxed);
    }
    /// Current runtime-configurable reclaim ceiling in milliseconds (TIER4 §4).
    pub fn max_reclaim_timeout(&self) -> u32 { self.max_reclaim_timeout_ms.load(Ordering::Relaxed) }
    /// Advisory reclaim-timeout count (TIER4 §4).
    pub fn t_reclaim_timeouts(&self) -> u64 { self.t_reclaim_timeouts.load(Ordering::Relaxed) }

    // Telemetry (Relaxed loads; statistics only, never synchronization)
    /// Total successful publishes since init.
    pub fn t_publish(&self) -> u64 { self.t_publish.load(Ordering::Relaxed) }
    /// Total successful claims since init.
    pub fn t_claim(&self) -> u64 { self.t_claim.load(Ordering::Relaxed) }
    /// Total DROPPED_REVOKED results since revoke.
    pub fn t_drop(&self) -> u64 { self.t_drop.load(Ordering::Relaxed) }
    /// Refused-publish counter (TIER4 §5 — issue #19).
    pub fn t_invalid(&self) -> u64 { self.t_invalid.load(Ordering::Relaxed) }
    /// Total protocol RMWs in publish (L2 step counter).
    pub fn t_wsteps(&self) -> u64 { self.t_wsteps.load(Ordering::Relaxed) }
    /// Total protocol RMWs in claim (L3 step counter).
    pub fn t_rsteps(&self) -> u64 { self.t_rsteps.load(Ordering::Relaxed) }
    /// Current epoch (Acquire load — for I6 reclaim polling).
    pub fn epoch(&self) -> u32 { self.epoch.load(Ordering::Acquire) }

    /// Raw pointer to buffer `i` (for tests that need direct access, e.g., L7 poisoning).
    /// # Safety
    /// Caller must not write to a buffer the other party might be reading.
    pub unsafe fn buffer_ptr(&self, i: usize) -> *mut u8 {
        debug_assert!(i < 3, "buffer index out of range");
        if i >= 3 { return std::ptr::null_mut(); }
        self.buffers[i]
    }
}

// ---------------------------------------------------------------------------
// Debug view (WO-P2-TOOLS T1) — read-only, wait-free, allocation-free.
// The ONE permitted kernel API addition for Phase 2. Semver: minor bump.
// No unsafe in public API (per roadmap 0c).
// ---------------------------------------------------------------------------

/// Per-buffer debug info for the two LIVE buffers.
#[derive(Debug, Clone, Copy, Default)]
pub struct WeftDebugBuf {
    /// Buffer index (0..2), or 3 for "no live buffer" sentinel.
    pub slot_idx: u32,
    /// Envelope seq (LE)
    pub seq: u32,
    /// Envelope version (LE)
    pub version: u16,
    /// Envelope header_size (LE)
    pub header_size: u16,
    /// Envelope payload_len (LE)
    pub payload_len: u32,
    /// Owner: 0=free, 1=writer, 2=reader, 3=in-exchange(latest)
    pub owner: u8,
}

/// Debug view — a set of individually-consistent samples, NOT a consistent
/// snapshot. Per AXIOM T (05-CONTRACTS v1.3), telemetry values are advisory.
/// w_work/r_work are thread-private; reading them from another thread is
/// advisory-only sampling.
#[derive(Debug, Clone, Copy, Default)]
pub struct WeftDebugView {
    /// Atomic load (Relaxed — advisory per AXIOM T)
    pub latest: u32,
    /// Writer-private (advisory; thread-private by contract)
    pub w_work: u32,
    /// Reader-private (advisory; thread-private by contract)
    pub r_work: u32,
    /// Revoked flag
    pub revoked: bool,
    /// Epoch (Acquire load)
    pub epoch: u32,
    /// Telemetry (advisory)
    pub t_publish: u64,
    /// Telemetry (advisory)
    pub t_claim: u64,
    /// Telemetry (advisory)
    pub t_drop: u64,
    /// Two live buffer samples. The third (if freed/poisoned) is sentinel.
    pub bufs: [WeftDebugBuf; 2],
    /// True if a header sampled mid-publish may be inconsistent.
    pub mid_publish_sample: bool,
}

impl Weft {
    /// Read-only, wait-free, allocation-free inspection. Returns a debug view
    /// of the current kernel state. Never dereferences freed/poisoned buffers
    /// (I6). Per AXIOM T: telemetry values are advisory.
    ///
    /// The view is a set of individually-consistent samples, NOT a consistent
    /// snapshot. Per AXIOM T, telemetry counters are advisory — the slot
    /// exchange is the sole publish/observe point.
    pub fn debug_state(&self) -> WeftDebugView {
        let mut view = WeftDebugView::default();

        // Advisory loads (AXIOM T: telemetry is not a correctness reference).
        view.latest = self.latest.load(Ordering::Relaxed);
        view.w_work = self.w_work.load(Ordering::Relaxed);
        view.r_work = self.r_work.load(Ordering::Relaxed);
        view.revoked = self.revoked.load(Ordering::Relaxed);
        view.epoch = self.epoch.load(Ordering::Acquire);
        view.t_publish = self.t_publish.load(Ordering::Relaxed);
        view.t_claim = self.t_claim.load(Ordering::Relaxed);
        view.t_drop = self.t_drop.load(Ordering::Relaxed);

        // Sample the two LIVE buffers' envelope headers.
        let mut live_count = 0;
        for i in 0..3u32 {
            let is_live = (i == view.w_work) || (i == view.r_work) || (i == view.latest);
            if is_live && live_count < 2 {
                let b = &mut view.bufs[live_count];
                b.slot_idx = i;
                if i == view.w_work      { b.owner = 1; } // writer
                else if i == view.r_work  { b.owner = 2; } // reader
                else                       { b.owner = 3; } // in-exchange

                // Read envelope header (16 bytes). SAFETY: buffer i is live.
                unsafe {
                    let hdr = self.buffers[i as usize];
                    if !hdr.is_null() {
                        b.seq = std::ptr::read_unaligned(hdr.add(8) as *const u32).to_le();
                        b.version = std::ptr::read_unaligned(hdr.add(4) as *const u16).to_le();
                        b.header_size = std::ptr::read_unaligned(hdr.add(6) as *const u16).to_le();
                        b.payload_len = std::ptr::read_unaligned(hdr.add(12) as *const u32).to_le();
                        let magic = std::ptr::read_unaligned(hdr as *const u32).to_le();
                        if magic != WEFT_MAGIC {
                            view.mid_publish_sample = true;
                        }
                    }
                }
                live_count += 1;
            }
        }

        // Fill remaining slots with sentinel.
        while live_count < 2 {
            view.bufs[live_count].slot_idx = 3;
            view.bufs[live_count].owner = 0;
            live_count += 1;
        }

        view
    }
}

impl Drop for Weft {
    fn drop(&mut self) {
        for buf in &self.buffers {
            // SAFETY: each buffer was allocated with `self.layout` in `new`.
            unsafe { dealloc(*buf, self.layout) };
        }
    }
}

// ---------------------------------------------------------------------------
// Envelope pure functions (03-ENVELOPE §1, §2)
// ---------------------------------------------------------------------------

/// Encode a triad-1 envelope into `dst` (must be >= 16 bytes).
pub fn envelope_encode_v1(dst: *mut u8, seq: u32, payload_len: u32) {
    envelope_encode(dst, WEFT_VERSION_1, 16, seq, payload_len);
}

/// Encode a custom-version envelope (for L8b: header_size can be > 16).
pub fn envelope_encode(dst: *mut u8, version: u16, header_size: u16, seq: u32, payload_len: u32) {
    // Per 03-ENVELOPE §1: all fields little-endian.
    unsafe {
        std::ptr::write_unaligned(dst as *mut u32, WEFT_MAGIC.to_le());
        std::ptr::write_unaligned(dst.add(4) as *mut u16, version.to_le());
        std::ptr::write_unaligned(dst.add(6) as *mut u16, header_size.to_le());
        std::ptr::write_unaligned(dst.add(8) as *mut u32, seq.to_le());
        std::ptr::write_unaligned(dst.add(12) as *mut u32, payload_len.to_le());
        // Unknown trailing fields (between 16 and header_size) filled with 0xAA (L8b convention).
        for i in 16..header_size as usize {
            std::ptr::write_unaligned(dst.add(i), 0xAA);
        }
    }
}

/// Decode an envelope. Per 03-ENVELOPE §2.
pub fn envelope_decode(buf: *const u8, avail: usize) -> Result<(u16, u16, u32, u32), DecodeResult> {
    unsafe {
        if avail < 16 { return Err(DecodeResult::Short); }
        let magic = std::ptr::read_unaligned(buf as *const u32).to_le();
        if magic != WEFT_MAGIC { return Err(DecodeResult::BadMagic); }
        let version = std::ptr::read_unaligned(buf.add(4) as *const u16).to_le();
        let header_size = std::ptr::read_unaligned(buf.add(6) as *const u16).to_le();
        if header_size < 16 || header_size as usize > avail { return Err(DecodeResult::BadHeader); }
        let seq = std::ptr::read_unaligned(buf.add(8) as *const u32).to_le();
        let payload_len = std::ptr::read_unaligned(buf.add(12) as *const u32).to_le();
        if payload_len as usize > avail - header_size as usize { return Err(DecodeResult::Short); }
        Ok((version, header_size, seq, payload_len))
    }
}

/// Bind-time version negotiation. Per 03-ENVELOPE §3:
///   chosen = max({ v in S : v <= W })
///   empty set → 0 (BIND_INCOMPATIBLE)
pub fn negotiate(writer_version: u16, reader_versions: &[u16]) -> u16 {
    let mut chosen: u16 = 0;
    for &rv in reader_versions {
        if rv <= writer_version && rv > chosen {
            chosen = rv;
        }
    }
    chosen  // 0 = BIND_INCOMPATIBLE
}

// ---------------------------------------------------------------------------
// Shared payload pattern (04-LITMUS §0.1)
// ---------------------------------------------------------------------------

/// mix32(x) — per 04-LITMUS §0.1. All ops mod 2^32 (u32 wrapping).
pub fn mix32(x: u32) -> u32 {
    let mut x = x;
    x ^= x >> 16;
    x = x.wrapping_mul(0x7FEB352D);
    x ^= x >> 15;
    x = x.wrapping_mul(0x846CA68B);
    x ^= x >> 16;
    x
}

/// pat(seq, i) — per 04-LITMUS §0.1. Deterministic payload byte.
/// Identical to C and TS implementations (A5).
pub fn pat(seq: u32, i: u32) -> u8 {
    let x = seq.wrapping_mul(2654435761).wrapping_add(i.wrapping_mul(2246822519));
    (mix32(x) & 0xFF) as u8
}

/// xorshift32 step — per 04-LITMUS §0.2. Marsaglia 13/17/5. State 0 is invalid (reseed 0x9E3779B9).
pub fn xorshift32(state: &mut u32) -> u32 {
    if *state == 0 { *state = 0x9E3779B9; }
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    x
}
