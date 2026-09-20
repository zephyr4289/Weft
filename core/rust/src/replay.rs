//! # RFC 0019 deterministic time-travel replay fold — Rust port.
//!
//! Mirror of `core/c/weft_replay.{h,c}` (NORMATIVE): a pure state machine
//! ("fold") over a .weftrec v4 event stream — after every event it
//! reconstructs the full shadow kernel state and reduces it to a 64-bit
//! FNV-1a hash. Every port implements EXACTLY the transition table and the
//! canonical byte serialization in RFC-0019 — cross-runtime hash logs are
//! byte-compared (fixtures/xlang-replay/, driven by the `replay_xlang`
//! bin). Read RFC-0019's "modeling contract" before changing ANY line
//! here: the three declared abstractions (null-frame len 0,
//! PUBLISH-implies-unrevoked, claim validation) are what make the fold
//! deterministic without payload bytes.
//!
//! The kernel (`crate::Weft`) is untouched — Law 3. Zero allocation in the
//! step path: the speculative copy is a `clone()` of a plain struct, which
//! is exactly the C's checkpoint struct copy — Law 2. No loops beyond the
//! event stream and the 111-byte serialization — Law 1.
//!
//! Pinned parity vectors (from the C reference — do not "fix" them):
//!   init hash  = 0x8a769a0111cf3af3
//!   100k soak  = 0x26beb484733ecde0  (the RFC-0019 fixture grammar)

/// Checkpoint interval — the jump-cost bound (R7's checkpoint/jump
/// equivalence folds in blocks of this many events; mirrors
/// `WEFT_REPLAY_CHECKPOINT` in core/c/weft_replay.h).
pub const WEFT_REPLAY_CHECKPOINT: u32 = 64;

/// Shadow per-slot frame header (RFC-0019 modeling state). The null frame
/// is `{seq: 0, len: 0, ver: 1}` — modeling rule 1.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ReplayBuf {
    /// Published sequence number (0 = the null frame).
    pub seq: u32,
    /// Payload length at publish time (0 for the null frame).
    pub len: u32,
    /// Envelope version (always 1 — rule 1).
    pub ver: u16,
}

/// Shadow kernel state (the RFC-0019 fold state). A plain value —
/// checkpointing is a `clone()`, no allocator involved (Law 2).
#[derive(Debug, Clone)]
pub struct WeftReplayState {
    /// Most recently published buffer index (0..3).
    pub latest: u32,
    /// Writer epoch (ACK-synced; DROP carries the epoch at ACK).
    pub epoch: u32,
    /// Writer's scratch slot (the exchange partner of `latest`).
    pub w_work: u32,
    /// Reader's scratch slot (the exchange partner of `latest` on claim).
    pub r_work: u32,
    /// 1 while the writer is revoked (publish implies rebind — rule 2).
    pub revoked: u8,
    /// Shadow per-slot frame headers (null frame = {0, 0, v1} — rule 1).
    pub buf: [ReplayBuf; 3],
    /// Publish events folded.
    pub t_publish: u64,
    /// Claim events folded.
    pub t_claim: u64,
    /// Drop events folded.
    pub t_drop: u64,
    /// Reserved (rejected-input) counter — carried by the serialization,
    /// never incremented by the fold (no v4 event kind maps to it).
    pub t_invalid: u64,
    /// Writer steps folded (publishes + drops).
    pub t_wsteps: u64,
    /// Reader steps folded (claims).
    pub t_rsteps: u64,
    /// Stall events folded.
    pub t_stall: u32,
    /// Tear events folded.
    pub t_tear: u32,
    /// Canary-fault events folded.
    pub t_canary: u32,
    /// Events consumed.
    pub step: u32,
    /// FNV-1a over the canonical serialization (the hash of step 0 is a
    /// pinned parity vector across all ports).
    pub hash: u64,
}

impl Default for WeftReplayState {
    fn default() -> Self {
        let mut s = Self {
            latest: 0,
            epoch: 0,
            w_work: 1,
            r_work: 2,
            revoked: 0,
            buf: [ReplayBuf { seq: 0, len: 0, ver: 1 }; 3],
            t_publish: 0,
            t_claim: 0,
            t_drop: 0,
            t_invalid: 0,
            t_wsteps: 0,
            t_rsteps: 0,
            t_stall: 0,
            t_tear: 0,
            t_canary: 0,
            step: 0,
            hash: 0,
        };
        replay_init(&mut s);
        s
    }
}

/// Fold verdicts — mirror of `weft_replay_result_t` (C numeric values
/// 0 / -1 / -2; the TS enum pins those numbers).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReplayResult {
    /// Step applied, hash updated.
    Ok,
    /// Trace/model disagreement (claim seq mismatch — the class of bug
    /// this debugger exists to surface; NEVER silent).
    Disagree,
    /// Unknown event kind in the stream.
    BadKind,
}

/// FNV-1a 64 over a byte range — offset `0xcbf29ce484222325`, prime
/// `0x100000001b3`. Exported so ports and tests share one definition
/// (exactly as the C exports `weft_replay_fnv1a`).
pub fn replay_fnv1a(data: &[u8]) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}

fn put_u16(out: &mut [u8], off: usize, v: u16) {
    out[off] = v as u8;
    out[off + 1] = (v >> 8) as u8;
}

fn put_u32(out: &mut [u8], off: usize, v: u32) {
    out[off] = v as u8;
    out[off + 1] = (v >> 8) as u8;
    out[off + 2] = (v >> 16) as u8;
    out[off + 3] = (v >> 24) as u8;
}

fn put_u64(out: &mut [u8], off: usize, v: u64) {
    for i in 0..8 {
        out[off + i] = (v >> (8 * i)) as u8;
    }
}

