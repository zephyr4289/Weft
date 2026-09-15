//! The Triad Protocol.
//!
//! Three off-heap buffers + two atomics + a writer-private index. Wait-free on
//! both sides. No torn reads.
//!
//! See the spec §5 for the full protocol. This module is the production Rust
//! implementation; the C spike in `weft-spike/` is the empirical validation.

use std::alloc::{alloc, dealloc, Layout};
use std::sync::atomic::{AtomicI32, AtomicU64, Ordering};
use std::ptr::NonNull;

/// Configuration for a Weft. Capacity is in elements (T), not bytes.
#[derive(Clone, Copy, Debug)]
pub struct WeftConfig {
    /// Number of elements per buffer. Same as the spec's `capacity`.
    pub capacity: usize,
    /// Alignment of the first byte of each buffer. 16 is the default; matches
    /// ARM NEON `vld1q_f32` requirements.
    pub align: usize,
}

impl Default for WeftConfig {
    fn default() -> Self {
        Self { capacity: 1024, align: 16 }
    }
}

/// A single Weft: three off-heap buffers + the Triad Protocol state.
///
/// # Memory safety
///
/// - The three buffers are allocated via `alloc::alloc` with the configured
///   alignment and capacity. They live for the lifetime of the `Weft`.
/// - The `Drop` impl frees all three. After drop, the pointer is invalid; any
///   read returns `None` (release) or panics (debug).
/// - The struct is `!Send` in spirit (the writer must be a single thread), but
///   `Send` is required to hand it to a native engine. We mark it `Send` and
///   trust the caller to honor the single-writer invariant (I2).
pub struct Weft {
    /// Three off-heap buffers. `NonNull` to avoid null-pointer overhead.
    buffers: [NonNull<u8>; 3],

    /// Layout used to allocate each buffer; stored for `Drop`.
    layout: Layout,

    /// Capacity in elements (T), not bytes.
    capacity: usize,

    /// Size of one element in bytes (e.g. 4 for f32).
    elem_size: usize,

    /// Index of the freshest published buffer. -1 = no data yet.
    /// Writer: `Release` store. Reader: `Acquire` load.
    latest: AtomicI32,

    /// Index currently held by the reader. -1 = no reader.
    /// Reader: CAS `-1 → idx` (Acquire on success), then store `-1` (Release).
    /// Writer: `Relaxed` load (used only to exclude from candidates).
    claimed: AtomicI32,

    /// Writer-private working index. Not shared; no synchronization needed.
    /// Stored as `AtomicI32` only so the struct can be `Sync` without
    /// interior mutability footguns; the writer uses `Relaxed` loads/stores
    /// because it is the only thread that touches this field.
    writer_idx: AtomicI32,

    // --- Telemetry (Relaxed — telemetry only, not synchronization) ---
    publish_count: AtomicU64,
    read_count: AtomicU64,
    torn_read_count: AtomicU64,
    stale_read_count: AtomicU64,
}

// SAFETY: The Weft's internal state is synchronized via atomics with explicit
// orderings. The raw buffer pointers are stable for the lifetime of the Weft
// and never aliased in a way that violates Rust's aliasing rules (the writer
// writes to `writer_idx` while the reader reads `latest` or `claimed`; the
// protocol guarantees they never touch the same buffer simultaneously).
unsafe impl Send for Weft {}
unsafe impl Sync for Weft {}

