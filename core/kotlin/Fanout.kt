// Fanout.kt — RFC 0004: Multi-Consumer Fan-Out ring, Kotlin/JVM driver layer
//
// WHY EXISTS: RFC 0004 (Accepted as driver-layer pattern, round-6 §4). The
// Triad kernel is 1-writer/1-reader by design; applications that need a
// primary canvas, a minimap, a flight recorder, and a network visualizer on
// one stream cannot bind N readers to one Triad. The TS port ships the ring
// over a SharedArrayBuffer (core/ts/fanout.ts) and the canonical C/Rust
// kernels got byte-compatible rings in Series 4 (core/c/fanout.{h,c},
// core/rust/src/fanout.rs) — but the Kotlin/JVM port had no equivalent:
// Android apps and JVM engines could only fan out through the JNI detour to
// the C ring. This module brings the ring to Kotlin with the SAME
// BYTE-COMPATIBLE LAYOUT, so a ring produced by any port is consumable by
// any other (docs/PORTS.md §6; the interop contract proven by
// fixtures/xlang-fanout/).
//
// RING LAYOUT (byte-identical to core/ts/fanout.ts and core/c/fanout.h):
//   byte 0              latestSeq   i64  0 = no frame yet; frames from 1
//   byte 8              publishes   i64  telemetry (one add per publish)
//   byte 16 + 8k        slotSeq[k]  i64  0 = INVALIDATED (fill in progress)
//   byte 16 + 8M        payload     M slots x payload_bytes (slot k at +k*payload_bytes)
// payload_bytes MUST be a multiple of 4 (u32 word granularity).
// ring_bytes = 16 + 8M + M*payload_bytes — identical formula in all ports.
// The ring lives in one DIRECT ByteBuffer: any other port (or a JNI peer)
// can attach to the same bytes via weft_fanout_attach_writer() /
// WeftFanoutReader(buf) with zero copy — the JVM analog of posting the SAB.
//
// PROTOCOL (RFC 0004 §Reference-level specification — same as every port):
//   Writer (single, by contract — the same contract as the kernel's writer):
//     begin():   wSeq += 1; k = (wSeq-1) mod M;
//                slotSeq[k] <- 0  (invalidate BEFORE the fill — the FI1 bracket)
//                return slot cursor (a ByteBuffer slice; plain writes under
//                the bracket discipline, the TS port's stance — or use fill()
//                for the race-free opaque-word path, the C port's stance)
//     publish(): slotSeq[k] <- wSeq; latestSeq <- wSeq; publishes += 1
//   Reader (N, independent; each owns its pre-allocated copy buffer — Law 2):
//     claim():   L = latestSeq; if L == 0 or L == lastSeq: not fresh
//                else bounded (<= 4 attempts):
//                  k = (L-1) mod M; sB = slotSeq[k]
//                  if sB != L: re-read latestSeq; unchanged -> SKIP this tick
//                    (Law 1: no spin; counted, never silent); changed -> chase
//                  copy slot k -> reader buffer; sA = slotSeq[k]
//                  if sA == L: consistent frame L; dropped = L - lastSeq - 1;
//                    advance lastSeq (FI2: per-slot stamp monotonicity)
//                  else: torn copy; retry on the newest completed frame
//                attempts exhausted: not fresh, counted, never a spin
//
// MEMORY ORDERING — the C port's fenced acq/rel regime, mapped to the JVM's
// VarHandle access modes (docs/PORTS.md §6):
//   Writer  begin:    invalidate = setVolatile (SeqCst store — the JVM's only
//                     SC store mode) + VarHandle.fullFence() BEFORE the fill
//                     cursor is returned. Property P1: no payload word of the
//                     new fill may become visible before the invalidate stamp.
//   Writer  publish:  stamp + latestSeq = setRelease (Release — orders the
//                     fill below it, the publication point). publishes =
//                     getAndAdd (SC — the JVM exposes no relaxed RMW; the
//                     counter is advisory per AXIOM T, immaterial).
//   Reader  claim:    stamps = getAcquire (pairs with the writer's release);
//                     payload words = getOpaque (coherence-only — the JVM
//                     analog of C's relaxed u32 accesses, race-free by
//                     construction); one VarHandle.fullFence() between the
//                     copy and the revalidation load. Property P2: if the
//                     copy observed any overwrite word, the revalidation load
//                     must observe the invalidate-or-newer stamp.
//
// PLATFORM BOUNDARY (declared, not defended): VarHandle requires JVM 9+ /
// Android API 33+. The kernel (Weft.kt) remains the 1:1 path on every API
// level; on older Android the fan-out road is the C ring via JNI — the same
// byte-compatible layout, either road (core/c/fanout.h).
//
// LAW 2: begin/fill/publish/claim allocate nothing (constructors may).
// LAW 1: every path is bounded; a skip or exhausted retry is counted in
//        reader stats, never silent, never a spin.
// LAW 4: honest boundaries — dropped = L - lastSeq - 1 in Long arithmetic
//        assumes the single-writer monotonic contract; multi-canvas renders
//        are approximately synchronized (latest-wins per consumer); the
//        payload contract is u32 words (byte-granular like the C port —
//        float users go through Float.toRawBits()/Float.fromBits()).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

