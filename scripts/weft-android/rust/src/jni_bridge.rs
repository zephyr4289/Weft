//! JNI bridge: panic-shielded entry points for the JVM to call.
//!
//! Every JNI function is wrapped in `catch_unwind` and converts panics to error
//! codes. No native panic can crash the JVM. This is the spec §7.1 L4 invariant.
//!
//! The JNI surface matches the Kotlin `TriadNative` external function declarations.
//! See `weft/src/main/kotlin/dev/weft/TriadNative.kt`.
//!
//! # Build
//!
//! This module is only compiled when the `jni` feature is enabled. Build
//! for Android with `cargo ndk -t arm64-v8a build --release --features jni`.

use crate::steward::{Steward, WeftId};
use crate::triad::WeftConfig;
use jni::objects::{JClass, JObject};
use jni::sys::{jlong, jint, jboolean, JNI_TRUE, JNI_FALSE};
use jni::JNIEnv;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, OnceLock};
use std::sync::atomic::{AtomicU64, Ordering};
use std::collections::HashMap;

// --- Handle table ---
//
// The JVM holds opaque `jlong` handles; we map them to Rust objects via a
// global handle table. This avoids passing raw pointers across the FFI
// boundary (safer) and lets us centralize lifetime management.

struct HandleTable<T> {
    next: AtomicU64,
    table: Mutex<HashMap<u64, T>>,
}

impl<T> HandleTable<T> {
    fn new() -> Self {
        Self {
            next: AtomicU64::new(1),
            table: Mutex::new(HashMap::new()),
        }
    }
    fn insert(&self, value: T) -> u64 {
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        self.table.lock().unwrap().insert(id, value);
        id
    }
    fn remove(&self, handle: u64) -> Option<T> {
        self.table.lock().unwrap().remove(&handle)
    }
    fn with<R>(&self, handle: u64, f: impl FnOnce(&T) -> R) -> Option<R> {
        let table = self.table.lock().unwrap();
        table.get(&handle).map(f)
    }
}

fn stewards() -> &'static HandleTable<Steward> {
    static T: OnceLock<HandleTable<Steward>> = OnceLock::new();
    T.get_or_init(HandleTable::new)
}

fn wefts() -> &'static HandleTable<WeftHandle> {
    static T: OnceLock<HandleTable<WeftHandle>> = OnceLock::new();
    T.get_or_init(HandleTable::new)
}

/// Internal: a Weft handle that knows its Steward + WeftId.
#[derive(Clone)]
struct WeftHandle {
    steward_id: u64,
    weft_id: WeftId,
}

// --- JNI entry points ---
//
// Each function:
// 1. Is `#[no_mangle]` and `pub extern "system"` (the JNI calling convention).
// 2. Wraps its body in `catch_unwind`.
// 3. Returns a sentinel (0, -1, or JNI_FALSE) on panic.
// 4. Logs the panic to logcat via the `log` crate (android_logger backend
//    is wired up in `lib.rs`'s android init hook — to be added in v0.2).

/// Create a Steward. Returns a `jlong` handle, or 0 on failure.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_stewardCreate(
    _env: JNIEnv, _class: JClass,
) -> jlong {
    shield(0, || {
        let s = Steward::new();
        stewards().insert(s) as jlong
    })
}

/// Destroy a Steward by handle. Frees all owned Wefts.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_stewardDestroy(
    _env: JNIEnv, _class: JClass, handle: jlong,
) {
    shield((), || {
        if let Some(s) = stewards().remove(handle as u64) {
            drop(s);  // Drop calls release_all.
        }
    });
}

/// Allocate a Weft for elements of `elem_size` bytes with the given capacity and alignment.
/// Returns a `jlong` handle, or 0 on failure.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_stewardWeft(
    _env: JNIEnv, _class: JClass, steward_handle: jlong,
    elem_size: jint, capacity: jint, align: jint,
) -> jlong {
    shield(0, || {
        let sh = steward_handle as u64;
        let id_opt = stewards().with(sh, |s| {
            let config = WeftConfig {
                capacity: capacity as usize,
                align: align as usize,
            };
            s.try_weft(elem_size as usize, config)
        });
        match id_opt {
            Some(Some(weft_id)) => {
                let handle = WeftHandle { steward_id: sh, weft_id };
                wefts().insert(handle) as jlong
            }
            _ => 0,
        }
    })
}

/// Release a Weft by handle.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_weftRelease(
    _env: JNIEnv, _class: JClass, weft_handle: jlong,
) {
    shield((), || {
        if let Some(h) = wefts().remove(weft_handle as u64) {
            stewards().with(h.steward_id, |s| s.release(h.weft_id));
        }
    });
}

