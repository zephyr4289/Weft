package dev.weft.sample

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import dev.weft.compose.rememberWeftHeddle
import dev.weft.compose.weftDraw

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
}

@Composable
fun WeftSampleScreen() {
    val heddle = rememberWeftHeddle(capacity = 512)

    Canvas(
        modifier = Modifier
            .fillMaxSize()
            .weftDraw(heddle) { buf ->
                val firstByte = buf.get(0).toInt() and 0xFF
                drawCircle(
                    color = Color(0xFF6200EE),
                    radius = (firstByte + 50).toFloat()
                )
            }
    ) {
    }
}