package dev.weft

import java.lang.invoke.MethodHandles
import java.lang.invoke.VarHandle
import java.nio.ByteBuffer
import java.nio.ByteOrder

/// Maximum ring depth. RFC 0004 recommends 4-8; the bound matches the C
/// port's WEFT_FANOUT_MAX_SLOTS so geometry validates identically.
const val WEFT_FANOUT_MAX_SLOTS: Int = 64

/// Bounded claim attempts (same constant and rationale as the TS/C ports: a
/// retry only happens when a NEWER frame completed during the claim, and the
/// newer frame's own slot is self-consistent, so convergence is immediate).
const val WEFT_FANOUT_MAX_CLAIM_ATTEMPTS: Int = 4

/// Claim result record — reader-owned and identity-stable, mutated in place
/// per claim so the hot path allocates nothing (Law 2). Read the fields
/// synchronously after claim(); do not retain the record across claims
/// expecting a snapshot.
class FanoutClaim(
    /** A new consistent frame was claimed this tick. */
    var fresh: Boolean,
    /** Frame seq now held (last consistent if !fresh). */
    var seq: Long,
    /** Frames completed without this reader ever observing them. */
    var dropped: Long
)

/// Advisory reader statistics (AXIOM T: advisory, never a correctness
/// reference). Field names mirror the TS/C ports.
class FanoutReaderStats(
    val reads: Long,
    val fresh: Long,
    val drops: Long,
    val skippedMidOverwrite: Long,
    val tornExhausted: Long
)

/// Advisory broadcaster state (cold path; AXIOM T applies).
class FanoutDebugStats(
    val latestSeq: Long,
    val publishes: Long,
    val slotCount: Int,
    val payloadBytes: Int,
    val slotStamps: LongArray
)

/// Total ring size in bytes for the given geometry (the interop contract —
/// identical to core/ts/fanout.ts and core/c/fanout.c). 0 on bad geometry.
fun weftFanoutRingBytes(payloadBytes: Int, slotCount: Int): Int {
    if (payloadBytes <= 0 || payloadBytes % 4 != 0) return 0
    if (slotCount < 2 || slotCount > WEFT_FANOUT_MAX_SLOTS) return 0
    return 16 + 8 * slotCount + slotCount * payloadBytes
}

// ---------------------------------------------------------------------------
// The fan-out ring: one writer + N readers, M pre-allocated slots, one
// publication point (latestSeq). Field discipline mirrors the kernel:
//   - ring: shared; synchronized ONLY by the stamp protocol above.
//   - wSeq/wSlot/begun: WRITER-PRIVATE (single writer by contract).
// ---------------------------------------------------------------------------

