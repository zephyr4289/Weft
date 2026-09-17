//! shm.rs — inter-process shared-memory rings (RFC-0004 layout over POSIX
//! shm / anonymous mmap), Rust driver layer.
//!
//! WHY EXISTS: the C driver layer grew `shm_ring.{h,c}` (Series 7) with a
//! 64-byte session header protocol ("WFSH") so one shm object can host N
//! processes on one fan-out ring with zero kernel transitions in the data
//! path. This module is the Rust twin: same byte layout, same attach
//! validation, same lifetime contract — so a Rust producer, a C producer,
//! and (via the TS fixture) a Node consumer all speak ONE session format.
//!
//! ZERO-CRATE DISCIPLINE (06-PITFALLS §3): no `[dependencies]` are added.
//! The POSIX entry points (shm_open, mmap, ftruncate, fstat, fork, waitpid)
//! are declared `extern "C"` against the system libc that std itself
//! already links — the same mechanism std uses internally, not a crate.
//! Windows is compile-gated exactly like the C side (named file mappings)
//! and is compile-verified by the windows CI leg only — declared.
//!
//! Session header (identical to core/c/shm_ring.h — the cross-language
//! contract, little-endian):
//!   [0..4)   magic "WFSH"      [4..6)   version 1
//!   [6..8)   header_size 64    [8..12)  flags 0 (unknown bits reject)
//!   [12..16) payload_bytes     [16..20) slot_count
//!   [20..28) ring_bytes        [28..32) creator_pid (advisory)
//!   [32..40) created_unix_ns (advisory)   [40..64) reserved zero
//!   [64..)   the RFC-0004 ring (fanout.rs layout)
//!
//! Layer discipline: driver layer; lib.rs gains one `pub mod` line; kernel
//! untouched. Law 2: create/attach may allocate; publish/claim allocate
//! nothing and make no syscalls (see the C strace evidence — the Rust data
//! path executes the identical instruction sequence over the same pages).

use crate::fanout::{WeftFanout, WeftFanoutReader};
use std::fs::File;
use std::io;
use std::os::unix::io::{AsRawFd, FromRawFd};

/// Session header size in bytes (the ring starts at this offset).
pub const HEADER_BYTES: usize = 64;
/// "WFSH" little-endian.
pub const MAGIC: u32 = 0x4853_4657;
/// Session protocol version (attachers reject mismatches).
pub const VERSION: u16 = 1;

// ---------------------------------------------------------------------------
// libc FFI (zero crates — std already links libc)
// ---------------------------------------------------------------------------

#[cfg(unix)]
mod sys {
    use std::os::raw::{c_char, c_int, c_long, c_void};

    /// open(2) O_CREAT.
    pub const O_CREAT: c_int = 0o100;
    /// open(2) O_EXCL.
    pub const O_EXCL: c_int = 0o200;
    /// open(2) O_RDWR.
    pub const O_RDWR: c_int = 0o2;
    /// open(2) O_RDONLY.
    pub const O_RDONLY: c_int = 0o0;
    /// mmap(2) PROT_READ.
    pub const PROT_READ: c_int = 0x1;
    /// mmap(2) PROT_WRITE.
    pub const PROT_WRITE: c_int = 0x2;
    /// mmap(2) MAP_SHARED.
    pub const MAP_SHARED: c_int = 0x01;
    #[cfg(target_os = "linux")]
    /// mmap(2) MAP_ANONYMOUS (Linux).
    pub const MAP_ANONYMOUS: c_int = 0x20;
    #[cfg(not(target_os = "linux"))]
    /// mmap(2) MAP_ANON (BSD/macOS).
    pub const MAP_ANONYMOUS: c_int = 0x1000;
    /// clock_gettime(2) CLOCK_REALTIME.
    pub const CLOCK_REALTIME: c_int = 0;

    #[repr(C)]
    pub struct Timespec {
        pub tv_sec: c_long,
        pub tv_nsec: c_long,
    }

    extern "C" {
        pub fn shm_open(name: *const c_char, oflag: c_int, mode: u32) -> c_int;
        pub fn shm_unlink(name: *const c_char) -> c_int;
        pub fn mmap(addr: *mut c_void, len: usize, prot: c_int, flags: c_int,
                    fd: c_int, offset: i64) -> *mut c_void;
        pub fn munmap(addr: *mut c_void, len: usize) -> c_int;
        pub fn ftruncate(fd: c_int, len: i64) -> c_int;
        pub fn getpid() -> c_int;
        pub fn clock_gettime(clk_id: c_int, tp: *mut Timespec) -> c_int;
        pub fn fork() -> i32;
        pub fn waitpid(pid: i32, status: *mut c_int, options: c_int) -> i32;
    }
}

