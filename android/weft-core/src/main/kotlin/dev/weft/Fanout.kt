// Fanout.kt — RFC 0004: Multi-Consumer Fan-Out ring, Kotlin/JVM port (driver layer)
//
// WHY EXISTS: RFC 0004 (Accepted as driver-layer pattern, round-6 §4). The
// Triad kernel is 1-writer/1-reader by design; applications that need a
// primary canvas, a minimap, a flight recorder, and a network visualizer on
// one stream cannot bind N readers to one Triad. This is the port-parity
// sibling of core/ts/fanout.ts (same protocol, same claim algorithm, same
// statistics names) so the JVM carries the same capability the TS port has
// shipped since the fan-out driver layer landed.
//
// SUBSTRATE HONESTY (the one structural difference from the TS port, stated
// up front): the JVM has no SharedArrayBuffer, so there is no byte-layout
// ring to post across threads. The control block lives in an AtomicLongArray
// (latestSeq, publishes, slotSeq[0..M)) and each slot is a heap FloatArray.
// This port is therefore a SEMANTICS port of the ring protocol — zero
// allocation on begin/publish/claim (Law 2), bounded retries (Law 1),
// advisory stats (AXIOM T) — not a byte-compatible one. Cross-language ring
// interop on Android/JVM goes through the JNI bridge to the C ring
// (TriadNative fanout* over weft_jni.c + core/c/fanout.c), which IS
// byte-compatible with the TS layout (16 + 8M ctrl + M*payload, see
// core/c/fanout.h). Sharing THIS ring across threads means sharing the
// broadcaster object reference — ordinary JVM heap sharing, no posting step.
//
// MEMORY ORDERING (port-mapping note, in the PORTS.md §1 tradition):
//   ctrl: TS uses seqcst Atomics on a BigInt64Array (JS Atomics give no
//         choice — "safe but noisier", 06-PITFALLS §4). JVM equivalent:
//         AtomicLongArray volatile accesses. JSR-133 gives volatiles
//         acquire/release semantics per variable — the same regime the C
//         ring ships as its default (Release stamps / Acquire loads), and
//         strictly stronger than plain fields.
//   payload words: plain FloatArray element writes/reads, bracketed by the
//         stamp protocol exactly as in TS (Float32Array) and C (relaxed
//         u32 words). JLS §17.7: 32-bit accesses do not tear (only
//         long/double may), so a racy word read observes either the old or
//         the new float — the seqlock bracket (sB read ... copy ... sA
//         revalidation) detects any mixed read, which is the whole point of
//         the bracket. The bracket's visibility on real JVMs rides the
//         volatile barriers (JSR-133 cookbook: the invalidate's trailing
//         StoreLoad keeps the fill below it; the revalidation's leading
//         LoadLoad keeps the copy above it) — the same class of
//         implementation-backed reasoning the TS port's seqcst stance
//         carries, stated rather than hidden.
//
// LAW 2: begin/publish/claim allocate nothing (the claim record is
//        reader-owned and mutated in place, exactly like the TS port).
// LAW 1: every path is bounded; skips and exhausted retries are counted in
//        reader stats, never silent, never a spin.
// LAW 4: dropped = L - lastSeq - 1 in Long arithmetic assumes the
//        single-writer monotonic contract (same boundary as TS/C ports).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.
// JVM semantics are covered by FanoutTest.kt (protocol + threaded torture);
// the Android build runs it via gradlew test (host JVM).

package dev.weft

import java.util.concurrent.atomic.AtomicLongArray

/// One claim's result. MUTATED IN PLACE per claim — the record is
/// reader-owned and identity-stable, so the hot path allocates nothing
/// (Law 2 — same discipline as the TS port's FanoutClaim). Read the fields
/// synchronously after claim(); do not retain the record across claims
/// expecting a snapshot.
class FanoutClaim {
    var fresh: Boolean = false
    var seq: Long = 0
    var dropped: Long = 0
}

/// Advisory reader statistics (cold path — allocates; AXIOM T: advisory,
/// never a correctness reference). Field names mirror the TS port.
class FanoutReaderStats(
    val reads: Long,
    val fresh: Long,
    val drops: Long,
    val skippedMidOverwrite: Long,
    val tornExhausted: Long,
)

