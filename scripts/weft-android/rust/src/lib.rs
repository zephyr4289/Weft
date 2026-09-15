//! # weft-core
//!
//! Off-heap, zero-copy, draw-phase-read channel for declarative UI frameworks.
//! Implements the Weft Continuous-State Plane Specification v0.1.
//!
//! ## Architecture
//!
//! The crate is split into three modules:
//! - [`triad`]: the Triad Protocol. Three off-heap buffers + two atomics + a
//!   writer-private index. Wait-free on both sides. No torn reads.
//! - [`steward`]: lifecycle manager. Allocates, binds, frees, leak-detects.
//! - [`jni_bridge`]: panic-shielded JNI surface (feature-gated).
//!
//! ## Memory model
//!
//! Every atomic operation has explicit ordering and a `SAFETY` comment justifying
//! the choice. There are no `Relaxed` loads that should be `Acquire`, and no
//! `Release` stores missing a matching `Acquire` on the read side.
//!
//! ## Invariants (from spec §5.5)
//!
//! - I1: No torn reads — reader always loads a fully-written buffer.
//! - I2: Wait-free writer — O(1) per publish, no spin, no retry.
//! - I3: Wait-free reader — O(1) per read, single CAS + single store.
//! - I4: Latest-wins — reader sees the freshest available frame.
//! - I5: No back-pressure — writer never waits for reader; reader never waits for writer.
//!
//! All five are verified empirically by the C spike (see `weft-spike/` directory).

#![deny(rust_2021_compatibility)]
#![deny(missing_docs)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![forbid(unsafe_op_in_unsafe_fn)]

pub mod triad;
pub mod steward;

#[cfg(feature = "jni")]
pub mod jni_bridge;

pub use triad::{Weft, WeftConfig};
pub use steward::{Steward, StewardStats, LeakTrace};

/// Version of the Weft specification this crate implements.
pub const SPEC_VERSION: &str = "0.1.0";