/// A mapped shared-memory session (header + RFC-0004 ring). `Drop` unmaps;
/// a CREATOR's drop also unlinks the name (a crash leaks the object on
/// purpose — the crash-tolerant posture; see the C header's lifetime note).
#[derive(Debug)]
#[allow(missing_docs)]  // field docs live in the C twin; layout is the contract
pub struct ShmMap {
    base: *mut u8,
    ring: *mut u8,
    mapping_bytes: usize,
    file: Option<File>,  // None for anonymous mappings
    creator: bool,
    name: String,        // "" = anonymous
}

// The mapping is shared between processes/threads by design; the Rust
// wrapper is move-only, and the raw pointers are only dereferenced through
// the fanout attach APIs (which impose their own aliasing discipline).
unsafe impl Send for ShmMap {}
unsafe impl Sync for ShmMap {}

fn name_valid(name: &str) -> bool {
    if name.is_empty() || name.len() > 80 {
        return false;
    }
    if name.starts_with('-') {
        return false;
    }
    name.bytes().all(|b| {
        b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-'
    })
}

fn header_write(base: *mut u8, payload_bytes: usize, slot_count: usize) {
    unsafe {
        std::ptr::write_bytes(base, 0, HEADER_BYTES);
        let rb = crate::fanout::ring_bytes(payload_bytes, slot_count)
            .expect("validated geometry");
        let p = base;
        std::ptr::write_unaligned(p as *mut u32, MAGIC);
        std::ptr::write_unaligned(p.add(4) as *mut u16, VERSION);
        std::ptr::write_unaligned(p.add(6) as *mut u16, HEADER_BYTES as u16);
        std::ptr::write_unaligned(p.add(8) as *mut u32, 0);
        std::ptr::write_unaligned(p.add(12) as *mut u32, payload_bytes as u32);
        std::ptr::write_unaligned(p.add(16) as *mut u32, slot_count as u32);
        std::ptr::write_unaligned(p.add(20) as *mut u64, rb as u64);
        std::ptr::write_unaligned(p.add(28) as *mut u32, sys::getpid() as u32);
        let mut ts = sys::Timespec { tv_sec: 0, tv_nsec: 0 };
        sys::clock_gettime(sys::CLOCK_REALTIME, &mut ts);
        let ns = ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64;
        std::ptr::write_unaligned(p.add(32) as *mut u64, ns);
    }
}

/// Validate a mapped header against the real mapping size.
/// Returns (payload_bytes, slot_count) or an error — never guesses.
fn header_validate(base: *const u8, mapping_bytes: usize) -> io::Result<(usize, usize)> {
    unsafe {
        let p = base;
        if mapping_bytes < HEADER_BYTES {
            return Err(bad("mapping smaller than the session header"));
        }
        if std::ptr::read_unaligned(p as *const u32) != MAGIC {
            return Err(bad("bad magic (not a Weft session)"));
        }
        if std::ptr::read_unaligned(p.add(4) as *const u16) != VERSION {
            return Err(bad("session version mismatch"));
        }
        if std::ptr::read_unaligned(p.add(6) as *const u16) as usize != HEADER_BYTES {
            return Err(bad("header size mismatch"));
        }
        if std::ptr::read_unaligned(p.add(8) as *const u32) != 0 {
            return Err(bad("unknown flag bits (version violation)"));
        }
        let pb = std::ptr::read_unaligned(p.add(12) as *const u32) as usize;
        let slots = std::ptr::read_unaligned(p.add(16) as *const u32) as usize;
        let rb = std::ptr::read_unaligned(p.add(20) as *const u64) as usize;
        match crate::fanout::ring_bytes(pb, slots) {
            Some(want) if want == rb => {}
            _ => return Err(bad("header geometry does not reconcile")),
        }
        for i in 40..HEADER_BYTES {
            if *p.add(i) != 0 {
                return Err(bad("reserved bytes not zero (version violation)"));
            }
        }
        if mapping_bytes != HEADER_BYTES + rb {
            return Err(bad("mapping size != header + ring_bytes"));
        }
        Ok((pb, slots))
    }
}