/// Get the writer's working buffer as a DirectByteBuffer. The JVM does not own
/// this memory; the Weft does. The native engine writes via this buffer's
/// address; the Steward frees it on Weft release.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_weftWriterBuffer(
    env: JNIEnv, _class: JClass, weft_handle: jlong,
) -> JObject<'static> {
    let default = JObject::null();
    shield(default, || {
        let h = wefts().with(weft_handle as u64, |h| h.clone())?;
        let buf_size = stewards().with(h.steward_id, |s| {
            s.borrow(h.weft_id).map(|g| {
                let idx = g.writer_idx() as usize;
                let byte_size = g.buffer_byte_size();
                // SAFETY: idx is in 0..3 (enforced by Weft::writer_idx).
                let ptr = unsafe { g.buffer_ptr(idx) };
                (ptr as *mut u8, byte_size)
            })
        })?;
        let (ptr, byte_size) = buf_size?;
        // Create a DirectByteBuffer wrapping the native memory.
        // SAFETY: ptr is valid for the lifetime of the Weft; byte_size matches
        // the allocated layout. The JVM does not own this memory.
        env.new_direct_byte_buffer(ptr, byte_size as jlong)
            .map(|o| o.into())
            .unwrap_or(default)
    })
}

/// Publish a frame. `data` is a direct ByteBuffer of size `buffer_byte_size()`.
/// Copies data into the writer's working buffer, then atomically publishes it
/// via the Triad Protocol.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_weftPublish(
    env: JNIEnv, _class: JClass, weft_handle: jlong, data: JObject,
) {
    shield((), || {
        let direct = env.get_direct_buffer_address(&data);
        let ptr = match direct {
            Ok(Some(p)) => p as *const u8,
            _ => {
                log::warn!("weftPublish: data is not a direct ByteBuffer");
                return;
            }
        };
        let h = match wefts().with(weft_handle as u64, |h| h.clone()) {
            Some(h) => h,
            None => return,
        };
        stewards().with(h.steward_id, |s| {
            if let Some(g) = s.borrow(h.weft_id) {
                // SAFETY: `ptr` is the address of `data`'s direct buffer; we
                // copy `buffer_byte_size()` bytes from it into the writer's
                // working buffer. The writer is the single writer (I2).
                unsafe { g.publish(ptr); }
            }
        });
    });
}

/// Read the latest frame. Copies into `out` (a direct ByteBuffer).
/// Returns JNI_TRUE if a frame was read; JNI_FALSE if no new data.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_weftRead(
    env: JNIEnv, _class: JClass, weft_handle: jlong, out: JObject,
) -> jboolean {
    shield(JNI_FALSE, || {
        let direct = env.get_direct_buffer_address(&out);
        let ptr = match direct {
            Ok(Some(p)) => p as *mut u8,
            _ => return JNI_FALSE,
        };
        let h = match wefts().with(weft_handle as u64, |h| h.clone()) {
            Some(h) => h,
            None => return JNI_FALSE,
        };
        let result = stewards().with(h.steward_id, |s| {
            s.borrow(h.weft_id).map(|g| {
                match g.read() {
                    Ok(Some(idx)) => {
                        // SAFETY: idx is currently claimed; ptr is buffer_byte_size() bytes.
                        unsafe { g.snapshot(idx, ptr); }
                        g.release_read(idx);
                        JNI_TRUE
                    }
                    _ => JNI_FALSE,
                }
            })
        });
        result.flatten().unwrap_or(JNI_FALSE)
    })
}

/// Telemetry: returns the publish count.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_weftPublishCount(
    _env: JNIEnv, _class: JClass, weft_handle: jlong,
) -> jlong {
    shield(0, || {
        wefts().with(weft_handle as u64, |h| {
            stewards().with(h.steward_id, |s| {
                s.borrow(h.weft_id).map(|g| g.stats().publish_count as jlong)
            }).flatten()
        }).flatten().unwrap_or(0)
    })
}

/// Telemetry: returns the read count.
#[no_mangle]
pub extern "system" fn Java_dev_weft_TriadNative_weftReadCount(
    _env: JNIEnv, _class: JClass, weft_handle: jlong,
) -> jlong {
    shield(0, || {
        wefts().with(weft_handle as u64, |h| {
            stewards().with(h.steward_id, |s| {
                s.borrow(h.weft_id).map(|g| g.stats().read_count as jlong)
            }).flatten()
        }).flatten().unwrap_or(0)
    })
}

// --- Panic shield ---

/// Wrap a closure in `catch_unwind`. On panic, log the error and return the
/// default value. This is the L4 invariant: no native panic can crash the JVM.
fn shield<R, F: FnOnce() -> R>(default: R, f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(r) => r,
        Err(_) => {
            log::error!("weft-core: panic in JNI entry point");
            default
        }
    }
}
