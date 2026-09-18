// CadenceTrace.kt — PC3 cadence-trace emitter (RFC-0009 §cadence), Kotlin side.
//
// Emits the packed decision log for the deterministic arrival trace shared
// with the TS/Swift/Dart emitters (fixtures/xlang-cadence/):
//
//   state = SEED (default 0x00C0FFEE)
//   for i in 0..N:
//     state = xorshift32(state); arrivals = state % 5; latest += arrivals
//     for policy in [LATEST_WINS, PACED_INTERPOLATE, BURST_COALESCE,
//                    PREDICTIVE_PACED]:
//       d = policy.step(latest)
//       emit byte1 = (present<<7) | (interp<<6) | (alphaQ12 >> 7)
//       emit byte2 = min(coalesced, 255)
//
// Hex-encoded (lowercase, no separators, one trailing newline) — 8 bytes
// per tick, four policies in kind order (PC3 v2, RFC-0012 added kind 3).
// run.sh byte-compares all ports.
//
// Build & run:
//   kotlinc core/kotlin/Governor.kt fixtures/xlang-cadence/kotlin/CadenceTrace.kt \
//           -include-runtime -d /tmp/cad-trace-kotlin.jar
//   java -jar /tmp/cad-trace-kotlin.jar [STEPS [SEED]]

package dev.weft.vmtrace

import dev.weft.CadencePolicy
import dev.weft.CadencePolicyKind

private fun xorshift32(x0: Long): Long {
    var x = x0 and 0xffffffffL
    x = x xor ((x shl 13) and 0xffffffffL)
    x = x xor (x ushr 17)
    x = x xor ((x shl 5) and 0xffffffffL)
    return x and 0xffffffffL
}

fun main(args: Array<String>) {
    val steps = if (args.isNotEmpty()) args[0].toInt() else 10_000
    val seed = if (args.size > 1) {
        if (args[1].startsWith("0x")) args[1].substring(2).toLong(16) else args[1].toLong()
    } else 0x00C0FFEEL

    val pols = arrayOf(
        CadencePolicy(CadencePolicyKind.LATEST_WINS),
        CadencePolicy(CadencePolicyKind.PACED_INTERPOLATE),
        CadencePolicy(CadencePolicyKind.BURST_COALESCE),
        CadencePolicy(CadencePolicyKind.PREDICTIVE_PACED)
    )
    val sb = StringBuilder(steps * 6 * 2)
    var state = seed
    var latest = 0L
    for (i in 0 until steps) {
        state = xorshift32(state)
        latest += state % 5
        for (p in pols) {
            val a = p.step(latest)
            val b1 = (((if (a.present) 1 else 0) shl 7) or
                ((if (a.interp) 1 else 0) shl 6) or
                (a.alphaQ12 shr 7)) and 0xff
            val b2 = (minOf(a.coalesced, 255L)).toInt() and 0xff
            sb.append(b1.toString(16).padStart(2, '0'))
            sb.append(b2.toString(16).padStart(2, '0'))
        }
    }
    println(sb.toString())
}