fn bad(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}

impl ShmMap {
    /// Ring base pointer (the RFC-0004 layout begins here).
    pub fn ring_ptr(&self) -> *mut u8 {
        self.ring
    }
    /// Ring size in bytes (the RFC-0004 identity 16 + 8M + M*payload_bytes).
    pub fn ring_bytes(&self) -> usize {
        self.mapping_bytes - HEADER_BYTES
    }
    /// Per-slot payload capacity, read from the session header.
    pub fn payload_bytes(&self) -> usize {
        unsafe { std::ptr::read_unaligned(self.base.add(12) as *const u32) as usize }
    }
    /// Ring depth M, read from the session header.
    pub fn slot_count(&self) -> usize {
        unsafe { std::ptr::read_unaligned(self.base.add(16) as *const u32) as usize }
    }
    /// The session name ("" for anonymous sessions).
    pub fn name(&self) -> &str {
        &self.name
    }
}

impl Drop for ShmMap {
    fn drop(&mut self) {
        unsafe {
            sys::munmap(self.base as *mut _, self.mapping_bytes);
        }
        // File::drop closes the fd (when there is one).
        self.file = None;
        if self.creator && !self.name.is_empty() {
            let _ = unlink(&self.name);
        }
    }
}

/// Remove a stale named session (the explicit replace-stale road).
pub fn unlink(name: &str) -> io::Result<()> {
    if !name_valid(name) {
        return Err(bad("invalid session name"));
    }
    unsafe {
        let path = format!("/{}", name);
        let c = std::ffi::CString::new(path).unwrap();
        let rc = sys::shm_unlink(c.as_ptr());
        if rc == 0 || io::Error::last_os_error().raw_os_error() == Some(2) {
            Ok(())
        } else {
            Err(io::Error::last_os_error())
        }
    }
}

/// Create a NAMED session (O_EXCL — re-creating an existing name fails).
pub fn create_named(name: &str, payload_bytes: usize, slot_count: usize)
                    -> io::Result<ShmMap> {
    if !name_valid(name) {
        return Err(bad("invalid session name"));
    }
    let rb = match crate::fanout::ring_bytes(payload_bytes, slot_count) {
        Some(rb) if rb > 0 => rb,
        _ => return Err(bad("invalid ring geometry")),
    };
    let mapping_bytes = HEADER_BYTES + rb;
    unsafe {
        let path = format!("/{}", name);
        let c = std::ffi::CString::new(path).unwrap();
        let fd = sys::shm_open(c.as_ptr(), sys::O_CREAT | sys::O_EXCL | sys::O_RDWR, 0o600);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // Own the fd through File: metadata() gives the size via std's
        // correctly-laid-out stat — no hand-rolled struct stat (the layout
        // differs by platform and a wrong offset reads garbage st_size).
        let file = unsafe { File::from_raw_fd(fd) };
        if sys::ftruncate(fd, mapping_bytes as i64) != 0 {
            return Err(io::Error::last_os_error());
        }
        let p = sys::mmap(std::ptr::null_mut(), mapping_bytes,
                          sys::PROT_READ | sys::PROT_WRITE, sys::MAP_SHARED, fd, 0);
        if p as isize == -1 {
            return Err(io::Error::last_os_error());
        }
        let base = p as *mut u8;
        header_write(base, payload_bytes, slot_count);
        let ring = base.add(HEADER_BYTES);
        Ok(ShmMap { base, ring, mapping_bytes,
                    file: Some(file), creator: true, name: name.to_string() })
    }
}

