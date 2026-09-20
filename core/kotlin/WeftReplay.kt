// WeftReplay.kt — RFC 0019: deterministic time-travel replay fold, Kotlin
// driver layer.
//
// WHY EXISTS: RFC-0019's fold (a pure state machine over a .weftrec v4
// event stream that reconstructs the full shadow kernel state per event and
// reduces it to a 64-bit FNV-1a hash) shipped as the C reference
// (core/c/weft_replay.{h,c}) with a TS twin (packages/core/src/replay.ts) —
// but the VM ports had no equivalent. This file is the Kotlin half,
// operation-identical to the C: the transition table and the 111-byte
// canonical serialization are NORMATIVE (RFC-0019) — cross-runtime hash
// logs are byte-compared by fixtures/xlang-replay/run.sh. Read the RFC's
// "modeling contract" before changing ANY line here: the three declared
// abstractions (null-frame len 0, PUBLISH-implies-unrevoked, claim
// validation) are what make the fold deterministic without payload bytes.
//
// ARITHMETIC PARITY: the shadow counters fit Long; the u32 state fields
// (latest/epoch/wWork/rWork, buf slots) live in Int BIT PATTERNS — every
// C unsigned op is a Kotlin Int op on the same bits (assignment, ==, ++
// and the i32 wrap of Int arithmetic), so no masks are needed beyond the
// u16 zero-extension on DROP. The u64 hash is a Long bit pattern: JVM Long
// multiply/xor ARE the mod-2^64 ops FNV-1a needs. Render it UNSIGNED with
// java.lang.Long.toHexString (16 hex digits) — never Long.toString.
//
// PINNED PARITY VECTORS (from the C reference — do not "fix" them):
//   init hash  = 0x8a769a0111cf3af3
//   100k soak  = 0x26beb484733ecde0  (the RFC-0019 fixture grammar,
//                                     seed 0x00C0FFEE)
//
// The speculative copy is REAL (the C's `weft_replay_state_t next = *s`):
// a step works on a fresh copy and assigns back ONLY on success, so a
// DISAGREE leaves the fold at the last good step — the debugger can
// inspect it (rule 3: never silent).
//
// Zero allocation in the fold path is NOT claimed here (the copy allocates;
// the fold is a debugger, not a hot path) — declared honestly, per the
// repo's per-port honesty culture. The C/TS legs make the same trade.
//
// STATUS: SOURCE-ONLY, PENDING TOOLCHAIN VERIFICATION (not compilable in
// the x86_64 Linux sandbox — no kotlinc; the arithmetic was validated
// op-for-op against the C reference before translation, and the fixture
// gate runs when android-packages CI brings the toolchain).

package dev.weft

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

/** Fold verdicts — PROTOCOL (do not renumber). */
object ReplayResult {
    const val OK: Int = 0        // step applied, hash updated
    const val DISAGREE: Int = -1 // trace/model disagreement (claim seq mismatch)
    const val BAD_KIND: Int = -2 // unknown event kind in the stream
}

/** RFC-0014 event kinds (u16) — PROTOCOL (the fold consumes them; do not renumber). */
object WeftTraceKind {
    const val PUBLISH: Int = 1     // aux = payload_len, data = seq
    const val CLAIM: Int = 2       // data = seq claimed
    const val DROP: Int = 3        // aux = epoch at ACK, data = seq refused
    const val REVOKE: Int = 4      // data = pre-revoke epoch
    const val ACK: Int = 5         // data = epoch after ACK
    const val STALL: Int = 6       // data = attempts (consumer skip)
    const val TEAR: Int = 7        // data = seq of torn frame
    const val CANARY_FAIL: Int = 8 // data = seq of bad frame
}

/** Checkpoint interval (jump cost bound) — RFC-0019. */
const val WEFT_REPLAY_CHECKPOINT: Int = 64

// ---------------------------------------------------------------------------
// Shadow kernel state (RFC-0019 fold state)
// ---------------------------------------------------------------------------

/// One ring slot of the shadow state (seq/len/ver — the fold carries no
/// payload bytes, only the slot bookkeeping).
class ReplayBuf(
    var seq: Int, // u32 bit pattern
    var len: Int, // u32 bit pattern
    var ver: Int  // u16 bit pattern
)

/**
 * The RFC-0019 fold state. Call [weftReplayInit] before the first step —
 * the constructor leaves the raw zeroed fields (the C is a POD; init is
 * the contract, including the pinned step-0 hash).
 */
class WeftReplayState {
    var latest: Int = 0   // u32 bit pattern
    var epoch: Int = 0    // u32 bit pattern
    var wWork: Int = 0    // u32 bit pattern
    var rWork: Int = 0    // u32 bit pattern
    var revoked: Int = 0  // 0/1 (u8 on the wire)

