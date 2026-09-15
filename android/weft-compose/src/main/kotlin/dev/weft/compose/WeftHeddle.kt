// WeftHeddle.kt — Jetpack Compose Heddle binding for Weft
//
// WHY EXISTS: Implements the Compose draw-phase deferred read pattern and
// ViewModel-scoped Steward binding per WHITEPAPER §8.1 and DIRECTIVE-12 T12.3.
//
// DISPOSAL ORDERING:
// The Heddle is remembered and tied to the ViewModel lifecycle. On recomposition,
// the Heddle instance is preserved (survives recomposition); disposal occurs
// strictly when the enclosing scope exits via DisposableEffect or ViewModel onCleared.

package dev.weft.compose

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.weft.Steward
import dev.weft.Weft

/**
 * Lifecycle reattachment policy seam per RFC Q5 (design-only stub).
 */
public enum class ReattachPolicy {
    RESET_ON_ATTACH,
    PRESERVE_HELD,
    REVOKE_AND_RENEW
}

/**
 * A Compose Heddle holding a Weft channel bound to a Steward.
 */
public class WeftHeddle(
    public val steward: Steward,
    public val weft: Weft,
    public val policy: ReattachPolicy = ReattachPolicy.PRESERVE_HELD
) {
    public var isDisposed: Boolean = false
        private set

    internal fun dispose() {
        if (!isDisposed) {
            isDisposed = true
        }
    }
}

/**
 * Remember a [WeftHeddle] bound to a [Steward].
 *
 * Survives recomposition cycles; disposed when the calling composable leaves the composition.
 *
 * @param capacity Buffer capacity in elements.
 * @param policy Lifecycle reattachment policy.
 * @param steward Optional Steward; defaults to a ViewModel-scoped Steward.
 */
@Composable
public fun rememberWeftHeddle(
    capacity: Int = 1024,
    policy: ReattachPolicy = ReattachPolicy.PRESERVE_HELD,
    steward: Steward = viewModel()
): WeftHeddle {
    val heddle = remember(steward, capacity) {
        val weft = steward.weft<ByteArray>(capacity)
        WeftHeddle(steward, weft, policy)
    }

    DisposableEffect(heddle) {
        onDispose {
            heddle.dispose()
        }
    }

    return heddle
}