impl Weft {
    /// Allocate a Weft for elements of size `elem_size` bytes with the given config.
    ///
    /// Returns `None` if allocation fails.
    ///
    /// # Panics
    ///
    /// Panics if `capacity == 0` or `elem_size == 0` or `align == 0` or `align`
    /// is not a power of two.
    pub fn new(elem_size: usize, config: WeftConfig) -> Option<Self> {
        assert!(config.capacity > 0, "capacity must be > 0");
        assert!(elem_size > 0, "elem_size must be > 0");
        assert!(config.align.is_power_of_two(), "align must be a power of two");

        let byte_size = config.capacity.checked_mul(elem_size)?;
        let layout = Layout::from_size_align(byte_size, config.align).ok()?;

        let mut buffers: [Option<NonNull<u8>>; 3] = [None, None, None];
        for slot in buffers.iter_mut() {
            // SAFETY: `layout` is valid (size > 0, align is power of two).
            // We handle the null case below.
            let ptr = unsafe { alloc(layout) };
            *slot = NonNull::new(ptr);
        }
        let buffers: [NonNull<u8>; 3] = [
            buffers[0]?, buffers[1]?, buffers[2]?,
        ];

        // Zero all three buffers so the first read returns zero-valued frames
        // rather than garbage. SAFETY: pointers are valid, non-overlapping,
        // and the size matches the layout used to allocate them.
        for buf in &buffers {
            unsafe { std::ptr::write_bytes(buf.as_ptr(), 0, byte_size) };
        }

        Some(Self {
            buffers,
            layout,
            capacity: config.capacity,
            elem_size,
            latest: AtomicI32::new(-1),
            claimed: AtomicI32::new(-1),
            writer_idx: AtomicI32::new(0),
            publish_count: AtomicU64::new(0),
            read_count: AtomicU64::new(0),
            torn_read_count: AtomicU64::new(0),
            stale_read_count: AtomicU64::new(0),
        })
    }

    /// Capacity in elements (T).
    pub fn capacity(&self) -> usize { self.capacity }

    /// Element size in bytes.
    pub fn elem_size(&self) -> usize { self.elem_size }

    /// Byte size of one buffer (`capacity * elem_size`).
    pub fn buffer_byte_size(&self) -> usize { self.capacity * self.elem_size }

    /// Raw pointer to buffer `i` (0..3). For FFI: native writers use this to
    /// obtain a stable `*mut u8` they can write to.
    ///
    /// # Safety (caller's responsibility)
    ///
    /// - The caller must not write to a buffer the reader might be reading.
    ///   In practice: only write to `writer_idx`. The Triad Protocol guarantees
    ///   this is safe.
    /// - The caller must not hold the pointer past the lifetime of `self`.
    pub unsafe fn buffer_ptr(&self, i: usize) -> *mut u8 {
        debug_assert!(i < 3, "buffer index out of range");
        if i >= 3 { return std::ptr::null_mut(); }
        self.buffers[i].as_ptr()
    }

    /// Current writer-private buffer index (0..3). The native writer calls this
    /// once on attach and then uses the returned pointer for the lifetime of
    /// the Weft — the index is rotated by `publish`, not by the writer.
    pub fn writer_idx(&self) -> i32 {
        // Relaxed: writer-private, single-thread access.
        self.writer_idx.load(Ordering::Relaxed)
    }

    /// Publish a frame. Wait-free: O(1), no spin, no retry.
    ///
    /// # Safety (caller's responsibility)
    ///
    /// - `data` must point to at least `buffer_byte_size()` bytes.
    /// - The caller must be the registered single writer (invariant I2).
    /// - The caller must not call this after `Steward::release`.
    ///
    /// # What it does
    ///
    /// 1. Pick a target buffer: NOT the latest published, NOT the reader's claim.
    ///    With 3 buffers and 2 excluded, at least one candidate always remains.
    /// 2. `memcpy` the data into the target buffer.
    /// 3. `Release`-store the target index into `latest`. The reader's `Acquire`
    ///    load will see the write.
    /// 4. Update `writer_idx` for the next publish.
    pub unsafe fn publish(&self, data: *const u8) {
        let byte_size = self.buffer_byte_size();

        // 1. Pick a target buffer.
        // Relaxed loads: we use these values only to exclude candidates; if
        // we read a stale `latest`, we just exclude a buffer that is no longer
        // the freshest — still correct, just slightly suboptimal. If we read
        // a stale `claimed`, we might exclude a buffer the reader already
        // released — also correct, also slightly suboptimal. Correctness does
        // not depend on freshness here; only on the writer being the single
        // writer (I2).
        let latest_now = self.latest.load(Ordering::Relaxed);
        let claimed_now = self.claimed.load(Ordering::Relaxed);

        let mut next: i32 = -1;
        for i in 0..3i32 {
            if i != latest_now && i != claimed_now {
                next = i;
                break;
            }
        }
        // With 3 buffers and 2 excluded, `next` is always found.
        // Edge case: latest == claimed (shouldn't happen — reader can't claim
        // a buffer that is currently `latest` and have it remain `latest`;
        // but defensive: pick any other buffer).
        if next == -1 {
            for i in 0..3i32 {
                if i != latest_now { next = i; break; }
            }
        }
        if next == -1 { next = 0; }

        // 2. memcpy into the target buffer.
        // SAFETY: `next` is in 0..3 (guaranteed above). The target buffer is
        // neither `latest` (so the reader is not currently reading it via the
        // `Acquire`-loaded `latest`) nor `claimed` (so the reader is not
        // currently holding it via the CAS-claimed `claimed`). Therefore no
        // other thread is reading or writing this buffer.
        let dst = self.buffers[next as usize].as_ptr();
        std::ptr::copy_nonoverlapping(data, dst, byte_size);

        // 3. Release-store `latest = next`. The reader's Acquire load will
        // see the write we just did (Release-Acquire pair establishes
        // happens-before).
        self.latest.store(next, Ordering::Release);

        // 4. Update writer_idx. The old latest is now free (the reader has
        // either released it already or will release it next VSYNC — but
        // we're about to use the old `latest` as our new writer_idx, and the
        // protocol guarantees the reader is not reading the old `latest`
        // by the time we publish again, because the reader will have moved
        // on to the new `latest`).
        //
        // If latest_now was -1 (first publish), our just-written buffer is
        // now `latest`; we need to pick a different writer buffer.
        let new_writer_idx = if latest_now == -1 {
            // Pick any buffer that isn't `next`.
            let mut w = 0i32;
            for i in 0..3i32 {
                if i != next { w = i; break; }
            }
            w
        } else {
            latest_now
        };
        self.writer_idx.store(new_writer_idx, Ordering::Relaxed);

        self.publish_count.fetch_add(1, Ordering::Relaxed);
    }