    val buf: Array<ReplayBuf> = arrayOf(
        ReplayBuf(0, 0, 1), ReplayBuf(0, 0, 1), ReplayBuf(0, 0, 1)
    )

    var tPublish: Long = 0 // u64
    var tClaim: Long = 0   // u64
    var tDrop: Long = 0    // u64
    var tInvalid: Long = 0 // u64
    var tWsteps: Long = 0  // u64
    var tRsteps: Long = 0  // u64
    var tStall: Int = 0    // u32 bit pattern
    var tTear: Int = 0     // u32 bit pattern
    var tCanary: Int = 0   // u32 bit pattern
    var step: Int = 0      // events consumed (u32 bit pattern)

    /// FNV-1a-64 over the canonical serialization — u64 bit pattern in a
    /// Long. Render with java.lang.Long.toHexString (UNSIGNED), never
    /// Long.toString (signed).
    var hash: Long = 0
}

// ---------------------------------------------------------------------------
// Serialization (canonical LE byte layout — RFC-0019, normative)
// ---------------------------------------------------------------------------

/// Canonical little-endian serialization (RFC-0019): 111 bytes —
/// u32 latest@0, u32 epoch@4, u32 w_work@8, u32 r_work@12, u8 revoked@16,
/// 3x(u32 seq, u32 len, u16 ver)@17, u64 t_publish/t_claim/t_drop/t_invalid/
/// t_wsteps/t_rsteps@47..94, u32 t_stall@95, t_tear@99, t_canary@103,
/// u32 step@107. Exposed for cross-port byte tests.
fun weftReplaySerialize(s: WeftReplayState): ByteArray {
    val out = ByteArray(111)
    var p = 0
    fun putU32(v: Int) {
        out[p] = v.toByte()
        out[p + 1] = (v shr 8).toByte()
        out[p + 2] = (v shr 16).toByte()
        out[p + 3] = (v shr 24).toByte()
        p += 4
    }
    fun putU16(v: Int) {
        out[p] = v.toByte()
        out[p + 1] = (v shr 8).toByte()
        p += 2
    }
    fun putU64(v: Long) {
        for (i in 0..7) out[p + i] = (v shr (8 * i)).toByte()
        p += 8
    }
    putU32(s.latest)
    putU32(s.epoch)
    putU32(s.wWork)
    putU32(s.rWork)
    out[p] = s.revoked.toByte()
    p += 1
    for (i in 0..2) {
        putU32(s.buf[i].seq)
        putU32(s.buf[i].len)
        putU16(s.buf[i].ver)
    }
    putU64(s.tPublish)
    putU64(s.tClaim)
    putU64(s.tDrop)
    putU64(s.tInvalid)
    putU64(s.tWsteps)
    putU64(s.tRsteps)
    putU32(s.tStall)
    putU32(s.tTear)
    putU32(s.tCanary)
    putU32(s.step)
    return out
}

// ---------------------------------------------------------------------------
// FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3)
// ---------------------------------------------------------------------------

/// FNV-1a 64 over a byte range — one definition shared by every fold path.
/// JVM Long arithmetic wraps mod 2^64: exactly the u64 ops FNV-1a needs.
/// The 0x literal is in the unsigned range — Kotlin Long hex literals
/// accept it and carry the two's-complement bit pattern.
fun weftReplayFnv1a(data: ByteArray): Long {
    var h = 0xcbf29ce484222325L
    for (b in data) {
        h = h xor (b.toLong() and 0xffL)
        h *= 0x100000001b3L
    }
    return h
}

// ---------------------------------------------------------------------------
// Fold
// ---------------------------------------------------------------------------

/// Reset to the RFC-0019 initial state (including the initial hash — the
/// hash of step 0 is a pinned parity vector across all ports).
fun weftReplayInit(s: WeftReplayState) {
    s.latest = 0
    s.epoch = 0
    s.wWork = 1
    s.rWork = 2
    s.revoked = 0
    for (i in 0..2) {
        s.buf[i].seq = 0
        s.buf[i].len = 0
        s.buf[i].ver = 1
    }
    s.tPublish = 0
    s.tClaim = 0
    s.tDrop = 0
    s.tInvalid = 0
    s.tWsteps = 0
    s.tRsteps = 0
    s.tStall = 0
    s.tTear = 0
    s.tCanary = 0
    s.step = 0
    s.hash = weftReplayFnv1a(weftReplaySerialize(s))
}