class WeftFanoutBroadcaster(
    /** Per-slot payload capacity in bytes (immutable after init; multiple of 4). */
    val payloadBytes: Int,
    /** Ring depth (immutable after init). RFC 0004 recommends 4-8. */
    val slotCount: Int = 4
) {

    /// The single ring allocation — a DIRECT ByteBuffer, LITTLE_ENDIAN.
    /// Hand it to reader threads directly, or to a JNI peer via
    /// NewDirectByteBuffer(getDirectBufferAddress(ring)) — the bytes are the
    /// interop contract, exactly like posting the SAB in the TS port.
    val ring: ByteBuffer

    // Writer-private frame counter (0 = no begin yet) and slot of the
    // current begin() — single writer by contract, the same discipline as
    // the kernel's thread-private w_work.
    private var wSeq: Long = 0
    private var wSlot: Int = 0
    private var begun: Boolean = false

    init {
        if (payloadBytes <= 0 || payloadBytes % 4 != 0) {
            throw IllegalArgumentException(
                "payloadBytes must be a positive multiple of 4 (got $payloadBytes)"
            )
        }
        if (slotCount < 2 || slotCount > WEFT_FANOUT_MAX_SLOTS) {
            throw IllegalArgumentException(
                "slotCount must be in [2, $WEFT_FANOUT_MAX_SLOTS] (got $slotCount)"
            )
        }
        ring = ByteBuffer.allocateDirect(weftFanoutRingBytes(payloadBytes, slotCount))
            .order(ByteOrder.LITTLE_ENDIAN)
        // Fresh memory is zero-initialized: latestSeq=0 (no frame yet),
        // publishes=0, every slotSeq=0 (all invalidated) — the same
        // invariants a fresh SAB gives the TS port. Stated for review.
    }

    /// Byte offset of slot k's payload region.
    private fun slotBase(k: Int): Int = 16 + 8 * slotCount + k * payloadBytes

    /// Begin the next frame: bumps the frame counter, INVALIDATES the target
    /// slot's stamp (SeqCst store + fullFence — property P1) and returns the
    /// slot's payload as a ByteBuffer slice. The view stays valid until the
    /// next begin(). Plain writes through it are visible to readers only
    /// after publish() (the bracket discipline, TS-port stance); use fill()
    /// for the race-free opaque-word path (C-port stance). Zero allocation.
    fun begin(): ByteBuffer {
        wSeq += 1
        wSlot = ((wSeq - 1) % slotCount).toInt()
        begun = true
        FanoutVh.stampStoreVolatile(ring, 8 * (CTRL_SLOTSEQ + wSlot), 0L)
        VarHandle.fullFence()
        val base = slotBase(wSlot)
        val dup = ring.duplicate()
        (dup as java.nio.Buffer).position(base)
        (dup as java.nio.Buffer).limit(base + payloadBytes)
        val sliced = dup.slice() as ByteBuffer
        sliced.order(ByteOrder.LITTLE_ENDIAN)
        return sliced
    }

    /// Fill the begun slot from src via opaque u32 word stores (the
    /// race-free fill path — strict-JSR-133-clean, the analog of the C port's
    /// relaxed-atomic weft_fanout_fill). words must be <= payloadBytes/4.
    /// Returns words written, or -1 on bad args / no begin(). Zero allocation.
    fun fill(src: IntArray, words: Int): Int {
        if (!begun) return -1
        if (words < 0 || words * 4 > payloadBytes) return -1
        val base = slotBase(wSlot)
        for (w in 0 until words) {
            FanoutVh.wordStoreOpaque(ring, base + 4 * w, src[w])
        }
        return words
    }

    /// Publish the begun frame: stamp the slot (Release), flip latestSeq
    /// (Release — the publication point), bump publishes (advisory).
    /// Returns the published frame seq, or 0 if no begin() ever ran (a
    /// detectable no-op, not an error — same as the TS/C ports). Zero
    /// allocation.
    fun publish(): Long {
        if (wSeq == 0L) return 0L
        FanoutVh.stampStoreRelease(ring, 8 * (CTRL_SLOTSEQ + wSlot), wSeq)
        FanoutVh.stampStoreRelease(ring, 8 * CTRL_LATEST, wSeq)
        FanoutVh.publishesAdd(ring, 1L)
        return wSeq
    }

    /// Create a reader bound to this ring (same process — pass `ring` to
    /// another constructor variant for cross-thread/JNI attach). Geometry
    /// travels with the constructor arguments, the same way the kernel's
    /// payloadMax does.
    fun createReader(): WeftFanoutReader = WeftFanoutReader(ring, payloadBytes, slotCount)

    /// Advisory state snapshot (cold path — allocates; never call per frame).
    fun debugStats(): FanoutDebugStats {
        val stamps = LongArray(slotCount)
        for (k in 0 until slotCount) {
            stamps[k] = FanoutVh.stampLoadAcquire(ring, 8 * (CTRL_SLOTSEQ + k))
        }
        return FanoutDebugStats(
            latestSeq = FanoutVh.stampLoadAcquire(ring, 8 * CTRL_LATEST),
            publishes = FanoutVh.stampLoadAcquire(ring, 8 * CTRL_PUBLISHES),
            slotCount = slotCount,
            payloadBytes = payloadBytes,
            slotStamps = stamps
        )
    }

    companion object {
        /// Control-block i64 indices (byte offset = 8 * index — the layout
        /// contract shared with the TS BigInt64Array ctrl and the C ctrl).
        const val CTRL_LATEST = 0
        const val CTRL_PUBLISHES = 1
        const val CTRL_SLOTSEQ = 2
    }
}

