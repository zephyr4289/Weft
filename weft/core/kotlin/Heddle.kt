// Heddle.kt — Draw-phase binding for Jetpack Compose (Kotlin)
//
// WHY EXISTS: Binds a Weft to the Draw phase via Modifier.drawWithContent.
// Per WHITEPAPER §8.1: the Compose draw-phase reference. Per 02-KERNEL §4.2:
// reads a Weft during the Draw phase only, bypassing composition and layout
// invalidation. Per Q1/Q3 open questions (WHITEPAPER §10): Compose Multiplatform
// graphicsLayer deferred-read support is unverified.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

package dev.weft

import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.DrawScope
import java.nio.ByteBuffer

/// Bind a Weft to a Compose draw scope. The lambda runs on every VSYNC,
/// reading the latest frame from the Weft. Zero recomposition.
fun Modifier.weftDraw(
    weft: Weft,
    onDraw: DrawScope.(ByteBuffer) -> Unit
): Modifier = this.drawWithContent {
    if (weft.claim().let { true }) {
        val buf = weft.wBegin() // Returns the writer's working buffer view
        onDraw(buf)
    }
    drawContent()
}

/// Remember a Steward scoped to the current composition.
/// Per WHITEPAPER §7.3: for most use cases, scope the Steward to a ViewModel.
@Composable
fun rememberSteward(): Steward {
    return androidx.compose.runtime.remember { Steward() }
}
