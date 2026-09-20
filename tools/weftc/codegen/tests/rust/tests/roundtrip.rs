//! Cross-language bit-exact roundtrip (Rust leg).
//!
//! Reads the stage-1 bins written by the C test binary (tests/c_roundtrip.c
//! `stage1`), projects them with the generated zero-copy API, asserts every
//! field against the SAME canonical values the C side asserted, then mutates
//! each frame with the const-fn builders and writes stage-2 bins that the C
//! binary re-verifies (`verify-stage2`). Three execution domains, one buffer,
//! zero copies.

use weftc_codegen_tests::audio_peak::AudioPeak;
use weftc_codegen_tests::camera_exposure::CameraExposure;
use weftc_codegen_tests::mcu_status::McuStatus;
use weftc_codegen_tests::sensor_event::SensorEvent;
use weftc_codegen_tests::telemetry_frame::TelemetryFrame;
use weftc_codegen_tests::weft_projection_core::{WeftError, WeftF16};

use std::env;
use std::fs;
use std::path::PathBuf;

fn stage_dir(which: &str) -> PathBuf {
    let key = format!("WEFT_{}_DIR", which.to_uppercase());
    if let Ok(v) = env::var(&key) {
        return PathBuf::from(v);
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../out").join(which.to_lowercase())
}

fn read_bin(which: &str, file: &str) -> Vec<u8> {
    let p = stage_dir(which).join(file);
    fs::read(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e))
}

fn write_bin(which: &str, file: &str, bytes: &[u8]) {
    let dir = stage_dir(which);
    fs::create_dir_all(&dir).unwrap();
    let p = dir.join(file);
    fs::write(&p, bytes).unwrap_or_else(|e| panic!("cannot write {}: {}", p.display(), e));
}

// ---------------------------------------------------------------------------
// stage 1: C wrote these bytes — Rust must read them bit-exact
// ---------------------------------------------------------------------------

#[test]
fn stage1_telemetry_frame_bit_exact() {
    let bytes = read_bin("stage1", "telemetry_frame.bin");
    assert_eq!(bytes.len(), TelemetryFrame::SIZE);

    let f = TelemetryFrame::from_bytes(&bytes).expect("cast ok");
    assert_eq!(f.schema_id, TelemetryFrame::SCHEMA_ID);
    assert_eq!(f.schema_id, 0x8F4C_1120_A9B3_0012); // the Pillar 1 example hash
    assert_eq!(f.velocity, 12.5);
    assert_eq!(f.altitude, 5400.25);
    assert_eq!(f.accel, [0.5, -1.25, 9.8]);
    assert_eq!(f.pad0, 0xDEAD_BEEF);
    assert_eq!(f.gyro, [0.01, -0.02, 0.03]);
    assert_eq!(f.baro_pressure, 101325.0);
    assert_eq!(f.quaternion, [1.0, 0.0, 0.0, 0.0]);

    // refusal modes
    let short = &bytes[..TelemetryFrame::SIZE - 1];
    assert_eq!(TelemetryFrame::from_bytes(short), Err(WeftError::ShortBuffer));
    let mut misaligned = vec![0u8; TelemetryFrame::SIZE + 1];
    misaligned[1..].copy_from_slice(&bytes);
    assert_eq!(TelemetryFrame::from_bytes(&misaligned[1..]), Err(WeftError::BadAlign));
    let mut corrupted = bytes.clone();
    corrupted[0] ^= 0xFF;
    assert_eq!(TelemetryFrame::from_bytes(&corrupted), Err(WeftError::SchemaMismatch));

    // alignment-free read from the misaligned copy (Law 3) — same values
    let packed = TelemetryFrame::read_packed(&misaligned[1..]).expect("packed read");
    assert_eq!(packed.velocity, 12.5);
    assert_eq!(packed.quaternion, [1.0, 0.0, 0.0, 0.0]);

    // as_bytes reproduces the exact buffer
    assert_eq!(f.as_bytes(), &bytes[..]);
}

