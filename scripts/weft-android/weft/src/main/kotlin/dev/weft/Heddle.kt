package dev.weft

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.DrawScope.Companion
import java.nio.ByteBuffer

/**
 * The Heddle: Draw-phase binding for a [Weft].
 *
 * Reads a Weft on every VSYNC via `Modifier.drawWithContent`, bypassing
 * composition and layout invalidation (see spec §4.2).
 *
 * ## Usage
 *
 * ```kotlin
 * Canvas(modifier = Modifier
 *     .fillMaxSize()
 *     .weftDraw(pcm) { drawScope, buf ->
 *         for (i in 0 until buf.capacity() / 4) {
 *             val v = buf.getFloat(i * 4)
 *             drawBar(drawScope, i, v)
 *         }
 *     }
 * ) { /* static layout */ }
 * ```
 *
 * ## Zero allocation
 *
 * The [DrawScope].(buf) lambda runs on every VSYNC. It MUST NOT allocate
 * objects. The [Weft.readBuffer] is pre-allocated and reused; the lambda
 * reads from it directly.
 *
 * @spec v0.1 §4.2, §8.1
 */

/**
 * Bind a [Weft] to a Compose draw scope. The lambda runs on every VSYNC,
 * reading the latest frame from the Weft.
 *
 * @param weft The Weft to read.
 * @param onDraw The Draw-phase callback. Receives the [DrawScope] and the
 *        pre-allocated read buffer (a direct [ByteBuffer] of size
 *        `weft.byteSize`). The lambda must not allocate.
 */
fun Modifier.weftDraw(
    weft: Weft<*>,
    onDraw: DrawScope.(ByteBuffer) -> Unit,
): Modifier = this.drawWithContent {
    // Read the latest frame into the pre-allocated readBuffer.
    // Zero allocation: readBuffer is reused.
    if (weft.read()) {
        onDraw(weft.readBuffer)
    }
    // Always draw the content below the weft overlay (e.g. the static
    // Canvas background).
    drawContent()
}

/**
 * Remember a Steward scoped to the current composition.
 *
 * For most use cases, scope the Steward to a `ViewModel` instead —
 * ViewModel survives configuration change. This helper is for cases where
 * the Steward's lifetime should match the composition.
 *
 * @spec v0.1 §7.3
 */
@Composable
fun rememberSteward(): Steward {
    return remember { Steward() }
}
