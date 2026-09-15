// WeftModifiers.kt — Compose draw-phase and graphicsLayer deferred read modifiers
//
// WHY EXISTS: Defers reading the Weft channel to the Compose Draw phase
// (bypassing composition and layout passes completely) per WHITEPAPER §8.1.
//
// AGSL / RENDER EFFECT INTEGRATION:
// Android 13+ (API 33+) allows setting RenderEffect via Modifier.graphicsLayer.
// Custom AGSL shaders consume the Weft channel's live direct buffer in the
// GPU render pipeline without copying to CPU heap.

package dev.weft.compose

import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.GraphicsLayerScope
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.graphicsLayer
import dev.weft.Weft
import java.nio.ByteBuffer

/**
 * Modifier that binds a Weft channel to Compose draw-phase execution.
 *
 * Reads occur strictly on VSYNC ticks inside [onDraw], avoiding recomposition.
 */
public fun Modifier.weftDraw(
    heddle: WeftHeddle,
    onDraw: DrawScope.(ByteBuffer) -> Unit
): Modifier = this.drawWithContent {
    val weft = heddle.weft
    weft.claim()
    val buf = weft.wBegin()
    onDraw(buf)
    drawContent()
}

/**
 * Modifier that configures a graphicsLayer with deferred draw-phase reads.
 *
 * Can be used with Android 13+ AGSL RuntimeShader / RenderEffect hooks for zero-copy
 * GPU shader parameters.
 */
public fun Modifier.weftGraphicsLayer(
    heddle: WeftHeddle,
    block: GraphicsLayerScope.(Weft) -> Unit
): Modifier = this.graphicsLayer {
    block(heddle.weft)
}