// ---------------------------------------------------------------------------
// Reader — the consumer side. N per ring, each fully independent.
// ---------------------------------------------------------------------------

class WeftFanoutReader(
    /// The ring bytes (borrowed; caller keeps it alive). May be the
    /// broadcaster's direct buffer, a JNI peer's buffer, or any ByteBuffer
    /// whose capacity matches the geometry — the wire protocol is the bytes.
    ring: ByteBuffer,
    /** Per-slot payload capacity in bytes (validated against ring capacity). */
    val payloadBytes: Int,
    /** Ring depth (validated against ring capacity). */
    val slotCount: Int = 4
) {

    val ring: ByteBuffer
    private val slotBaseOff: Int

    /// The reader's own pre-allocated copy buffer — u32 words (payloadBytes/4),
    /// stable identity for the consumer's lifetime; holds frame data only
    /// after a fresh claim (Law 2).
    private val target: IntArray

    /// Last frame seq this reader has held consistent (0 = none yet).
    private var lastSeq: Long = 0

    /// Preallocated, identity-stable claim record (mutated per claim).
    private val rec = FanoutClaim(fresh = false, seq = 0, dropped = 0)

    // Reader-private statistics (advisory; exposed via stats()).
    private var nReads = 0L
    private var nFresh = 0L
    private var nDrops = 0L
    private var nSkip = 0L
    private var nExhausted = 0L

    init {
        if (payloadBytes <= 0 || payloadBytes % 4 != 0) {
            throw IllegalArgumentException(
                "payloadBytes must be a positive multiple of 4 (got $payloadBytes)"
            )
        }
        if (slotCount < 2 || slotCount > WEFT_FANOUT_MAX_SLOTS) {
            throw IllegalArgumentException(
                "slotCount must be in [2, $WEFT_FANOUT_MAX_SLOTS] (got $slotCount)"
            )
        }
        val expect = weftFanoutRingBytes(payloadBytes, slotCount)
        if (ring.capacity() != expect) {
            throw IllegalArgumentException(
                "ring geometry mismatch: expected $expect bytes for " +
                    "$slotCount slots x $payloadBytes bytes, got ${ring.capacity()}"
            )
        }
        this.ring = ring
        this.slotBaseOff = 16 + 8 * slotCount
        this.target = IntArray(payloadBytes / 4)
    }

    /// Claim the freshest completed frame into this reader's buffer. Never
    /// blocks, never spins unboundedly, never fails: a tick on which no
    /// consistent newer frame is available returns fresh=false and the reader
    /// keeps its last consistent frame. Returns the reader-owned claim record
    /// (identity-stable, mutated in place — zero allocation per claim).
    ///
    /// `dropped` counts frames that completed without this reader ever
    /// observing them (RFC 0004 per-reader drop accounting). The telescoping
    /// identity sum(dropped) == lastSeq - freshClaims holds exactly.
    fun claim(): FanoutClaim {
        nReads++
        var L = FanoutVh.stampLoadAcquire(ring, 8 * WeftFanoutBroadcaster.CTRL_LATEST)
        if (L == 0L || L == lastSeq) {
            rec.fresh = false
            rec.seq = lastSeq
            rec.dropped = 0
            return rec
        }
        for (attempt in 0 until WEFT_FANOUT_MAX_CLAIM_ATTEMPTS) {
            val k = (((L - 1) % slotCount)).toInt()
            val sB = FanoutVh.stampLoadAcquire(ring, 8 * (WeftFanoutBroadcaster.CTRL_SLOTSEQ + k))
            if (sB != L) {
                // Slot mid-overwrite (stamp 0) or already re-stamped by a
                // newer frame. Re-read latestSeq: unchanged -> graceful skip
                // (Law 1); changed -> a newer frame completed, chase it.
                val L2 = FanoutVh.stampLoadAcquire(ring, 8 * WeftFanoutBroadcaster.CTRL_LATEST)
                if (L2 == L) {
                    nSkip++
                    rec.fresh = false
                    rec.seq = lastSeq
                    rec.dropped = 0
                    return rec
                }
                L = L2
                continue
            }
            // Stamp matches frame L: copy, then re-validate (P2 fence below).
            val base = slotBaseOff + k * payloadBytes
            for (w in target.indices) {
                target[w] = FanoutVh.wordLoadOpaque(ring, base + 4 * w)
            }
            // Property P2: the copy is ordered before the revalidation load
            // by a full fence — if the copy observed any overwrite word, the
            // revalidation must observe the invalidate-or-newer stamp.
            VarHandle.fullFence()
            val sA = FanoutVh.stampLoadAcquire(ring, 8 * (WeftFanoutBroadcaster.CTRL_SLOTSEQ + k))
            if (sA == L) {
                // Consistent frame L (FI2: unchanged stamp proves no
                // overwrite began during the copy).
                val dropped = L - lastSeq - 1
                nDrops += dropped
                lastSeq = L
                nFresh++
                rec.fresh = true
                rec.seq = lastSeq
                rec.dropped = dropped
                return rec
            }
            // Torn copy detected — retry on the newest completed frame.
            L = FanoutVh.stampLoadAcquire(ring, 8 * WeftFanoutBroadcaster.CTRL_LATEST)
        }
        // Bounded retries exhausted: keep the last consistent frame. Counted,
        // never silent, never a spin (Law 1).
        nExhausted++
        rec.fresh = false
        rec.seq = lastSeq
        rec.dropped = 0
        return rec
    }

    /// The reader's pre-allocated copy buffer as u32 words (stable identity).
    /// Meaningful after a fresh claim; read it live before the next claim —
    /// the same discipline as the kernel's rLive (A3). Float users:
    /// Float.fromBits(view()[i]).
    fun view(): IntArray = target

    /// Advisory statistics snapshot (cold path — allocates; AXIOM T:
    /// advisory, never a correctness reference).
    fun stats(): FanoutReaderStats = FanoutReaderStats(
        reads = nReads,
        fresh = nFresh,
        drops = nDrops,
        skippedMidOverwrite = nSkip,
        tornExhausted = nExhausted
    )
}

