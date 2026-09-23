// TrendTrace.kt — RFC 0020 cross-language verdict-stream emitter, Kotlin side.
//
// Drives the trend estimator with the deterministic behind trace
// (behind = xorshift32(state) % 64, seed 0x00C0FFEE) and prints the packed
// verdict stream (verdict << 6 | min(skip_n, 63), hex, one line) —
// byte-identical to core/c/trend_runner.c (the C reference), trend_emitter.mjs
// (TS), and the Swift/Dart VM emitters beside this one.
// fixtures/xlang-trend/run.sh byte-compares them all.
//
// Pinned parity vector (from the C reference — do not "fix" it): the
// 2000-sample hex stream hashes (FNV-1a over the stream) to
// 0x11187b9a02b378ef.
//
// Build & run (no Gradle — the fixture is a two-file kotlinc invocation):
//   kotlinc core/kotlin/WeftTrend.kt fixtures/xlang-trend/vm/kotlin/TrendTrace.kt \
//           -include-runtime -d /tmp/trend-trace-kotlin.jar
//   java -jar /tmp/trend-trace-kotlin.jar [STEPS [SEED]]

package dev.weft.vmtrace

import dev.weft.TrendOut
import dev.weft.WeftTrend
import dev.weft.weftTrendInit
import dev.weft.weftTrendObserve
import dev.weft.weftTrendPack

private fun xorshift32(x0: Long): Long {
    var x = x0 and 0xffffffffL
    x = x xor ((x shl 13) and 0xffffffffL)
    x = x xor (x ushr 17)
    x = x xor ((x shl 5) and 0xffffffffL)
    return x and 0xffffffffL
}

fun main(args: Array<String>) {
    val steps = if (args.isNotEmpty()) args[0].toLong() else 10_000L
    val seed = if (args.size > 1) {
        if (args[1].startsWith("0x")) args[1].substring(2).toLong(16) else args[1].toLong()
    } else 0x00C0FFEEL

    val t = WeftTrend()
    weftTrendInit(t)
    val out = TrendOut()
    val sb = StringBuilder((steps * 2).toInt())
    var state = seed
    for (i in 0 until steps) {
        state = xorshift32(state)
        val behind = (state % 64).toInt() // state is a u32 value (non-negative)
        weftTrendObserve(t, behind, out)
        val packed = weftTrendPack(out)
        sb.append(packed.toString(16).padStart(2, '0'))
    }
    println(sb.toString())
}
