// GovernedFanoutConsumer.kt — RFC-0009 Series 7: the composed display
// consumer (fan-out reader + FreshnessGovernor + CadencePolicy + two-frame
// history over the Series-7 buffer recyclers), Kotlin driver layer.
//
// WHY EXISTS: RFC-0009's two open questions both resolved "composed" — the
// governor never touches a Triad or a ring; the reader's per-consumer
// staleness is the input. This class IS that composition, once per display
// tick, so the app does not hand-roll it per screen:
//
//   ticker ──tick──> CONSUMER ──┬─ claim()          (fan-out reader, zero alloc)
//                               ├─ governor.step()  (staleness CLASS: FastPath /
//                               │                    Skip / Snapshot / Reseed —
//                               │                    advisory; the app decides
//                               │                    what a class MEANS)
//                               ├─ policy.step()    (PRESENTATION: present? interp?
//                               │                    alphaQ12 — the raster decision)
//                               └─ raster           (blend(prev, new, alphaQ12)
//                                                    into a pooled slot — LATEST/
//                                                    BURST copy newest directly)
//
// ZERO-GC PER TICK (Law 2, the R8 audit's subject): claim mutates the
// reader's record; governor/policy mutate identity-stable records; the
// two-frame history is two preallocated IntArrays; the raster is a pooled
// slot from WeftBufferRecycler (LIVE for the consumer's lifetime — the
// memory-pressure backstop can never take it mid-blend; dispose() returns
// it to the pool). The Kotlin battery audits the whole tick at ZERO bytes
// (RecyclerTest R8's twin discipline, pinned per consumer in
// GovernedFanoutConsumerTest).
//
// PACED CONTINUITY (the RFC's construction): on a fresh claim the window
// advances — prevWords := newWords, newWords := reader.view() — and the
// blend at alpha=0 equals prev (the completed previous blend): the raster
// is continuous by construction, never extrapolated past the newest frame
// (saturated alpha holds).
//
// THE GOVERNOR STAYS ADVISORY: tick() returns the policy decision (what
// the display loop needs every frame); the ladder's action is exposed via
// [action] + [actionChanged] for the app's CLASS response (skip decorative
// work on Skip, re-snapshot on Snapshot, rebuild on Reseed). Nothing in
// this class branches on a telemetry counter (AXIOM T).
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION (JVM battery green;
// android-packages gradle CI is the cover).

package dev.weft

/**
 * One governed display consumer. Single-threaded by contract (the draw
 * thread — the reader's discipline); the reader is borrowed, never owned.
 *
 * @param reader the fan-out reader (RFC-0004) — claim() once per tick.
 * @param policyKind a [CadencePolicyKind] (PROTOCOL value).
 * @param rasterPool the recycler the raster slot comes from (default: a
 *   private pool of one — LIVE slots survive trims by construction).
 * @param governorConfig the staleness ladder (RFC-0009 published defaults).
 * @param reassessTicks BURST_COALESCE hysteresis window.
 */
class GovernedFanoutConsumer(
    /** Borrowed fan-out reader; claim() is called once per tick(). */
    val reader: WeftFanoutReader,
    /** A [CadencePolicyKind] PROTOCOL value. */
    val policyKind: Int,
    rasterPool: WeftBufferRecycler? = null,
    governorConfig: GovernorConfig = GOVERNOR_DEFAULTS,
    reassessTicks: Int = 8,
) {
    private val governor = FreshnessGovernor(governorConfig)
    private val policy = CadencePolicy(CadenceConfig(policyKind, reassessTicks))

    /** Words per frame (payloadBytes / 4). */
    val words: Int = reader.payloadBytes / 4

    /** The recycler the raster slot came from (null = privately owned). */
    private val myPool: WeftBufferRecycler?

    /** The raster slot (pooled; LIVE for this consumer's lifetime). */
    val raster: ByteArray

    /** Dispose gate (null after dispose — raster stays the stable ref). */
    private var rasterOrNull: ByteArray? = null

    // --- two-frame history (preallocated; zero alloc per tick) ---
    private val prevWords = IntArray(words)
    private val newWords = IntArray(words)

    // --- advisory state (AXIOM T) ---
    /** The ladder's latest action (identity-stable record). */
    val action: GovernorAction get() = governor.act
    /** True when this tick's ladder action differs from the last (class
     *  change edge — the app's hook for class responses). */
    var actionChanged: Boolean = false
        private set
    private var lastActionKind = GovernorActionKind.FAST_PATH

    /** The policy's counters (presents, coalescedByDecision, ...). */
    val cadence: CadencePolicy get() = policy
    /** The ladder's counters (decidedDrops, reseeds, ...). */
    val staleness: FreshnessGovernor get() = governor

    init {
        val bytes = reader.payloadBytes
        val slot = rasterPool?.acquire()?.also {
            require(it.size >= bytes) {
                "raster pool slot too small: ${it.size} < $bytes"
            }
        }
        raster = slot ?: ByteArray(bytes)
        myPool = if (slot != null) rasterPool else null
        rasterOrNull = raster
    }

    /**
     * One display tick: claim -> ladder -> policy -> raster. Returns the
     * presentation decision (identity-stable — read synchronously).
     * Zero allocation.
     */
    fun tick(): PresentDecision {
        val rec = reader.claim() // zero alloc; mutates the reader's record
        // The ladder: staleness CLASS from this reader's drop accounting
        // (rec.dropped == the frames this consumer missed since its last
        // fresh claim — RFC-0008's per-consumer framesBehind).
        val a = governor.step(if (rec.fresh) rec.dropped else 0L, nowMs())
        actionChanged = a.kind != lastActionKind
        lastActionKind = a.kind
        // The window: on a fresh claim, prev := new, new := view.
        if (rec.fresh) {
            System.arraycopy(newWords, 0, prevWords, 0, words)
            val v = reader.view()
            for (i in 0 until words) newWords[i] = v[i]
        }
        // The presentation decision.
        val d = policy.step(rec.seq)
        // The raster: blend at alpha (PACED) or copy newest (LATEST/BURST).
        if (d.present) {
            if (d.interp) {
                val alpha = d.alphaQ12
                val inv = CADENCE_ALPHA_ONE_Q12 - alpha
                for (i in 0 until words) {
                    // BlendQ12 = the single spec'd raster op (Series 8;
                    // bit-exact to the C SIMD kernel via the xlang gate).
                    val packed = BlendQ12.blendPacked(prevWords[i], newWords[i], alpha)
                    raster[4 * i] = packed.toByte()
                    raster[4 * i + 1] = (packed ushr 8).toByte()
                    raster[4 * i + 2] = (packed ushr 16).toByte()
                    raster[4 * i + 3] = (packed ushr 24).toByte()
                }
            } else {
                for (i in 0 until words) {
                    val packed = newWords[i]
                    raster[4 * i] = packed.toByte()
                    raster[4 * i + 1] = (packed ushr 8).toByte()
                    raster[4 * i + 2] = (packed ushr 16).toByte()
                    raster[4 * i + 3] = (packed ushr 24).toByte()
                }
            }
        }
        return d
    }

    /** The ladder's clock (milliseconds, monotonic-ish). Injectable for
     *  deterministic tests; wall-clock by default. */
    var clock: () -> Long = { System.currentTimeMillis() }

    private fun nowMs(): Long = clock()

    /** Stop the consumer. The raster slot returns to its pool (the next
     *  trim can reclaim it — the consumer is done drawing). Idempotent. */
    fun dispose() {
        val r = rasterOrNull ?: return
        rasterOrNull = null
        myPool?.release(r)
    }

}