// ---------------------------------------------------------------------------
// VarHandle plumbing — the JVM's only acquire/release-capable primitive over
// raw buffer bytes. Views are LITTLE_ENDIAN (the wire contract); every
// access goes through these helpers so the ordering regime of the header is
// auditable in one place (the same review posture as the C port's inline
// fan_stamp_store/fan_stamp_load pair).
// ---------------------------------------------------------------------------

internal object FanoutVh {
    private val LONG_VIEW: VarHandle =
        MethodHandles.byteBufferViewVarHandle(LongArray::class.java, ByteOrder.LITTLE_ENDIAN)
    private val INT_VIEW: VarHandle =
        MethodHandles.byteBufferViewVarHandle(IntArray::class.java, ByteOrder.LITTLE_ENDIAN)

    private val LONG_GET_ACQUIRE: java.lang.invoke.MethodHandle =
        LONG_VIEW.toMethodHandle(VarHandle.AccessMode.GET_ACQUIRE)
    private val LONG_SET_RELEASE: java.lang.invoke.MethodHandle =
        LONG_VIEW.toMethodHandle(VarHandle.AccessMode.SET_RELEASE)
    private val LONG_SET_VOLATILE: java.lang.invoke.MethodHandle =
        LONG_VIEW.toMethodHandle(VarHandle.AccessMode.SET_VOLATILE)
    private val LONG_GET_AND_ADD: java.lang.invoke.MethodHandle =
        LONG_VIEW.toMethodHandle(VarHandle.AccessMode.GET_AND_ADD)
    private val INT_GET_OPAQUE: java.lang.invoke.MethodHandle =
        INT_VIEW.toMethodHandle(VarHandle.AccessMode.GET_OPAQUE)
    private val INT_SET_OPAQUE: java.lang.invoke.MethodHandle =
        INT_VIEW.toMethodHandle(VarHandle.AccessMode.SET_OPAQUE)