#[test]
fn stage1_mcu_status_bit_exact() {
    let bytes = read_bin("stage1", "mcu_status.bin");
    let m = McuStatus::from_bytes(&bytes).expect("cast ok");
    assert_eq!(m.mode(), 2);
    assert_eq!(m.armed(), 1);
    assert_eq!(m.error_code(), 0x5A);
    assert_eq!(m.vbus_mv, 11800);
    assert_eq!(m.temp_c, WeftF16(0x5150)); // f16(42.5)
    assert_eq!(m.gyro_temp_c, WeftF16(0x5138)); // f16(41.75)
    assert_eq!(m.motor_rpm, [1200, 1250, 1180, 1225]);
    assert_eq!(m.crc32, 0xCAFE_BABE);

    // setters preserve neighbors (same invariant the C side proved)
    let mut n = *m;
    n.set_mode(1);
    assert_eq!((n.mode(), n.error_code()), (1, 0x5A));
    n.set_mode(2);
    assert_eq!(n, *m);
}

#[test]
fn stage1_camera_exposure_bit_exact() {
    let bytes = read_bin("stage1", "camera_exposure.bin");
    let c = CameraExposure::from_bytes(&bytes).expect("cast ok");
    assert_eq!(c.schema_id, CameraExposure::SCHEMA_ID);
    assert_eq!(c.ae_lock(), 1);
    assert_eq!(c.sensor(), 5);
    assert_eq!(c.mode, 0xB); // bitfield packing (ae_lock | sensor << 1)
    assert_eq!(c.gain, 1.75);
    assert_eq!(c.exposure_us, 8333.0);
    assert_eq!(c.position, [1.0, 2.0, 3.0, 4.0]);
    let mut ident = [0.0f32; 16];
    ident[0] = 1.0;
    ident[5] = 1.0;
    ident[10] = 1.0;
    ident[12] = 5.0;
    ident[13] = 6.0;
    ident[14] = 7.0;
    ident[15] = 1.0;
    assert_eq!(c.projection, ident);
    assert_eq!(c.roi, [[10.0, 20.0, 30.0, 40.0], [50.0, 60.0, 70.0, 80.0]]);
}

#[test]
fn stage1_sensor_event_bit_exact() {
    let bytes = read_bin("stage1", "sensor_event.bin");
    let e = SensorEvent::from_bytes(&bytes).expect("cast ok");
    assert_eq!(e.timestamp_ns, 0x1122_3344_5566_7788);
    assert_eq!(e.imu.x, -123);
    assert_eq!(e.imu.y, 456);
    assert_eq!(e.imu.z, -789);
    assert_eq!(e.imu.pad, 0x4242);
    assert_eq!(e.quality, 0xC0FF_EE);
    assert_eq!(e.crc32, 0x1234_ABCD);
}

#[test]
fn stage1_audio_peak_bit_exact() {
    let bytes = read_bin("stage1", "audio_peak.bin");
    let a = AudioPeak::from_bytes(&bytes).expect("cast ok");
    let want = [
        WeftF16::from_f32(-6.5),
        WeftF16::from_f32(-12.25),
        WeftF16::from_f32(-3.75),
        WeftF16::from_f32(-20.0),
    ];
    assert_eq!(a.levels_db, want);
    // f16 codec spot-agrees with the C core's constants
    assert_eq!(a.levels_db[0].0, 0xC680); // -6.5
    assert_eq!(WeftF16::from_f32(1.0).0, 0x3C00);
    assert_eq!(WeftF16::from_f32(-2.75).0, 0xC180);
    assert_eq!(WeftF16::from_f32(0.1).0, 0x2E66);
    assert_eq!(WeftF16(0x3C00).to_f32(), 1.0);
}

#[test]
fn f16_exhaustive_identity() {
    // f16 -> f32 -> f16 must be the identity for every non-NaN bit pattern
    // (the C core proves the same 65536 patterns from its side)
    let mut bad = 0u32;
    for h in 0u32..=0xFFFF {
        let bits = h as u16;
        if (bits >> 10) & 0x1F == 0x1F {
            continue; // NaN exponent — payload not preserved by design
        }
        let f = WeftF16(bits).to_f32();
        if WeftF16::from_f32(f).0 != bits {
            bad += 1;
        }
    }
    assert_eq!(bad, 0, "f16 exhaustive identity failed for {} patterns", bad);
}

