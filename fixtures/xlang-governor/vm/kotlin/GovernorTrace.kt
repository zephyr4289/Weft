// GovernorTrace.kt — G5 ladder-trace emitter (RFC-0009), Kotlin side.
//
// Emits the packed action log for the deterministic (behind, now_ms) trace
// shared with fixtures/xlang-governor/gov_trace.mjs (TS),
// core/c/governor-test xlang-dump (C), core/rust governor_xlang (Rust),
// and the Swift/Dart VM emitters beside this one:
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N: state = xorshift32(state);
//                  behind = state % 128; now_ms = i;
//                  action = governor.step(behind, now_ms)
//                  emit byte (kind << 6) | min(skip_n, 63)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — the exact
// format every other emitter produces. run.sh byte-compares them all.
//
// Build & run (no Gradle — the fixture is a two-file kotlinc invocation):
//   kotlinc core/kotlin/Governor.kt fixtures/xlang-governor/vm/kotlin/GovernorTrace.kt \
//           -include-runtime -d /tmp/gov-trace-kotlin.jar
//   java -jar /tmp/gov-trace-kotlin.jar [STEPS [SEED]]

package dev.weft.vmtrace

import dev.weft.FreshnessGovernor

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

    val gov = FreshnessGovernor()
    val sb = StringBuilder(((steps * 2).toInt()))
    var state = seed
    for (i in 0 until steps) {
        state = xorshift32(state)
        val behind = state % 128
        val a = gov.step(behind, i)
        val packed = ((a.kind shl 6) or minOf(a.skipN, 63)) and 0xff
        sb.append(packed.toString(16).padStart(2, '0'))
    }
    println(sb.toString())
}
