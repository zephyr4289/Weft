// FanoutCompat.kt — RFC 0004 fan-out ring, API<33 compat regime (Kotlin)
//
// WHY EXISTS: the VarHandle ring (Fanout.kt) binds its access modes via
// MethodHandles.byteBufferViewVarHandle — JVM 9+ / Android API 33+ only.
// On API < 33 that class path is a hard NoClassDefFoundError at first
// touch, which means the ring was UNREACHABLE below 33 despite the header's
// "the kernel remains the 1:1 path" note — the fan-out road just died
// there (or silently routed through JNI, which requires the native library
// a pure-JVM consumer does not have). This module completes the matrix:
// the SAME protocol, byte-compatible LAYOUT CONTRACT, and F-series gates
// expressed in what API < 33 honestly has.
//
// MEMORY ORDERING — the honest pre-33 regime (docs/PORTS.md §6):
//   Stamps: java.util.concurrent.atomic.AtomicLongArray. get/set carry
//   VOLATILE semantics (JSR-133 §8.1: volatile read/writes are sequentially
//   consistent) — exactly the SC-stamp regime the Swift port pays (TS-port
//   stance: SC stamps carry the P1/P2 bracket duty; there is no standalone
//   fullFence on old APIs and none is needed under SC stamps). The
//   invalidate, re-stamp, latestSeq and revalidation loads are all SC.
//   Payload: PLAIN IntArray words under the bracket discipline (the TS
//   port's stance): plain writes before the publish stamp are ordered by
//   the publishing volatile store; plain reads before the revalidation
//   volatile load are ordered by it. getAndAddIncrement on the telemetry
//   counter is an SC RMW (the JVM exposes no relaxed RMW on old APIs —
//   same declared divergence as the VarHandle ring, AXIOM T).
//   Divergence vs the VarHandle ring: payload words are plain (not opaque)
//   — the price of the bracket discipline on pre-33, identical in kind to
//   the TS port's plain Float32 stores.
//
// LAYOUT: the control block lives in an AtomicLongArray with the SAME
// indices as the wire contract (0 = latestSeq, 1 = publishes, 2+k =
// slotSeq[k]); the payload lives in a plain IntArray sized
// slotCount * payloadWords. The wire BYTES of Fanout.kt's ByteBuffer are
// NOT shared with this ring (an AtomicLongArray is not memory-mappable) —
// cross-port interop below API 33 goes through the C ring via JNI, same
// as before; this ring's job is the pure-JVM multi-consumer road on every
// API level, with the protocol and gates identical.
//
// LAW 2: begin/fill/publish/claim allocate nothing (constructors may).
// LAW 1: every path bounded; skips and exhausted retries counted, never
//        silent, never a spin.
//
// STATUS: CI-PROVEN on the JVM (FanoutCompatMatrixTest runs the F-series
// over BOTH regimes); real-device API<33 legs are the emulator matrix
// (android-emulator.yml).

package dev.weft

import java.util.concurrent.atomic.AtomicLongArray

/// Maximum ring depth (same bound as the VarHandle ring).
const val WEFT_FANOUT_COMPAT_MAX_SLOTS: Int = 64

/// Bounded claim attempts (same constant and rationale as every port).
const val WEFT_FANOUT_COMPAT_MAX_CLAIM_ATTEMPTS: Int = 4

/// Control-block long indices — identical to the VarHandle ring's byte
/// layout contract (byte offset = 8 * index over the wire).
object FanoutCompatCtrl {
    const val LATEST = 0
    const val PUBLISHES = 1
    const val SLOTSEQ = 2
}

/// Total payload words for a geometry (mirror of weftFanoutRingBytes's
/// role for the plain-array regime). 0 on bad geometry.
fun weftFanoutCompatPayloadWords(payloadBytes: Int, slotCount: Int): Int {
    if (payloadBytes <= 0 || payloadBytes % 4 != 0) return 0
    if (slotCount < 2 || slotCount > WEFT_FANOUT_COMPAT_MAX_SLOTS) return 0
    return slotCount * (payloadBytes / 4)
}