/// The C's `weft_replay_state_t next = *s` — a real deep copy (buf slots
/// included); the fold works on this and assigns back ONLY on success.
private fun copyOf(s: WeftReplayState): WeftReplayState {
    val c = WeftReplayState()
    c.latest = s.latest
    c.epoch = s.epoch
    c.wWork = s.wWork
    c.rWork = s.rWork
    c.revoked = s.revoked
    for (i in 0..2) {
        c.buf[i].seq = s.buf[i].seq
        c.buf[i].len = s.buf[i].len
        c.buf[i].ver = s.buf[i].ver
    }
    c.tPublish = s.tPublish
    c.tClaim = s.tClaim
    c.tDrop = s.tDrop
    c.tInvalid = s.tInvalid
    c.tWsteps = s.tWsteps
    c.tRsteps = s.tRsteps
    c.tStall = s.tStall
    c.tTear = s.tTear
    c.tCanary = s.tCanary
    c.step = s.step
    c.hash = s.hash
    return c
}

private fun assignBack(dst: WeftReplayState, src: WeftReplayState) {
    dst.latest = src.latest
    dst.epoch = src.epoch
    dst.wWork = src.wWork
    dst.rWork = src.rWork
    dst.revoked = src.revoked
    for (i in 0..2) {
        dst.buf[i].seq = src.buf[i].seq
        dst.buf[i].len = src.buf[i].len
        dst.buf[i].ver = src.buf[i].ver
    }
    dst.tPublish = src.tPublish
    dst.tClaim = src.tClaim
    dst.tDrop = src.tDrop
    dst.tInvalid = src.tInvalid
    dst.tWsteps = src.tWsteps
    dst.tRsteps = src.tRsteps
    dst.tStall = src.tStall
    dst.tTear = src.tTear
    dst.tCanary = src.tCanary
    dst.step = src.step
    dst.hash = src.hash
}

/// Apply ONE event. Returns [ReplayResult.OK] and updates s.hash, or a
/// verdict WITHOUT mutating state (a disagreement leaves the fold at the
/// last good step — the debugger can inspect it).
///
/// `kind` is the RFC-0014 u16 kind; `aux`/`data` are the u32 payload bit
/// patterns. The transition table is NORMATIVE (weft_replay.c) — mirror,
/// never redesign.
fun weftReplayStep(s: WeftReplayState, kind: Int, aux: Int, data: Int): Int {
    // speculative copy: disagreement leaves the caller's state untouched
    val next = copyOf(s)
    when (kind) {
        WeftTraceKind.PUBLISH -> {
            // buf[w_work] = {seq, aux, v1}; old = latest; latest = w_work;
            // w_work = old. THE exchange, exactly as weft.c's step 4-5.
            next.buf[next.wWork].seq = data
            next.buf[next.wWork].len = aux
            next.buf[next.wWork].ver = 1
            val old = next.latest
            next.latest = next.wWork
            next.wWork = old
            next.revoked = 0 // modeling rule 2: publish implies rebind
            next.tPublish++
            next.tWsteps++
        }
        WeftTraceKind.CLAIM -> {
            // mine = latest; latest = r_work; r_work = mine.
            val mine = next.latest
            next.latest = next.rWork
            next.rWork = mine
            if (next.buf[mine].seq != data) {
                return ReplayResult.DISAGREE // rule 3: never silent
            }
            next.tClaim++
            next.tRsteps++
        }
        WeftTraceKind.DROP -> {
            // aux is the u16 epoch at ACK — zero-extend (the C's uint16_t
            // aux field widens implicitly; the mask makes it explicit).
            next.epoch = aux and 0xFFFF
            next.tDrop++
        }
        WeftTraceKind.REVOKE -> next.revoked = 1
        WeftTraceKind.ACK -> next.epoch = data // epoch after ACK
        WeftTraceKind.STALL -> next.tStall++
        WeftTraceKind.TEAR -> next.tTear++
        WeftTraceKind.CANARY_FAIL -> next.tCanary++
        else -> return ReplayResult.BAD_KIND
    }
    next.step++
    next.hash = weftReplayFnv1a(weftReplaySerialize(next))
    assignBack(s, next)
    return ReplayResult.OK
}

/// Convenience: fold `evs` (kind/aux/data triples), appending per-step
/// hashes to `hashesOut` when non-null. Returns the last verdict.
fun weftReplayFold(
    s: WeftReplayState,
    evs: List<Triple<Int, Int, Int>>,
    hashesOut: MutableList<Long>? = null
): Int {
    var rc = ReplayResult.OK
    for (e in evs) {
        rc = weftReplayStep(s, e.first, e.second, e.third)
        if (rc != ReplayResult.OK) return rc
        hashesOut?.add(s.hash)
    }
    return rc
}
