// WeftGovernedDraw.kt — Series 7: the Jetpack Compose DrawScope adapter
// for GovernedFanoutConsumer (RFC-0009's composed display consumer).
//
// WHY EXISTS: the work order's "render-thread deep integration" — Compose's
// draw phase is where the governed consumer belongs (WHITEPAPER §8.1's
// deferred-read discipline): ONE tick per draw pass (claim -> ladder ->
// policy -> raster, all zero-allocation — the consumer's contract), and
// the DrawScope lambda runs ONLY on present ticks (the policy's elided
// ticks draw nothing — the previous raster stays on screen; that IS the
// steady-cadence contract, LATEST_WINS/PACED/BURST decided).
//
// THE LADDER STAYS ADVISORY: [onDraw] receives the action record too —
// the app responds to CLASS edges (Skip: skip decorative work; Snapshot:
// re-sync; Reseed: rebuild the consumer) without this modifier ever
// branching on telemetry (AXIOM T). [consumer.actionChanged] carries the
// class-change edge.
//
// IMPPELLER/AGSL-FRIENDLY: the raster is packed u32 words, little-endian
// (the wire contract) — feed it to a RuntimeShader uniform buffer or read
// ARGB channels directly; no wrapper allocations per frame.

package dev.weft.compose

import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.drawscope.DrawScope
import dev.weft.GovernedFanoutConsumer
import dev.weft.GovernorAction
import dev.weft.PresentDecision

/**
 * Modifier that drives a [GovernedFanoutConsumer] from Compose's draw
 * phase: one `tick()` per draw pass; [onDraw] runs only when the cadence
 * policy PRESENTS (elided ticks leave the previous raster — decided
 * coalescing, Law 4).
 *
 * Reads occur strictly inside the draw lambda (no recomposition, no
 * re-layout). The consumer is single-threaded by contract — the draw
 * thread owns it.
 *
 * @param consumer the composed display consumer (reader + governor +
 *   policy + two-frame history over the Series-7 recyclers).
 * @param onDraw receives the raster (packed u32 LE words, stable array
 *   identity — do not retain), the presentation decision, and the
 *   staleness-ladder action (advisory class).
 */
public fun Modifier.weftGovernedDraw(
    consumer: GovernedFanoutConsumer,
    onDraw: DrawScope.(raster: ByteArray, decision: PresentDecision, action: GovernorAction) -> Unit
): Modifier = this.drawWithContent {
    val d = consumer.tick() // claim + ladder + policy + raster — zero alloc
    if (d.present) {
        onDraw(consumer.raster, d, consumer.action)
    }
    drawContent()
}
