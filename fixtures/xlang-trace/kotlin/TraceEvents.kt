// TraceEvents.kt — RFC 0014 kernel-trace emitter (issue #20 task 1), Kotlin side.
//
// Runs the deterministic scenario shared with the C reference
// (core/c/trace_dump.c scenario_run) and the TS emitter
// (fixtures/xlang-trace/trace_emitter.mjs) and prints the packed event
// stream (8 bytes/event: u16 kind | u16 aux | u32 data) as lowercase hex.
// run.sh byte-compares all ports — any divergence in the event order, the
// epoch plumbing, or the slot rotation is a hard failure.
//
// Toolchain policy (the repo's per-port honesty pattern): runs only when
// kotlinc is present; CI owns the leg (android-packages workflow).
//
// Build & run (no dependencies beyond the kernel port):
//   kotlinc fixtures/xlang-trace/kotlin/TraceEvents.kt core/kotlin/Weft.kt \
//     -include-runtime -d /tmp/trace.jar && java -jar /tmp/trace.jar 2000 0x00C0FFEE

package dev.weft

import java.nio.ByteBuffer
import java.nio.ByteOrder

val xsSeed = java.util.concurrent.atomic.AtomicInteger(0)

fun xorshift32(): Long {
    var x = xsSeed.get()
    if (x == 0) x = 0x9E3779B9.toInt()
    x = x xor (x shl 13)
    x = x xor (x ushr 17)
    x = x xor (x shl 5)
    xsSeed.set(x)
    return x.toLong() and 0xFFFFFFFFL
}

fun packEvent(kind: Int, aux: Int, data: Int): ByteArray {
    val b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
    b.putShort(kind.toShort())
    b.putShort(aux.toShort())
    b.putInt(data)
    return b.array()
}

fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it) }

fun main(args: Array<String>) {
    val n = if (args.isNotEmpty()) args[0].toInt() else 2000
    val seedRaw = if (args.size > 1) args[1] else "0x00C0FFEE"
    val seed = if (seedRaw.startsWith("0x")) seedRaw.removePrefix("0x").toLong(16).toInt() else seedRaw.toLong().toInt()
    val out = StringBuilder()

    val w = Weft(64)
    xsSeed.set(seed)
    var seq = 0
    val revokeStep = n / 2

    for (step in 0 until n) {
        val plen = (xorshift32() % 65L).toInt()
        seq += 1
        val cursor = w.wBegin()
        for (i in 0 until plen) cursor.put(i, pat(seq, i))
        val r = w.publish(seq, plen)
        if (r == PubResult.DROPPED_REVOKED) {
            val ep = w.epochVal()
            out.append(hex(packEvent(3, 0, seq)))   // DROP
            out.append(hex(packEvent(5, 0, ep)))    // ACK
        } else {
            out.append(hex(packEvent(1, plen, seq))) // PUBLISH
        }
        if (xorshift32() % 3L == 0L) {
            w.claim()
            out.append(hex(packEvent(2, 0, w.rSeq()))) // CLAIM
            val canary = w.rCanary()
            check(canary == (w.rSeq().toLong() and 0xFFFFFFFFL)) { "canary check FAILED at step $step" }
        }
        if (step == revokeStep) {
            val e0 = w.epochVal()
            w.revoke()
            out.append(hex(packEvent(4, 0, e0))) // REVOKE
        }
    }
    println(out.toString())
}