/// The API<33 fan-out broadcaster. Same single-writer contract, same
/// publication point, same protocol shape as WeftFanoutBroadcaster.
class WeftFanoutBroadcasterCompat(
    /** Per-slot payload capacity in bytes (immutable; multiple of 4). */
    val payloadBytes: Int,
    /** Ring depth (immutable). RFC 0004 recommends 4-8. */
    val slotCount: Int = 4
) {

    /// Control block — volatile get/set (SC) on every access.
    val ctrl: AtomicLongArray

    /// Payload region — plain words under the bracket discipline.
    val payload: IntArray

    // Writer-private (single writer by contract — the kernel's discipline).
    private var wSeq: Long = 0
    private var wSlot: Int = 0
    private var begun: Boolean = false

    init {
        if (payloadBytes <= 0 || payloadBytes % 4 != 0) {
            throw IllegalArgumentException(
                "payloadBytes must be a positive multiple of 4 (got $payloadBytes)"
            )
        }
        if (slotCount < 2 || slotCount > WEFT_FANOUT_COMPAT_MAX_SLOTS) {
            throw IllegalArgumentException(
                "slotCount must be in [2, $WEFT_FANOUT_COMPAT_MAX_SLOTS] (got $slotCount)"
            )
        }
        ctrl = AtomicLongArray(2 + slotCount) // zero-init: latest=0, publishes=0, stamps=0
        payload = IntArray(slotCount * (payloadBytes / 4))
    }

    /// Word base of slot k in [payload].
    fun slotWordBase(k: Int): Int = k * (payloadBytes / 4)

    /// Begin the next frame: INVALIDATES the target slot's stamp (SC store,
    /// property P1 — the invalidate is visible before any payload word of
    /// the new fill) and returns the slot's word base. Write payload words
    /// directly at [payload][base + w] (plain stores, bracket discipline)
    /// or use fill(). Zero allocation.
    fun begin(): Int {
        wSeq += 1
        wSlot = ((wSeq - 1) % slotCount).toInt()
        begun = true
        ctrl.set(FanoutCompatCtrl.SLOTSEQ + wSlot, 0L)
        return slotWordBase(wSlot)
    }

    /// The slot base of the current begin() (for readers and tests).
    fun currentSlotBase(): Int = slotWordBase(wSlot)

    /// Fill the begun slot from src via plain word stores (the TS-port
    /// stance — ordering carried by the publish stamp). words must be
    /// <= payloadBytes/4. Returns words written, or -1 on bad args /
    /// no begin(). Zero allocation.
    fun fill(src: IntArray, words: Int): Int {
        if (!begun) return -1
        if (words < 0 || words * 4 > payloadBytes) return -1
        val base = slotWordBase(wSlot)
        for (w in 0 until words) {
            payload[base + w] = src[w]
        }
        return words
    }

    /// Publish the begun frame: stamp the slot (SC), flip latestSeq (SC —
    /// the publication point), bump publishes (SC RMW, advisory). Returns
    /// the published frame seq, or 0 if no begin() ever ran (a detectable
    /// no-op — same as every port). Zero allocation.
    fun publish(): Long {
        if (wSeq == 0L) return 0L
        ctrl.set(FanoutCompatCtrl.SLOTSEQ + wSlot, wSeq)
        ctrl.set(FanoutCompatCtrl.LATEST, wSeq)
        ctrl.incrementAndGet(FanoutCompatCtrl.PUBLISHES)
        return wSeq
    }

    /// Create a reader bound to this ring (same process).
    fun createReader(): WeftFanoutReaderCompat =
        WeftFanoutReaderCompat(ctrl, payload, payloadBytes, slotCount)

    /// Advisory state snapshot (cold path — allocates; never per frame).
    fun debugStats(): FanoutDebugStats {
        val stamps = LongArray(slotCount)
        for (k in 0 until slotCount) {
            stamps[k] = ctrl.get(FanoutCompatCtrl.SLOTSEQ + k)
        }
        return FanoutDebugStats(
            latestSeq = ctrl.get(FanoutCompatCtrl.LATEST),
            publishes = ctrl.get(FanoutCompatCtrl.PUBLISHES),
            slotCount = slotCount,
            payloadBytes = payloadBytes,
            slotStamps = stamps
        )
    }
}