#[test]
fn batch_scan_helper() {
    // the core's generic batch validator over packed sensor events
    let one_bytes = read_bin("stage1", "sensor_event.bin");
    let one = SensorEvent::from_bytes(&one_bytes).unwrap();
    let mut buf = Vec::new();
    for _ in 0..8 {
        buf.extend_from_slice(one.as_bytes());
    }
    // corrupt frame 5
    let at = 5 * SensorEvent::SIZE;
    buf[at] ^= 0xFF;

    let (ok, bad) = weftc_codegen_tests::weft_projection_core::weft_validate_batch::<32>(
        &buf,
        |b| {
            let mut id = 0u64;
            for i in 0..8 {
                id |= (b[i] as u64) << (8 * i);
            }
            id
        },
        SensorEvent::SCHEMA_ID,
    );
    assert_eq!((ok, bad), (5, Some(5)));
}

// ---------------------------------------------------------------------------
// stage 2: Rust mutates via the const-fn builders; C re-verifies bit-exact
// ---------------------------------------------------------------------------

#[test]
fn stage2_write_mutations() {
    // telemetry: velocity 99.5, accel[0] 2.5, quaternion[3] -1.0
    {
        let bytes = read_bin("stage1", "telemetry_frame.bin");
        let f = TelemetryFrame::from_bytes(&bytes).unwrap();
        let g = f
            .with_velocity(99.5)
            .with_accel([2.5, -1.25, 9.8])
            .with_quaternion([1.0, 0.0, 0.0, -1.0]);
        let mut out = [0u8; TelemetryFrame::SIZE];
        g.write_packed(&mut out).unwrap();
        assert_eq!(&out[..], g.as_bytes());
        write_bin("stage2", "telemetry_frame.bin", &out);
    }
    // mcu: mode 3, temp -10.5 (f16), motor_rpm[2] 999
    {
        let bytes = read_bin("stage1", "mcu_status.bin");
        let m = McuStatus::from_bytes(&bytes).unwrap();
        let n = m
            .with_mode(3)
            .with_temp_c(WeftF16::from_f32(-10.5))
            .with_motor_rpm([1200, 1250, 999, 1225]);
        let mut out = [0u8; McuStatus::SIZE];
        n.write_packed(&mut out).unwrap();
        write_bin("stage2", "mcu_status.bin", &out);
    }
    // camera: gain 3.5, roi[1] = [1,2,3,4]
    {
        let bytes = read_bin("stage1", "camera_exposure.bin");
        let c = CameraExposure::from_bytes(&bytes).unwrap();
        let d = c
            .with_gain(3.5)
            .with_roi([[10.0, 20.0, 30.0, 40.0], [1.0, 2.0, 3.0, 4.0]]);
        let mut out = [0u8; CameraExposure::SIZE];
        d.write_packed(&mut out).unwrap();
        write_bin("stage2", "camera_exposure.bin", &out);
    }
    // sensor: quality 0x0BADBEEF, imu.y 777
    {
        let bytes = read_bin("stage1", "sensor_event.bin");
        let e = SensorEvent::from_bytes(&bytes).unwrap();
        use weftc_codegen_tests::axis_sample::AxisSample;
        let f = e
            .with_quality(0x0BAD_BEEF)
            .with_imu(AxisSample { x: -123, y: 777, z: -789, pad: 0x4242 });
        let mut out = [0u8; SensorEvent::SIZE];
        f.write_packed(&mut out).unwrap();
        write_bin("stage2", "sensor_event.bin", &out);
    }
    // audio: levels_db[2] = f16(-1.5)
    {
        let bytes = read_bin("stage1", "audio_peak.bin");
        let a = AudioPeak::from_bytes(&bytes).unwrap();
        let mut levels = a.levels_db;
        levels[2] = WeftF16::from_f32(-1.5);
        let b = a.with_levels_db(levels);
        let mut out = [0u8; AudioPeak::SIZE];
        b.write_packed(&mut out).unwrap();
        write_bin("stage2", "audio_peak.bin", &out);
    }
}
