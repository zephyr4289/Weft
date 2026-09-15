//! The Steward: lifecycle manager for Wefts.
//!
//! The Steward allocates, binds, frees, and leak-detects Wefts. It is the
//! answer to "what happens to off-heap memory when the composition is destroyed?"
//! (spec §7.3).
//!
//! The Steward is `Send + Sync` and can be held by a `ViewModel` (Android),
//! `@StateObject` (iOS), or `useRef` (Web). It survives configuration change.
//! The composition borrows a `Weft` from the Steward; the binding is destroyed
//! on dispose, but the buffer itself survives.

use crate::triad::{Weft, WeftConfig, WeftStats};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicU64, Ordering};

/// A unique identifier for a Weft. Minted by the Steward; never reused.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct WeftId(u64);

/// Statistics snapshot for the Steward (aggregated across all owned Wefts).
#[derive(Debug, Default, Clone, Copy)]
pub struct StewardStats {
    pub weft_count: usize,
    pub total_publishes: u64,
    pub total_reads: u64,
    pub total_torn_reads: u64,
    pub total_stale_reads: u64,
}

/// A leak trace. Returned by `Steward::dump_leaks()` when a Weft survives
/// its scope. Contains the Weft's id, capacity, and the stack trace at bind time.
#[derive(Debug, Clone)]
pub struct LeakTrace {
    pub weft_id: WeftId,
    pub capacity: usize,
    pub elem_size: usize,
    pub bound_at: std::time::SystemTime,
    pub stack_trace: String,
}

/// Internal record for each Weft the Steward owns.
struct Record {
    weft: Weft,
    weft_id: WeftId,            // Stored here so dump_leaks can report it.
    bound_at: std::time::SystemTime,
    stack_trace: String,
    /// `false` while bound; `true` after `release()`. If the Steward is dropped
    /// while any record is still `bound == false`, those are leaks (in debug).
    released: bool,
}

/// The Steward. Owns Wefts; allocates and frees them.
///
/// Thread-safe. Use `clone()` to share via `Arc<Steward>`; the underlying
/// records are shared via an internal `Arc<Mutex<...>>`.
#[derive(Clone)]
pub struct Steward {
    inner: Arc<Mutex<HashMap<WeftId, Record>>>,
    next_id: Arc<AtomicU64>,
}

impl Steward {
    /// Create a new, empty Steward.
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(HashMap::new())),
            next_id: Arc::new(AtomicU64::new(1)),
        }
    }

    /// Allocate a Weft for elements of size `elem_size` bytes with the given config.
    /// The Weft is immediately tracked by the Steward. Returns its id.
    ///
    /// # Panics
    ///
    /// Panics if allocation fails (OOM). Use `try_weft()` for the fallible version.
    pub fn weft(&self, elem_size: usize, config: WeftConfig) -> WeftId {
        self.try_weft(elem_size, config)
            .expect("weft: allocation failed (OOM)")
    }

    /// Fallible version of `weft()`.
    pub fn try_weft(&self, elem_size: usize, config: WeftConfig) -> Option<WeftId> {
        let weft = Weft::new(elem_size, config)?;
        let id = WeftId(self.next_id.fetch_add(1, Ordering::Relaxed));
        let record = Record {
            weft,
            weft_id: id,
            bound_at: std::time::SystemTime::now(),
            stack_trace: capture_stack_trace(),
            released: false,
        };
        self.inner.lock().unwrap().insert(id, record);
        Some(id)
    }

    /// Borrow a Weft by id. Returns a guard that derefs to `&Weft`.
    ///
    /// The guard holds the Steward's mutex for its lifetime; the Weft cannot
    /// be removed or released while the guard exists. This is the standard
    /// MutexGuard-deref pattern, applied to the Weft inside the HashMap.
    pub fn borrow(&self, id: WeftId) -> Option<WeftGuard<'_>> {
        let mut guard = self.inner.lock().unwrap();
        let record = guard.get_mut(&id)?;
        if record.released {
            return None;
        }
        Some(WeftGuard {
            weft: &mut record.weft,
            _mutex_guard: guard,
        })
    }

    /// Release a Weft. Frees the underlying off-heap buffers and removes the
    /// record from the Steward. After release, `borrow(id)` returns `None`.
    pub fn release(&self, id: WeftId) {
        let mut map = self.inner.lock().unwrap();
        if let Some(mut record) = map.remove(&id) {
            record.released = true;
            // `record.weft` is dropped here (when `record` goes out of scope),
            // freeing the three buffers via Weft::drop.
            drop(record);
        }
    }

    /// Release all Wefts owned by this Steward.
    pub fn release_all(&self) {
        let mut map = self.inner.lock().unwrap();
        for (_, mut record) in map.drain() {
            record.released = true;
            drop(record);  // frees the weft
        }
    }

    /// Aggregate stats across all owned Wefts.
    pub fn stats(&self) -> StewardStats {
        let map = self.inner.lock().unwrap();
        let mut total_publishes = 0;
        let mut total_reads = 0;
        let mut total_torn = 0;
        let mut total_stale = 0;
        for record in map.values() {
            let s = record.weft.stats();
            total_publishes += s.publish_count;
            total_reads += s.read_count;
            total_torn += s.torn_read_count;
            total_stale += s.stale_read_count;
        }
        StewardStats {
            weft_count: map.len(),
            total_publishes,
            total_reads,
            total_torn_reads: total_torn,
            total_stale_reads: total_stale,
        }
    }

    /// Debug-only. Returns leak traces for any Weft that survived its scope
    /// (i.e., was never `release()`d). In production, this returns an empty
    /// vec because Drop auto-frees. The method exists for the spec's L5
    /// invariant: leak detection.
    pub fn dump_leaks(&self) -> Vec<LeakTrace> {
        let map = self.inner.lock().unwrap();
        map.values()
            .filter(|r| !r.released)
            .map(|r| LeakTrace {
                weft_id: r.weft_id,
                capacity: r.weft.capacity(),
                elem_size: r.weft.elem_size(),
                bound_at: r.bound_at,
                stack_trace: r.stack_trace.clone(),
            })
            .collect()
    }

    /// Debug-only. Panics if any Weft is still bound (un-released).
    pub fn assert_no_leaks(&self) {
        let leaks = self.dump_leaks();
        if !leaks.is_empty() {
            panic!("Steward has {} leaked Wefts: {:#?}", leaks.len(), leaks);
        }
    }
}