    /** Acquire load of an i64 ctrl slot (getAcquire; pairs with stampStoreRelease). */
    fun stampLoadAcquire(buf: ByteBuffer, byteOffset: Int): Long =
        LONG_GET_ACQUIRE.invokeWithArguments(buf, byteOffset) as Long

    /** Release store of an i64 ctrl slot (setRelease; the publication point). */
    fun stampStoreRelease(buf: ByteBuffer, byteOffset: Int, v: Long) {
        LONG_SET_RELEASE.invokeWithArguments(buf, byteOffset, v)
    }

    /** SeqCst store of an i64 ctrl slot (setVolatile; the FI1 invalidate). */
    fun stampStoreVolatile(buf: ByteBuffer, byteOffset: Int, v: Long) {
        LONG_SET_VOLATILE.invokeWithArguments(buf, byteOffset, v)
    }

    /** SC fetch-add on the publishes telemetry counter (advisory; the JVM
     *  exposes no relaxed RMW mode — declared divergence, AXIOM T). */
    fun publishesAdd(buf: ByteBuffer, delta: Long): Long =
        LONG_GET_AND_ADD.invokeWithArguments(buf, 8 * WeftFanoutBroadcaster.CTRL_PUBLISHES, delta) as Long

    /** Opaque (coherence-only) load of a u32 payload word (getOpaque) — the JVM analog
     *  of C's relaxed u32 accesses: race-free by construction, ordering
     *  carried by the stamp bracket, not the words. */
    fun wordLoadOpaque(buf: ByteBuffer, byteOffset: Int): Int =
        INT_GET_OPAQUE.invokeWithArguments(buf, byteOffset) as Int

    /** Opaque store of a u32 payload word (setOpaque; the race-free fill path). */
    fun wordStoreOpaque(buf: ByteBuffer, byteOffset: Int, v: Int) {
        INT_SET_OPAQUE.invokeWithArguments(buf, byteOffset, v)
    }
}