/// Advisory broadcaster state (cold path — allocates). Longs, not BigInt —
/// the JVM has real i64s (the TS port's bigint fields exist only because
/// JS numbers cannot hold u64; Kotlin can, like the C port).
class FanoutDebugStats(
    val latestSeq: Long,
    val publishes: Long,
    val slotCount: Int,
    val payloadFloats: Int,
    val slotStamps: LongArray,
)

// Control-block indices (AtomicLongArray slots — same order as the TS port).
private const val IDX_LATEST = 0L.toInt() // 0
private const val IDX_PUBLISHES = 1
private const val IDX_SLOTSEQ = 2 // + k

/// Bounded claim attempts. A retry only happens when a NEWER frame completed
/// during the claim; the newer frame's own slot is self-consistent, so
/// convergence is immediate. 4 is generous, not tuned (TS/C parity).
private const val MAX_CLAIM_ATTEMPTS = 4

/// Ring depth bound (C parity: WEFT_FANOUT_MAX_SLOTS).
const val FANOUT_MAX_SLOTS = 64

// ---------------------------------------------------------------------------
// Broadcaster — the writer side. One per stream. Single writer by contract
// (the same contract as the kernel's writer; sharing the BEGIN/PUBLISH pair
// across threads without external coordination is a caller error).
// ---------------------------------------------------------------------------

class WeftFanoutBroadcaster(val payloadFloats: Int, val slotCount: Int = 4) {

    /// Control block: [latestSeq, publishes, slotSeq[0..M)). latestSeq = 0
    /// means "no frame yet"; slotSeq[k] = 0 means "invalidated / mid-fill".
    /// (Byte-layout parity exists in the C ring; here the array IS the ctrl.)
    internal val ctrl = AtomicLongArray(2 + slotCount)

    /// Payload slots. Slot k holds frame (seq) where (seq - 1) mod M == k.
    /// Cached once at construction; begin() hands out the live view (Law 2).
    internal val slots = Array(slotCount) { FloatArray(payloadFloats) }

    /// Writer-private frame counter (plain — single writer by contract).
    private var wSeq: Long = 0

    init {
        require(payloadFloats >= 1) { "payloadFloats must be >= 1 (got $payloadFloats)" }
        require(slotCount in 2..FANOUT_MAX_SLOTS) { "slotCount must be in 2..64 (got $slotCount)" }
        // AtomicLongArray zero-initializes: latestSeq=0, publishes=0, every
        // slotSeq=0 (all invalidated) — the fresh-ring invariants, same as a
        // fresh SAB gives the TS port.
    }

    /// Live write cursor for the NEXT frame: the slot's FloatArray. Its
    /// stamp is invalidated BEFORE the view is returned, so any reader
    /// targeting an older frame in this slot detects the overwrite (FI1
    /// bracket). The view stays valid until the next begin(). Zero
    /// allocation per call.
    fun begin(): FloatArray {
        wSeq += 1
        val k = ((wSeq - 1) % slotCount).toInt()
        ctrl.set(IDX_SLOTSEQ + k, 0L) // volatile store — the invalidate
        return slots[k]
    }

    /// Publish the begun frame: stamp the slot, then flip latestSeq (both
    /// volatile stores). The payload fill happened between begin() and here,
    /// bracketed by the two stamps. Returns the published frame seq, or 0 if
    /// no begin() preceded (nothing is published — a detectable no-op, not
    /// an error; TS/C parity).
    fun publish(): Long {
        if (wSeq == 0L) return 0L
        val k = ((wSeq - 1) % slotCount).toInt()
        ctrl.set(IDX_SLOTSEQ + k, wSeq)   // volatile — release: orders the fill
        ctrl.set(IDX_LATEST, wSeq)        // volatile — the publication point
        ctrl.incrementAndGet(IDX_PUBLISHES) // advisory telemetry (AXIOM T)
        return wSeq
    }

    /// Create a reader bound to this ring (same process — share the
    /// broadcaster reference across threads; there is no SAB to post).
    fun createReader(): WeftFanoutReader = WeftFanoutReader(this)

    /// Advisory state snapshot (cold path — allocates; never call per frame).
    fun debugStats(): FanoutDebugStats {
        val stamps = LongArray(slotCount)
        for (k in 0 until slotCount) stamps[k] = ctrl.get(IDX_SLOTSEQ + k).toLong()
        return FanoutDebugStats(
            latestSeq = ctrl.get(IDX_LATEST),
            publishes = ctrl.get(IDX_PUBLISHES),
            slotCount = slotCount,
            payloadFloats = payloadFloats,
            slotStamps = stamps,
        )
    }