/// Canonical little-endian serialization (RFC-0019 — NORMATIVE byte
/// layout, 111 bytes): u32 latest/epoch/w_work/r_work @0..16, u8 revoked
/// @16, 3x (u32 seq, u32 len, u16 ver) @17..47, u64 t_publish / t_claim /
/// t_drop / t_invalid / t_wsteps / t_rsteps @47..95, u32 t_stall /
/// t_tear / t_canary / step @95..111. No padding, no float — the layout
/// IS the cross-port contract.
pub fn replay_serialize(s: &WeftReplayState, out: &mut [u8; 111]) {
    put_u32(out, 0, s.latest);
    put_u32(out, 4, s.epoch);
    put_u32(out, 8, s.w_work);
    put_u32(out, 12, s.r_work);
    out[16] = s.revoked;
    for i in 0..3 {
        let off = 17 + 10 * i;
        put_u32(out, off, s.buf[i].seq);
        put_u32(out, off + 4, s.buf[i].len);
        put_u16(out, off + 8, s.buf[i].ver);
    }
    put_u64(out, 47, s.t_publish);
    put_u64(out, 55, s.t_claim);
    put_u64(out, 63, s.t_drop);
    put_u64(out, 71, s.t_invalid);
    put_u64(out, 79, s.t_wsteps);
    put_u64(out, 87, s.t_rsteps);
    put_u32(out, 95, s.t_stall);
    put_u32(out, 99, s.t_tear);
    put_u32(out, 103, s.t_canary);
    put_u32(out, 107, s.step);
}

/// Reset to the RFC-0019 initial state — including the initial hash (the
/// hash of step 0 is a pinned parity vector: 0x8a769a0111cf3af3).
pub fn replay_init(s: &mut WeftReplayState) {
    s.latest = 0;
    s.epoch = 0;
    s.w_work = 1;
    s.r_work = 2;
    s.revoked = 0;
    s.buf = [ReplayBuf { seq: 0, len: 0, ver: 1 }; 3];
    s.t_publish = 0;
    s.t_claim = 0;
    s.t_drop = 0;
    s.t_invalid = 0;
    s.t_wsteps = 0;
    s.t_rsteps = 0;
    s.t_stall = 0;
    s.t_tear = 0;
    s.t_canary = 0;
    s.step = 0;
    let mut ser = [0u8; 111];
    replay_serialize(s, &mut ser);
    s.hash = replay_fnv1a(&ser);
}

/// Apply ONE event. The work happens on a speculative clone and is
/// committed only on success — a disagreement (or an unknown kind) leaves
/// the caller's state at the last good step so the debugger can inspect
/// it. Event kinds are the RFC-0014 v4 trace values (1=PUBLISH, 2=CLAIM,
/// 3=DROP, 4=REVOKE, 5=ACK, 6=STALL, 7=TEAR, 8=CANARY_FAIL).
pub fn replay_step(s: &mut WeftReplayState, kind: u16, aux: u16, data: u32) -> ReplayResult {
    let mut next = s.clone(); // speculative copy: disagreement leaves the
                              // caller's state untouched
    match kind {
        // PUBLISH: aux = payload_len, data = seq. THE exchange, exactly
        // as weft.c's publish (steps 4-5).
        1 => {
            next.buf[next.w_work as usize] = ReplayBuf {
                seq: data,
                len: aux as u32,
                ver: 1,
            };
            let old = next.latest;
            next.latest = next.w_work;
            next.w_work = old;
            next.revoked = 0; // modeling rule 2: publish implies rebind
            next.t_publish += 1;
            next.t_wsteps += 1;
        }
        // CLAIM: data = claimed seq. mine = latest; latest = r_work;
        // r_work = mine — then the claim is VALIDATED: a seq mismatch is
        // the trace/model disagreement (rule 3: never silent).
        2 => {
            let mine = next.latest;
            next.latest = next.r_work;
            next.r_work = mine;
            if next.buf[mine as usize].seq != data {
                return ReplayResult::Disagree;
            }
            next.t_claim += 1;
            next.t_rsteps += 1;
        }
        // DROP: aux = epoch at ACK.
        3 => {
            next.epoch = aux as u32;
            next.t_drop += 1;
        }
        // REVOKE: the writer is withdrawn until the next publish.
        4 => {
            next.revoked = 1;
        }
        // ACK: data = epoch after ACK.
        5 => {
            next.epoch = data;
        }
        6 => {
            next.t_stall += 1;
        }
        7 => {
            next.t_tear += 1;
        }
        8 => {
            next.t_canary += 1;
        }
        _ => return ReplayResult::BadKind,
    }
    next.step += 1;
    let mut ser = [0u8; 111];
    replay_serialize(&next, &mut ser);
    next.hash = replay_fnv1a(&ser);
    *s = next;
    ReplayResult::Ok
}

/// Convenience: fold `events` (each a `(kind, aux, data)` triple — the
/// RFC-0014 packed record without the CRC), writing per-step hashes to
/// `hashes` when given. Returns the first non-Ok verdict; the fold stops
/// there with the state at the last good step.
pub fn replay_fold(
    s: &mut WeftReplayState,
    events: &[(u16, u16, u32)],
    mut hashes: Option<&mut Vec<u64>>,
) -> ReplayResult {
    let mut rc = ReplayResult::Ok;
    for &(kind, aux, data) in events {
        rc = replay_step(s, kind, aux, data);
        if rc != ReplayResult::Ok {
            return rc;
        }
        if let Some(out) = hashes.as_mut() {
            out.push(s.hash);
        }
    }
    rc
}
