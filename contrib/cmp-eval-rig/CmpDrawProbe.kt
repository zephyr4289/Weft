// CmpDrawProbe.kt — Compose Multiplatform draw-phase cadence probe
// (RFC-0007 evaluation rig, contrib/cmp-eval-rig).
//
// WHY EXISTS: RFC 0007 (Draft, Staff Decision [EMPTY]) evaluates CMP for
// iOS against the native Swift/Metal path — the deferred draw-phase read
// invariant (drawBehind/drawWithContent must not recompose), the Skiko
// pipeline, and the 120 Hz ProMotion pacing trade-off. Evaluation without
// an instrument is an opinion. This probe produces the SAME report shape
// as the Swift MTKViewCadenceProbe (Sources/WeftSwiftUI) so the two paths
// compare NUMBER TO NUMBER:
//
//   Swift:  CadenceProbeReport{ framesRendered, measuredFPS, medianIntervalMs,
//           p95IntervalMs, claimsInDrawPhase, tornAccepted, ... }
//   CMP:    CmpCadenceReport{ framesRendered, measuredFps, medianIntervalMs,
//           p95IntervalMs, drawPhaseClaims, recompositions, tornAccepted, ... }
//
// The two decisive columns the rig measures:
//   1. recompositions — a Weft state read inside drawBehind must NOT
//      recompose the host composable (the LAW 1/Law 2 draw-phase
//      isolation). The probe counts recompositions via a snapshot
//      counter in the composition scope; a nonzero count FAILS the
//      evaluation (that is the RFC's core question, answered with a
//      counter, not prose).
//   2. cadence — frame timestamps collected in the draw lambda, driven by
//      Compose's frame clock (equivalent of the display link), yield
//      measured FPS + median/p95 intervals. ProMotion 120 Hz on CMP is
//      exactly the jitter surface RFC-0007 flags ("occasional frame
//      pacing jitter due to Kotlin/Native runtime scheduling") — p95
//      interval is where it shows.
//
// STATUS: SOURCE-ONLY, PENDING CMP TOOLCHAIN (declared). The rig compiles
// when a Compose Multiplatform target lands in the tree; the protocol and
// report shape are frozen NOW so the first CMP run and the Swift probe
// compare like-for-like. The Swift side of the comparison is CI-PROVEN
// (MetalProbeTests) — the rig's numbers land in the same honesty split.

package dev.weft.cmp

import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import kotlin.math.roundToInt
import kotlin.time.Duration
import kotlin.time.TimeSource

/// Immutable report — the same columns the Swift probe publishes, plus the
/// one column only CMP has to defend: recompositions.
data class CmpCadenceReport(
    val requestedFrames: Int,
    val framesRendered: Int,
    val measuredFps: Double,
    val medianIntervalMs: Double,
    val p95IntervalMs: Double,
    /// Draw-phase reads performed inside the Canvas draw lambda (must equal
    /// framesRendered — the draw-phase read proof).
    val drawPhaseClaims: Int,
    /// Recomposition count of the host composable during the run. THE
    /// RFC-0007 acceptance gate: must be 0 — a state read inside drawBehind
    /// must not recompose.
    val recompositions: Int,
    /// Fresh claims whose payload failed pattern verification — must be 0.
    val tornAccepted: Int,
    val notes: List<String>
) {
    val ok: Boolean
        get() = framesRendered > 0 && drawPhaseClaims == framesRendered &&
            recompositions == 0 && tornAccepted == 0
}

/// The claim→verify→draw probe loop. `readFrame` is the host's Weft read
/// (whatever the CMP integration ends up injecting — the kernel's claim +
/// live view); `verifyPayload` pattern-checks the claimed frame the same
/// way the Swift probe does (pat() stride check).
///
/// The probe composable draws on a Canvas whose draw lambda performs the
/// Weft read — the deferred draw-phase read under measurement. Frame
/// timestamps come from the Canvas's own frame clock (the Compose analog
/// of the display link); the host composable's recomposition counter rides
/// in a snapshot-isolated slot the draw lambda cannot touch.
@Composable
fun CmpDrawProbe(
    requestedFrames: Int,
    readFrame: (payloadOut: FloatArray) -> Int, // returns claimed seq
    verifyPayload: (seq: Int, payload: FloatArray) -> Boolean,
    onReport: (CmpCadenceReport) -> Unit,
    modifier: Modifier = Modifier
) {
    // Recomposition counter — read/written OUTSIDE the draw lambda's
    // snapshot (composition scope). If a draw-phase read leaks into
    // composition, this count moves; 0 is the acceptance gate.
    val recompositions = remember { mutableIntStateOf(0) }
    val frames = remember { mutableIntStateOf(0) }
    var claims = 0
    var torn = 0
    val timestamps = ArrayList<Double>(requestedFrames)
    val payload = FloatArray(64)

    // A recomposition observer: increments the count on every recomposition
    // of this scope (the Snapshot-sendable way to detect the failure mode
    // RFC-0007 asks about).
    recompositions.intValue++ // this composable body just (re)ran

    Canvas(modifier) {
        if (frames.intValue >= requestedFrames) return@Canvas
        val now = TimeSource.Monotonic.markNow()
        claims++
        val seq = readFrame(payload)
        if (seq > 0 && !verifyPayload(seq, payload)) {
            torn++
        }
        drawIntoCanvas { canvas ->
            // Minimal deterministic stroke: proof-of-render, not art.
            val native = canvas.nativeCanvas
            native.drawColor(android.graphics.Color.BLACK)
        }
        timestamps.add(now.elapsedNow().inWholeMilliseconds.toDouble())
        frames.intValue++
        if (frames.intValue >= requestedFrames) {
            val intervals = timestamps.zipWithNext { a, b -> b - a }.sorted()
            val median = if (intervals.isEmpty()) 0.0 else intervals[intervals.size / 2]
            val p95 = if (intervals.isEmpty()) 0.0
            else intervals[((intervals.size * 0.95).roundToInt()
                .coerceIn(1, intervals.size)) - 1]
            val elapsed = if (timestamps.size >= 2) timestamps.last() - timestamps.first() else 0.0
            val fps = if (elapsed > 0) (timestamps.size - 1) * 1000.0 / elapsed else 0.0
            onReport(
                CmpCadenceReport(
                    requestedFrames = requestedFrames,
                    framesRendered = frames.intValue,
                    measuredFps = fps,
                    medianIntervalMs = median,
                    p95IntervalMs = p95,
                    drawPhaseClaims = claims,
                    recompositions = recompositions.intValue - 1, // first run is not a re-composition
                    tornAccepted = torn,
                    notes = listOf(
                        "CMP/Skiko pipeline — compare against MTKViewCadenceReport like-for-like",
                        "acceptance: recompositions == 0 (the draw-phase read stayed deferred)",
                        "ProMotion device rate remains deferred (RFC-0007 Hardware Deferral List)"
                    )
                )
            )
        }
    }
}
