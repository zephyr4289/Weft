package dev.weft.sample

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.weft.Steward
import dev.weft.compose.ReattachPolicy
import dev.weft.compose.rememberWeftHeddle
import dev.weft.compose.weftDraw
import kotlinx.coroutines.isActive

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    WeftSampleScreen()
                }
            }
        }
    }

    // SERIES 7 — the memory-pressure backstop wiring (RFC-0009 work order):
    // ONE registration point forwards Android's trim/low-memory events to
    // every Weft recycler (FREE slots drop; LIVE slots are never touched —
    // a raster mid-blend cannot lose its buffer; the next acquire lazily
    // reallocates and counts it, per AXIOM T).
    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        dev.weft.WeftRecyclerCenter.onTrimMemory(level)
    }

    override fun onLowMemory() {
        super.onLowMemory()
        dev.weft.WeftRecyclerCenter.onLowMemory()
    }
}

@Composable
fun WeftSampleScreen() {
    // ViewModel-scoped Steward: the channel survives configuration change
    // (WHITEPAPER §7.3). REVOKE_AND_RENEW gives the demo a clean teardown.
    val steward: Steward = viewModel()
    val heddle = rememberWeftHeddle(capacity = 512, policy = ReattachPolicy.REVOKE_AND_RENEW, steward = steward)

    // Producer side: a data-driven writer paced to Choreographer ticks.
    // In production this loop lives on a native engine or worker thread; the
    // Triad Protocol makes the draw thread's claim() wait-free regardless of
    // this producer's cadence (RFC-0001 I2/I5).
    LaunchedEffect(heddle) {
        var seq = 0
        while (isActive) {
            val buf = heddle.weft.wBegin()
            buf.put(0, ((seq * 3) and 0xFF).toByte())   // payload byte 0: radius driver
            buf.put(1, ((seq * 7) and 0xFF).toByte())   // payload byte 1: hue driver
            heddle.weft.publish(seq = ++seq, payloadLen = 2)
            withFrameNanos { it } // vsync-paced producer
        }
    }

    Canvas(
        modifier = Modifier
            .fillMaxSize()
            .weftDraw(heddle) { buf ->
                // buf is the READER-HELD payload (rLiveBuf slice): absolute
                // index 0 = payload byte 0. Zero recomposition per frame.
                val radiusByte = buf.get(0).toInt() and 0xFF
                val hueByte = buf.get(1).toInt() and 0xFF
                drawCircle(
                    color = Color(hueByte / 255f, 0.4f, 0.9f),
                    radius = (radiusByte * 1.5f + 50f)
                )
            }
    ) {
    }
}
