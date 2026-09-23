// ReplayTrace.kt — RFC 0019 time-travel replay hash-log emitter, Kotlin side.
//
// Folds the deterministic RFC-0019 fixture scenario (the normative grammar
// below, xorshift32-seeded — the repo's canonical 04-LITMUS §0.2 generator)
// and prints the per-step u64 state hashes as one lowercase-hex line —
// byte-identical to core/c/replay_runner.c (the C reference), the Rust
// replay_xlang bin, replay_emitter.mjs (TS), and the Swift/Dart VM emitters
// beside this one. fixtures/xlang-replay/run.sh byte-compares them all.
//
// NORMATIVE SCENARIO GRAMMAR (mirror of replay_runner.c's scen_next —
// every emitter implements this EXACTLY):
//
//   state = SEED; latest=0 w_work=1 r_work=2 epoch=0 revoked=0 seq=0
//   bufseq = [0,0,0]                      # shadow seq per buffer slot
//   for i in 0..N:
//     state = xorshift32(state); op = state & 15; u = state (unsigned)
//     op < 7   : seq++; len = (u >> 4) % 1024
//                if !revoked: PUBLISH(aux=len, data=seq);
//                  bufseq[w_work] = seq; (latest,w_work) = (w_work,latest)
//                else: epoch++; DROP(aux=epoch & 0xffff, data=seq)
//     op < 12  : CLAIM(data=bufseq[latest]);
//                (latest,r_work) = (r_work,latest)
//     op == 12 : !revoked: REVOKE(data=epoch); revoked=1
//                else:     ACK(data=epoch)
//     op == 13 : revoked: ACK(data=epoch); revoked=0
//                else:    STALL(data=(u >> 4) % 8)
//     op == 14 : TEAR(data=seq)
//     else     : CANARY_FAIL(data=seq)
//
// The scenario mirror keeps its own shadow (same exchange rules as the
// fold) so CLAIM events carry the seq the model will reconstruct — the
// fold must therefore NEVER disagree; a disagreement is a hard error.
//
// Pinned parity vectors (from the C reference — do not "fix" them):
//   init hash  = 0x8a769a0111cf3af3
//   100k soak  = 0x26beb484733ecde0  (this grammar, seed 0x00C0FFEE)
//
// Build & run (no Gradle — the fixture is a two-file kotlinc invocation):
//   kotlinc core/kotlin/WeftReplay.kt fixtures/xlang-replay/vm/kotlin/ReplayTrace.kt \
//           -include-runtime -d /tmp/replay-trace-kotlin.jar
//   java -jar /tmp/replay-trace-kotlin.jar [STEPS [SEED]]

package dev.weft.vmtrace

import dev.weft.WeftReplayState
import dev.weft.WeftTraceKind
import dev.weft.weftReplayInit
import dev.weft.weftReplayStep
import kotlin.system.exitProcess

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

    // scenario shadow (RFC-0019 fixture grammar — normative table above).
    // u32 values live in Longs masked to the unsigned range; toInt() hands
    // the fold the exact 32-bit patterns.
    var latest = 0L
    var wWork = 1L
    var rWork = 2L
    var epoch = 0L
    var revoked = false
    var seq = 0L
    val bufseq = longArrayOf(0L, 0L, 0L)

    val s = WeftReplayState()
    weftReplayInit(s)
    val sb = StringBuilder((steps * 16).toInt())
    var state = seed
    for (i in 0 until steps) {
        state = xorshift32(state)
        val u = state // u32 value (non-negative after the mask in xorshift32)
        val op = (u and 15L).toInt()
        var kind = 0
        var aux = 0
        var data = 0
        if (op < 7) {
            seq = (seq + 1L) and 0xffffffffL
            val len = ((u ushr 4) % 1024L).toInt()
            if (!revoked) {
                bufseq[wWork.toInt()] = seq
                val old = latest
                latest = wWork
                wWork = old
                kind = WeftTraceKind.PUBLISH
                aux = len
                data = seq.toInt()
            } else {
                epoch = (epoch + 1L) and 0xffffffffL
                kind = WeftTraceKind.DROP
                aux = (epoch and 0xffffL).toInt()
                data = seq.toInt()
            }
        } else if (op < 12) {
            data = bufseq[latest.toInt()].toInt()
            val mine = latest
            latest = rWork
            rWork = mine
            kind = WeftTraceKind.CLAIM
        } else if (op == 12) {
            if (!revoked) {
                revoked = true
                kind = WeftTraceKind.REVOKE
                data = epoch.toInt()
            } else {
                kind = WeftTraceKind.ACK
                data = epoch.toInt()
            }
        } else if (op == 13) {
            if (revoked) {
                revoked = false
                kind = WeftTraceKind.ACK
                data = epoch.toInt()
            } else {
                kind = WeftTraceKind.STALL
                data = ((u ushr 4) % 8L).toInt()
            }
        } else if (op == 14) {
            kind = WeftTraceKind.TEAR
            data = seq.toInt()
        } else {
            kind = WeftTraceKind.CANARY_FAIL
            data = seq.toInt()
        }
        val rc = weftReplayStep(s, kind, aux, data)
        if (rc != dev.weft.ReplayResult.OK) {
            System.err.println("replay_emitter: fold disagreement at step $i")
            exitProcess(1)
        }
        // u64 rendered UNSIGNED — 16 lowercase hex digits.
        sb.append(java.lang.Long.toHexString(s.hash).padStart(16, '0'))
    }
    println(sb.toString())
}
