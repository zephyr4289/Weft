// Heddle.kt — Draw-phase binding for Jetpack Compose (Kotlin)
//
// WHY EXISTS: Binds a Weft to the Draw phase via Modifier.drawWithContent.
// Per WHITEPAPER §8.1: the Compose draw-phase reference. Per 02-KERNEL §4.2:
// reads a Weft during the Draw phase only, bypassing composition and layout
// invalidation. Per Q1/Q3 open questions (WHITEPAPER §10): Compose Multiplatform
// graphicsLayer deferred-read support is unverified.
//
// DRAW-PHASE READ RULE (RFC-0001 §4.3): after claim(), the freshest complete
// frame lives in the READER-HELD buffer (r_work). Draw code MUST read it via
// weft.rLiveBuf(). Reading weft.wBegin() here is a protocol violation: that is
// the writer's scratch buffer, concurrently being filled on the producer
// thread — a torn read by construction, the exact tearing the Triad Protocol
// exists to make impossible.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

package dev.weft

import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.DrawScope
import java.nio.ByteBuffer

/// Bind a Weft to a Compose draw scope. The lambda runs on every VSYNC,
/// claiming the latest frame and exposing the reader-held payload as a
/// zero-copy, payload-relative view. Zero recomposition.
fun Modifier.weftDraw(
    weft: Weft,
    onDraw: DrawScope.(ByteBuffer) -> Unit
): Modifier = this.drawWithContent {
    weft.claim()
    onDraw(weft.rLiveBuf())
    drawContent()
}

/// Remember a Steward scoped to the current composition.
/// Per WHITEPAPER §7.3: for most use cases, scope the Steward to a ViewModel.
@Composable
fun rememberSteward(): Steward {
    return androidx.compose.runtime.remember { Steward() }
}