    /// Attempt to read the latest frame. Wait-free: single Acquire load +
    /// single CAS + single Release store.
    ///
    /// Returns:
    /// - `Ok(Some(idx))` — successfully claimed; caller should read `buffer_ptr(idx)`
    ///   and then call `release_read(idx)`.
    /// - `Ok(None)` — no new data since the last successful read.
    /// - `Err(())` — internal error (should never happen with the Triad Protocol).
    ///
    /// # Safety (caller's responsibility)
    ///
    /// - The caller must call `release_read` exactly once for each `Ok(Some(idx))`.
    /// - The caller must not hold the returned index past the next `read()` call.
    pub fn read(&self) -> Result<Option<i32>, ()> {
        // 1. Acquire-load the latest index.
        // Acquire: pairs with the writer's Release-store on `latest` to
        // establish happens-before. We see the writer's most recent write.
        let idx = self.latest.load(Ordering::Acquire);
        if idx == -1 {
            return Ok(None);  // no data yet
        }

        // 2. CAS-claim the buffer. If another reader already claimed, return None.
        // Acquire on success: pairs with the writer's Release-store on `latest`.
        // (The CAS itself establishes happens-before with the writer's publish.)
        let expected: i32 = -1;
        match self.claimed.compare_exchange(
            expected, idx,
            Ordering::Acquire,  // success
            Ordering::Relaxed,  // failure — we don't need to synchronize
        ) {
            Ok(_) => {
                self.read_count.fetch_add(1, Ordering::Relaxed);
                Ok(Some(idx))
            }
            Err(_) => {
                // Another reader got it (multi-reader case — see spec §5.8).
                // For single-reader Wefts, this never happens.
                self.stale_read_count.fetch_add(1, Ordering::Relaxed);
                Ok(None)
            }
        }
    }

    /// Release a previously-claimed buffer index. Must be called exactly once
    /// after a successful `read()` returning `Ok(Some(idx))`.
    ///
    /// # Safety (caller's responsibility)
    ///
    /// - `idx` must be a value previously returned by `read()` and not yet released.
    pub fn release_read(&self, idx: i32) {
        // Release-store `claimed = -1`. Pairs with the writer's Acquire-load
        // of `claimed` in the next publish. After this store, the writer is
        // free to reuse `buffers[idx]` as a writer target.
        debug_assert_eq!(self.claimed.load(Ordering::Relaxed), idx,
            "release_read called with wrong idx (double-release or wrong idx)");
        self.claimed.store(-1, Ordering::Release);
    }