impl Default for Steward {
    fn default() -> Self { Self::new() }
}

impl Drop for Steward {
    fn drop(&mut self) {
        // Auto-release all Wefts on Steward drop. This is the safety net for
        // spec invariant L4: composition-bound lifetime. If the composition
        // (and therefore the Steward's ViewModel) is destroyed, all Wefts
        // are freed. No leak.
        self.release_all();
    }
}

/// A guard that gives access to a borrowed Weft. Holds the Steward's mutex
/// for the guard's lifetime; the Weft cannot be removed while the guard exists.
pub struct WeftGuard<'a> {
    weft: &'a mut Weft,
    _mutex_guard: std::sync::MutexGuard<'a, HashMap<WeftId, Record>>,
}

impl<'a> std::ops::Deref for WeftGuard<'a> {
    type Target = Weft;
    fn deref(&self) -> &Weft { self.weft }
}

impl<'a> std::ops::DerefMut for WeftGuard<'a> {
    fn deref_mut(&mut self) -> &mut Weft { self.weft }
}

/// Capture a stack trace at the current point. Uses `std::backtrace` (stable
/// since Rust 1.65). Returns the trace as a string.
fn capture_stack_trace() -> String {
    format!("{}", std::backtrace::Backtrace::force_capture())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn steward_allocates_and_frees() {
        let s = Steward::new();
        let id = s.weft(4, WeftConfig { capacity: 1024, align: 16 });
        assert_eq!(s.stats().weft_count, 1);
        s.release(id);
        assert_eq!(s.stats().weft_count, 0);
    }

    #[test]
    fn steward_release_all_on_drop() {
        let s = Steward::new();
        let _id1 = s.weft(4, WeftConfig::default());
        let _id2 = s.weft(4, WeftConfig::default());
        // Steward dropped here; both Wefts should be freed via release_all.
        drop(s);
    }

    #[test]
    fn steward_borrow_after_release_returns_none() {
        let s = Steward::new();
        let id = s.weft(4, WeftConfig::default());
        s.release(id);
        assert!(s.borrow(id).is_none());
    }

    #[test]
    fn steward_multi_weft_isolation() {
        let s = Steward::new();
        let id1 = s.weft(4, WeftConfig { capacity: 1024, align: 16 });
        let id2 = s.weft(4, WeftConfig { capacity: 2048, align: 16 });
        let g1 = s.borrow(id1).unwrap();
        let g2 = s.borrow(id2).unwrap();
        assert_ne!(g1.buffer_byte_size(), g2.buffer_byte_size());
    }

    #[test]
    fn steward_dump_leaks_empty_when_released() {
        let s = Steward::new();
        let id = s.weft(4, WeftConfig::default());
        s.release(id);
        assert!(s.dump_leaks().is_empty());
    }

    #[test]
    fn steward_dump_leaks_reports_unreleased() {
        let s = Steward::new();
        let _id = s.weft(4, WeftConfig::default());
        // Don't release. dump_leaks should report it.
        let leaks = s.dump_leaks();
        assert_eq!(leaks.len(), 1);
        // Release now to avoid the leak actually happening (Drop would catch it).
        s.release_all();
    }
}