/// The API<33 reader. Same claim protocol, same bounded-retry shape, same
/// per-reader drop accounting as the VarHandle ring.
class WeftFanoutReaderCompat(
    /// The broadcaster's control block (shared; SC get/set only).
    val ctrl: AtomicLongArray,
    /// The broadcaster's payload region (shared; plain words).
    val payload: IntArray,
    /** Per-slot payload capacity in bytes (validated against payload size). */
    val payloadBytes: Int,
    /** Ring depth (validated). */
    val slotCount: Int = 4
) {

    /// The reader's own pre-allocated copy buffer — u32 words, stable
    /// identity (Law 2); holds frame data only after a fresh claim.
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
        if (slotCount < 2 || slotCount > WEFT_FANOUT_COMPAT_MAX_SLOTS) {
            throw IllegalArgumentException(
                "slotCount must be in [2, $WEFT_FANOUT_COMPAT_MAX_SLOTS] (got $slotCount)"
            )
        }
        val expect = slotCount * (payloadBytes / 4)
        if (payload.size != expect) {
            throw IllegalArgumentException(
                "ring geometry mismatch: expected $expect payload words for " +
                    "$slotCount slots x $payloadBytes bytes, got ${payload.size}"
            )
        }
        target = IntArray(payloadBytes / 4)
    }

    /// Claim the freshest completed frame. Same protocol as the VarHandle
    /// ring's claim() — bounded (<= 4 attempts), counted, latest-wins,
    /// per-reader telescoping drop accounting. Zero allocation.
    fun claim(): FanoutClaim {
        nReads++
        var L = ctrl.get(FanoutCompatCtrl.LATEST)
        if (L == 0L || L == lastSeq) {
            rec.fresh = false
            rec.seq = lastSeq
            rec.dropped = 0
            return rec
        }
        for (attempt in 0 until WEFT_FANOUT_COMPAT_MAX_CLAIM_ATTEMPTS) {
            val k = ((L - 1) % slotCount).toInt()
            val sB = ctrl.get(FanoutCompatCtrl.SLOTSEQ + k)
            if (sB != L) {
                // Mid-overwrite or re-stamped: re-read latest — unchanged ->
                // graceful skip (Law 1); changed -> chase.
                val L2 = ctrl.get(FanoutCompatCtrl.LATEST)
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
            // Stamp matches frame L: copy (plain reads — the publish stamp
            // ordered them), then re-validate (SC load — carries P2 here
            // under the SC-stamp regime).
            val base = k * (payloadBytes / 4)
            for (w in target.indices) {
                target[w] = payload[base + w]
            }
            val sA = ctrl.get(FanoutCompatCtrl.SLOTSEQ + k)
            if (sA == L) {
                val dropped = L - lastSeq - 1
                nDrops += dropped
                lastSeq = L
                nFresh++
                rec.fresh = true
                rec.seq = lastSeq
                rec.dropped = dropped
                return rec
            }
            // Torn copy — retry on the newest completed frame.
            L = ctrl.get(FanoutCompatCtrl.LATEST)
        }
        nExhausted++
        rec.fresh = false
        rec.seq = lastSeq
        rec.dropped = 0
        return rec
    }

    /// The reader's pre-allocated copy buffer (stable identity — Law 2).
    fun view(): IntArray = target

    /// Advisory statistics snapshot (cold path — allocates; AXIOM T).
    fun stats(): FanoutReaderStats = FanoutReaderStats(
        reads = nReads,
        fresh = nFresh,
        drops = nDrops,
        skippedMidOverwrite = nSkip,
        tornExhausted = nExhausted
    )
}

/// The API gate: production routing between the two Kotlin regimes. API 33+
/// wants the VarHandle ring (opaque payload words, the C-port stance);
/// below 33, the compat ring (SC stamps + bracket payload, the TS-port
/// stance). Both carry the same protocol contract and the same F-series
/// gates — the matrix test runs BOTH on every host so neither regime can
/// rot while the other is exercised.
object WeftFanoutFactory {
    /// true when the host should take the VarHandle road.
    fun varHandleAvailable(apiLevel: Int): Boolean = apiLevel >= 33

    /// The broadcast side of the routed pair. Caller casts or uses the
    /// common F-series harness (the test matrix) over the returned type.
    fun broadcasterFor(apiLevel: Int, payloadBytes: Int, slotCount: Int): Any =
        if (varHandleAvailable(apiLevel)) {
            WeftFanoutBroadcaster(payloadBytes, slotCount)
        } else {
            WeftFanoutBroadcasterCompat(payloadBytes, slotCount)
        }
}