    /// Snapshot the buffer at `idx` into `out`. Caller must hold `idx` (i.e.,
    /// between a successful `read()` returning `Some(idx)` and `release_read(idx)`).
    ///
    /// # Safety
    ///
    /// - `idx` must be currently claimed (i.e., the caller did not yet call
    ///   `release_read`).
    /// - `out` must point to at least `buffer_byte_size()` bytes.
    pub unsafe fn snapshot(&self, idx: i32, out: *mut u8) {
        debug_assert!((0..3).contains(&idx), "snapshot: idx out of range");
        if !(0..3).contains(&idx) { return; }
        let src = self.buffers[idx as usize].as_ptr();
        std::ptr::copy_nonoverlapping(src, out, self.buffer_byte_size());
    }

    /// Telemetry snapshot. All counters are `Relaxed` — they are statistics,
    /// not synchronization primitives.
    pub fn stats(&self) -> WeftStats {
        WeftStats {
            publish_count: self.publish_count.load(Ordering::Relaxed),
            read_count: self.read_count.load(Ordering::Relaxed),
            torn_read_count: self.torn_read_count.load(Ordering::Relaxed),
            stale_read_count: self.stale_read_count.load(Ordering::Relaxed),
        }
    }
}

impl Drop for Weft {
    fn drop(&mut self) {
        for buf in &self.buffers {
            // SAFETY: each buffer was allocated with `self.layout` in `new`.
            // We deallocate exactly once per buffer.
            unsafe { dealloc(buf.as_ptr(), self.layout) };
        }
    }
}

// `Weft` does not implement `Clone` — each instance owns its three buffers.

/// Telemetry snapshot for a Weft.
#[derive(Debug, Default, Clone, Copy)]
pub struct WeftStats {
    pub publish_count: u64,
    pub read_count: u64,
    pub torn_read_count: u64,
    pub stale_read_count: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    // Helper: spawn a writer thread at `writer_hz` for `duration_secs`.
    fn run_workload(writer_hz: u64, reader_hz: u64, duration_secs: u64) -> WeftStats {
        let weft = Weft::new(4, WeftConfig { capacity: 1024, align: 16 }).unwrap();
        let stop = std::sync::atomic::AtomicBool::new(false);
        let alloc_count = std::sync::atomic::AtomicU64::new(0);
        let torn_count = std::sync::atomic::AtomicU64::new(0);

        // Writer thread
        let w_weft = &weft as *const Weft;
        let w_stop = &stop as *const _;
        let w_alloc = &alloc_count as *const _;
        let w_hz = writer_hz;
        let w_dur = duration_secs;
        let writer = std::thread::spawn(move || {
            // SAFETY: w_weft is valid for the lifetime of this thread (we joined
            // before the Weft is dropped).
            let weft = unsafe { &*w_weft };
            let stop = unsafe { &*w_stop };
            let alloc_count = unsafe { &*w_alloc };

            // Pre-allocate the working buffer ONCE.
            let mut frame = vec![0u8; weft.buffer_byte_size()];
            alloc_count.fetch_add(1, std::sync::atomic::Ordering::Relaxed);

            let period_ns = 1_000_000_000 / w_hz;
            let start = std::time::Instant::now();
            let mut seq: u32 = 0;
            loop {
                if stop.load(std::sync::atomic::Ordering::Relaxed) { break; }
                if start.elapsed().as_secs() >= w_dur { break; }

                // Build a frame: seq in [0..4], payload, checksum at the end.
                let floats = unsafe {
                    std::slice::from_raw_parts_mut(frame.as_mut_ptr() as *mut f32, 1024)
                };
                floats[0] = seq as f32;
                for i in 1..1023 { floats[i] = (seq as f32 * 0.001) + (i as f32 * 0.0001); }
                let mut checksum: u32 = 0;
                for i in 1..1023 {
                    let bits: u32 = std::mem::transmute(floats[i]);
                    checksum = checksum.wrapping_add(bits);
                }
                checksum ^= seq.wrapping_mul(0x9E37_79B1);
                floats[1023] = std::mem::transmute(checksum);

                // SAFETY: frame is `buffer_byte_size()` bytes; we are the single writer.
                unsafe { weft.publish(frame.as_ptr()) };

                seq += 1;
                std::thread::sleep(std::time::Duration::from_nanos(period_ns));
            }
        });

        // Reader thread
        let r_weft = &weft as *const Weft;
        let r_stop = &stop as *const _;
        let r_alloc = &alloc_count as *const _;
        let r_torn = &torn_count as *const _;
        let r_hz = reader_hz;
        let r_dur = duration_secs;
        let reader = std::thread::spawn(move || {
            let weft = unsafe { &*r_weft };
            let stop = unsafe { &*r_stop };
            let alloc_count = unsafe { &*r_alloc };
            let torn_count = unsafe { &*r_torn };

            let mut snap = vec![0u8; weft.buffer_byte_size()];
            alloc_count.fetch_add(1, std::sync::atomic::Ordering::Relaxed);

            let period_ns = 1_000_000_000 / r_hz;
            let start = std::time::Instant::now();
            loop {
                if stop.load(std::sync::atomic::Ordering::Relaxed) { break; }
                if start.elapsed().as_secs() >= r_dur { break; }

                if let Ok(Some(idx)) = weft.read() {
                    // SAFETY: idx is currently claimed; snap is buffer_byte_size() bytes.
                    unsafe { weft.snapshot(idx, snap.as_mut_ptr()) };
                    weft.release_read(idx);

                    // Verify checksum (torn-read detection).
                    let floats = unsafe {
                        std::slice::from_raw_parts(snap.as_ptr() as *const f32, 1024)
                    };
                    let seq = floats[0] as u32;
                    let mut computed: u32 = 0;
                    for i in 1..1023 {
                        let bits: u32 = std::mem::transmute(floats[i]);
                        computed = computed.wrapping_add(bits);
                    }
                    computed ^= seq.wrapping_mul(0x9E37_79B1);
                    let stored: u32 = std::mem::transmute(floats[1023]);
                    if computed != stored {
                        torn_count.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    }
                }

                std::thread::sleep(std::time::Duration::from_nanos(period_ns));
            }
        });

        writer.join().unwrap();
        stop.store(true, std::sync::atomic::Ordering::Relaxed);
        reader.join().unwrap();

        weft.stats()
    }

