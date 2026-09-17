// BlendVerify.kt — Kotlin verifier for the xlang-blend golden vectors.
//
// Replays the C kernel's deterministic buffer construction (blend_test.c
// fill_random, seed 0x5EEDBEEF, two xorshift32 draws per word over
// words + 8 guard words), blends with dev.weft.BlendQ12.blendWords, and
// FNV-1a-64 digests the output — the digest MUST equal the C kernel's
// for every (size, alpha) row of golden-vectors.csv.
//
// Build & run:
//   kotlinc core/kotlin/BlendQ12.kt fixtures/xlang-blend/kotlin/BlendVerify.kt \
//           -include-runtime -d /tmp/blend-verify.jar
//   java -jar /tmp/blend-verify.jar [CSV]

package dev.weft.vmverify

import dev.weft.BlendQ12
import java.io.File

private fun xorshift32(x0: Long): Long {
    var x = x0 and 0xffffffffL
    x = x xor ((x shl 13) and 0xffffffffL)
    x = x xor (x ushr 17)
    x = x xor ((x shl 5) and 0xffffffffL)
    return x and 0xffffffffL
}

private fun fnv1a64(words: IntArray, n: Int): Long {
    // 0xcbf29ce484222325 sets the sign bit — a ULong literal carries it
    // into Long (the repo's Kotlin idiom, cf. GovernorTest.fnv1a64).
    var h = 0xcbf29ce484222325uL.toLong()
    for (i in 0 until n) {
        var w = words[i]
        for (shift in intArrayOf(0, 8, 16, 24)) {
            h = h xor ((w ushr shift) and 0xff).toLong()
            h *= 0x100000001b3L
        }
    }
    return h
}

fun main(args: Array<String>) {
    val csv = if (args.isNotEmpty()) args[0] else "golden-vectors.csv"
    val lines = File(csv).readLines().filter { it.matches(Regex("""^[0-9]+,.*""")) }
    var pass = 0
    var fail = 0
    for (line in lines) {
        val parts = line.trim().split(",")
        val words = parts[0].toInt()
        val alpha = parts[1].toInt()
        val want = parts[2]
        var state = 0x5EEDBEEFL
        val prev = IntArray(words + 8)
        val newest = IntArray(words + 8)
        for (i in 0 until words + 8) {
            state = xorshift32(state); prev[i] = state.toInt()
            state = xorshift32(state); newest[i] = state.toInt()
        }
        val out = IntArray(words + 8)
        BlendQ12.blendWords(prev, newest, alpha, out)
        val got = java.lang.Long.toHexString(fnv1a64(out, words)).padStart(16, '0')
        if (got == want) pass++ else {
            fail++
            System.err.println("MISMATCH size=$words alpha=$alpha: want $want got $got")
        }
    }
    println("xlang-blend Kotlin verifier: $pass/${pass + fail} golden digests match")
    if (fail > 0) kotlin.system.exitProcess(1)
}
