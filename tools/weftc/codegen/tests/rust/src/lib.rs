//! weftc-codegen-tests — mounts every weftc-generated Rust module.
//!
//! This lib is `#![no_std]`: compiling it IS the Law-2 proof that generated
//! projection modules run on bare-metal / MCU / kernel targets with `core`
//! only. The integration tests (tests/roundtrip.rs) are a separate std crate
//! that performs the cross-language bit-exact protocol against the C binary.
//!
//! Generated files are copied into src/gen/ by tests/run_tests.sh before
//! `cargo test` (kept out of git; the canonical copies are tests/golden/).

#![no_std]

#[path = "gen/weft_projection_core.rs"]
pub mod weft_projection_core;

#[path = "gen/telemetry_frame.rs"]
pub mod telemetry_frame;

#[path = "gen/mcu_status.rs"]
pub mod mcu_status;

#[path = "gen/camera_exposure.rs"]
pub mod camera_exposure;

#[path = "gen/axis_sample.rs"]
pub mod axis_sample;

#[path = "gen/sensor_event.rs"]
pub mod sensor_event;

#[path = "gen/audio_peak.rs"]
pub mod audio_peak;