    /// Destroy: JVM GC owns the arrays; explicit no-op for API parity with
    /// the C ring's weft_fanout_destroy (the same stance as Weft.destroy()).
    fun destroy() {}
}

// ---------------------------------------------------------------------------
// Reader — the consumer side. N per ring, each fully independent.
// ---------------------------------------------------------------------------

class WeftFanoutReader internal constructor(
    broadcaster: WeftFanoutBroadcaster,
) {
    private val ctrl = broadcaster.ctrl
    private val slots = broadcaster.slots
    val payloadFloats = broadcaster.payloadFloats
    val slotCount = broadcaster.slotCount

    /// The reader's own pre-allocated copy buffer — "each reader polls its
    /// own pre-allocated buffer" (RFC 0004 §Guide). Stable identity for the
    /// consumer's lifetime; holds frame data only after a fresh claim.
    private val target = FloatArray(payloadFloats)

    /// Last frame seq this reader has held consistent (0 = none yet).
    private var lastSeq: Long = 0

    /// Pre-allocated, identity-stable claim record (mutated per claim).
    private val rec = FanoutClaim()

    // Reader-private statistics (advisory; exposed via stats()).
    private var nReads: Long = 0
    private var nFresh: Long = 0
    private var nDrops: Long = 0
    private var nSkip: Long = 0
    private var nExhausted: Long = 0

    /// Claim the freshest completed frame into this reader's buffer. Never
    /// blocks, never spins unboundedly, never fails: a tick on which no
    /// consistent newer frame is available returns fresh=false and the
    /// reader keeps its last consistent frame. Returns the reader-owned
    /// claim record (identity-stable, mutated in place — zero allocation
    /// per claim).
    ///
    /// `dropped` counts frames that completed without this reader ever
    /// observing them (RFC 0004 §Reference: per-reader drop accounting).
    /// The telescoping identity sum(dropped) = lastSeq - freshClaims holds
    /// exactly. Mirrors core/ts/fanout.ts claim() line for line.
    fun claim(): FanoutClaim {
        nReads++
        var L: Long = ctrl.get(IDX_LATEST) // volatile read — acquire
        if (L <= 0L || L == lastSeq) {
            rec.fresh = false
            rec.seq = lastSeq
            rec.dropped = 0
            return rec
        }
        for (attempt in 0 until MAX_CLAIM_ATTEMPTS) {
            val k = ((L - 1) % slotCount).toInt()
            val sB = ctrl.get(IDX_SLOTSEQ + k) // volatile read
            if (sB != L) {
                // Slot mid-overwrite (stamp 0) or already re-stamped by a
                // newer frame. Re-read latestSeq: unchanged means the writer
                // is mid-fill on our slot — skip the tick (Law 1); changed
                // means a newer frame completed — chase it.
                val L2 = ctrl.get(IDX_LATEST)
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
            // Stamp matches frame L: copy, then re-validate the stamp.
            System.arraycopy(slots[k], 0, target, 0, payloadFloats)
            val sA = ctrl.get(IDX_SLOTSEQ + k) // volatile read — revalidation
            if (sA == L) {
                // Consistent frame L. Slot stamps are strictly monotonic per
                // slot (each overwrite re-stamps with a higher frame seq), so
                // an unchanged stamp proves no overwrite began during the
                // copy (FI2).
                val dropped = L - lastSeq - 1
                nDrops += dropped
                lastSeq = L
                nFresh++
                rec.fresh = true
                rec.seq = lastSeq
                rec.dropped = dropped
                return rec
            }
            // Torn copy detected (an overwrite began mid-copy). Retry on the
            // newest completed frame.
            L = ctrl.get(IDX_LATEST)
        }
        // Bounded retries exhausted: keep the last consistent frame.
        // Counted, never silent, never a spin (Law 1).
        nExhausted++
        rec.fresh = false
        rec.seq = lastSeq
        rec.dropped = 0
        return rec
    }

    /// The reader's pre-allocated copy buffer (FloatArray, stable identity).
    /// Meaningful after a fresh claim(); overwritten by the next fresh claim
    /// — read it live in the Draw phase, the same discipline as rLive() (A3).
    fun view(): FloatArray = target

    /// Advisory statistics snapshot (cold path — allocates; AXIOM T).
    fun stats(): FanoutReaderStats =
        FanoutReaderStats(nReads, nFresh, nDrops, nSkip, nExhausted)
}