/// Attach an existing NAMED session (validates the full header contract).
pub fn attach_named(name: &str, read_only: bool) -> io::Result<ShmMap> {
    if !name_valid(name) {
        return Err(bad("invalid session name"));
    }
    unsafe {
        let path = format!("/{}", name);
        let c = std::ffi::CString::new(path).unwrap();
        let fd = sys::shm_open(c.as_ptr(), if read_only { sys::O_RDONLY } else { sys::O_RDWR }, 0);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let file = unsafe { File::from_raw_fd(fd) };
        // Object size via std metadata (correct stat layout on every platform).
        let mapping_bytes = file.metadata()?.len() as usize;
        if mapping_bytes < HEADER_BYTES {
            return Err(bad("object smaller than the session header"));
        }
        let prot = if read_only { sys::PROT_READ } else { sys::PROT_READ | sys::PROT_WRITE };
        let p = sys::mmap(std::ptr::null_mut(), mapping_bytes, prot, sys::MAP_SHARED, fd, 0);
        if p as isize == -1 {
            return Err(io::Error::last_os_error());
        }
        if let Err(e) = header_validate(p as *const u8, mapping_bytes) {
            unsafe { sys::munmap(p, mapping_bytes) };
            return Err(e);
        }
        Ok(ShmMap { base: p as *mut u8, ring: (p as *mut u8).add(HEADER_BYTES),
                    mapping_bytes, file: Some(file), creator: false, name: name.to_string() })
    }
}

/// Create an ANONYMOUS session (fork-inherited; no name, no fd).
pub fn create_anon(payload_bytes: usize, slot_count: usize) -> io::Result<ShmMap> {
    let rb = match crate::fanout::ring_bytes(payload_bytes, slot_count) {
        Some(rb) if rb > 0 => rb,
        _ => return Err(bad("invalid ring geometry")),
    };
    let mapping_bytes = HEADER_BYTES + rb;
    unsafe {
        let p = sys::mmap(std::ptr::null_mut(), mapping_bytes,
                          sys::PROT_READ | sys::PROT_WRITE,
                          sys::MAP_SHARED | sys::MAP_ANONYMOUS, -1, 0);
        if p as isize == -1 {
            return Err(io::Error::last_os_error());
        }
        let base = p as *mut u8;
        header_write(base, payload_bytes, slot_count);
        Ok(ShmMap { base, ring: base.add(HEADER_BYTES), mapping_bytes,
                    file: None, creator: true, name: String::new() })
    }
}

// ---------------------------------------------------------------------------
// Fan-out bindings (the conveniences the C side exports)
// ---------------------------------------------------------------------------

impl ShmMap {
    /// Bind a broadcaster to this session's ring (writer side).
    pub fn attach_writer(&self) -> Option<WeftFanout> {
        unsafe { WeftFanout::attach_raw(self.ring, self.ring_bytes(),
                                        self.payload_bytes(), self.slot_count()) }
    }
    /// Bind a reader to this session's ring.
    pub fn attach_reader(&self) -> Option<WeftFanoutReader> {
        unsafe { WeftFanoutReader::attach_raw(self.ring, self.ring_bytes(),
                                              self.payload_bytes(), self.slot_count()) }
    }
}

/// Create a named session AND bind its broadcaster (creator road).
pub fn fanout_create_named(name: &str, payload_bytes: usize, slot_count: usize)
                           -> io::Result<(WeftFanout, ShmMap)> {
    let m = create_named(name, payload_bytes, slot_count)?;
    match m.attach_writer() {
        Some(f) => Ok((f, m)),
        None => Err(bad("writer attach rejected the session geometry")),
    }
}

/// Attach a broadcaster to an existing named session (handoff road).
pub fn fanout_attach_writer_named(name: &str) -> io::Result<(WeftFanout, ShmMap)> {
    let m = attach_named(name, false)?;
    match m.attach_writer() {
        Some(f) => Ok((f, m)),
        None => Err(bad("writer attach rejected the session geometry")),
    }
}

/// Attach a reader to an existing named session (read_only=1 is the
/// flight-recorder posture).
pub fn fanout_attach_reader_named(name: &str, read_only: bool)
                                  -> io::Result<(WeftFanoutReader, ShmMap)> {
    let m = attach_named(name, read_only)?;
    match m.attach_reader() {
        Some(r) => Ok((r, m)),
        None => Err(bad("reader attach rejected the session geometry")),
    }
}

// ---------------------------------------------------------------------------
// fork torture support (POSIX; used by tests/shm_test.rs)
// ---------------------------------------------------------------------------

/// Raw fork for the torture harness (child returns 0). Zero crates: the
/// extern "C" declaration above. Not part of the public API surface.
#[doc(hidden)]
pub unsafe fn fork_raw() -> i32 {
    unsafe { sys::fork() }
}

/// Raw waitpid for the torture harness.
#[doc(hidden)]
pub unsafe fn waitpid_raw(pid: i32) -> i32 {
    let mut status: i32 = 0;
    unsafe { sys::waitpid(pid, &mut status, 0) };
    status
}