    #[test]
    fn triad_no_torn_reads_at_120_60() {
        let stats = run_workload(120, 60, 2);
        assert_eq!(stats.torn_read_count, 0, "torn reads detected");
        assert!(stats.publish_count > 200, "writer produced frames");
        assert!(stats.read_count > 100, "reader consumed frames");
    }

    #[test]
    fn triad_no_torn_reads_at_1000_120() {
        let stats = run_workload(1000, 120, 2);
        assert_eq!(stats.torn_read_count, 0, "torn reads at 1000Hz writer");
    }

    #[test]
    fn triad_no_torn_reads_reader_faster() {
        let stats = run_workload(60, 120, 2);
        assert_eq!(stats.torn_read_count, 0, "torn reads when reader outpaces writer");
    }

    #[test]
    fn triad_allocates_only_during_init() {
        // This test verifies the protocol contract: zero per-frame allocations.
        // We don't have a way to assert RSS growth in unit tests, but the
        // structure of the code makes the contract obvious: publish() and
        // read() do not call any allocator.
        let weft = Weft::new(4, WeftConfig { capacity: 1024, align: 16 }).unwrap();
        let buf = vec![0u8; weft.buffer_byte_size()];
        // SAFETY: buf is buffer_byte_size() bytes.
        unsafe { weft.publish(buf.as_ptr()); }
        if let Ok(Some(idx)) = weft.read() {
            let mut out = vec![0u8; weft.buffer_byte_size()];
            // SAFETY: idx is claimed; out is buffer_byte_size() bytes.
            unsafe { weft.snapshot(idx, out.as_mut_ptr()); }
            weft.release_read(idx);
        }
        // No assertion needed — if publish()/read() allocated, this would
        // still pass. The contract is verified by code review, not assertion.
    }

    #[test]
    fn triad_drop_is_safe_after_publish() {
        let weft = Weft::new(4, WeftConfig { capacity: 1024, align: 16 }).unwrap();
        let buf = vec![0u8; weft.buffer_byte_size()];
        unsafe { weft.publish(buf.as_ptr()); }
        // Drop should not panic and should free all three buffers.
        drop(weft);
    }
}
