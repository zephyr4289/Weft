// FanoutChaos.kt — RFC 0011 deterministic chaos engine, JVM/Kotlin port.
//
// "chaos contract" — mirrors core/c/fanout_chaos.c (the reference oracle)
// micro-step for micro-step, draw for draw. Given the same config, the
// stepped verdict JSON is BYTE-IDENTICAL to the C engine's (verified by
// ci/scripts/run_chaos_parity.sh; the golden fixture lives in
// tools/chaos-fixtures/). Read core/c/fanout_chaos.h for the normative
// contract; this port adds nothing and drops nothing.
//
// u32 discipline: Kotlin's UInt is the exact analog of C's uint32_t —
// times/shl wrap modulo 2^32, ushr is the logical right shift. No masking
// gymnastics needed; the contract vectors pin the behavior.
//
// LAW 4 honesty: the stepped engine models the RFC 0004 protocol exactly
// (FI1/FI2 brackets, the bounded 4-attempt claim loop, latest-wins). The
// real-ring JVM chaos leg (threads + chaos points over WeftFanoutBroadcaster)
// lives in the Android test suite; this file is the parity oracle.

import kotlin.system.exitProcess

data class ChaosConfig(
    val seed: Int,        // master seed (u32)
    val steps: Long,      // scheduler step budget (stepped mode)
    val slots: Int,       // M — ring depth
    val words: Int,       // W — payload words per slot
    val readers: Int,     // R — concurrent reader SMs
    val frames: Int,      // F — frames the writer publishes
    val chaosRate: Int,   // per-mille fault probability per step
)

data class ChaosVerdict(val pass: Boolean, val engine: String, val json: String)

// ---------------------------------------------------------------------------
// The chaos contract: PRNG + pattern (mirror of core/c/fanout_chaos.c)
// ---------------------------------------------------------------------------

internal fun mix32(x0: UInt): UInt {
    var x = x0
    x = x xor (x shr 16)
    x = x * 0x7FEB352Du
    x = x xor (x shr 15)
    x = x * 0x846CA68Bu
    x = x xor (x shr 16)
    return x
}

internal class ChaosRng {
    var a: UInt = 0u; var b: UInt = 0u; var c: UInt = 0u; var d: UInt = 0u

    fun seed(seed: Int) {
        a = mix32(seed.toUInt() xor 0xA341316Cu)
        b = mix32(seed.toUInt() xor 0xC8013EA4u)
        c = a xor 0x9E3779B9u
        d = b xor 0x85EBCA6Bu
    }

    fun next(): UInt {
        var t = d
        val s = a
        d = c; c = b; b = s
        t = t xor (t shl 11)
        t = t xor (t shr 8)
        a = t xor s xor (s shr 10)
        return a
    }
}

internal fun tword(seq: Int, w: Int): UInt =
    mix32((seq.toUInt() * 2654435761u) + w.toUInt())

// ---------------------------------------------------------------------------
// Stepped engine — the deterministic scheduler (mirror of the C oracle)
// ---------------------------------------------------------------------------

private const val W_IDLE = 0; private const val W_FILL = 1
private const val W_STAMP = 2; private const val W_PUBLISH = 3; private const val W_DONE = 4
private const val R_IDLE = 0; private const val R_STAMP_B = 1; private const val R_COPY = 2
private const val R_STAMP_A = 3; private const val R_ACCEPT = 4; private const val R_TICK_END = 5
private const val R_SKIP_TICK = 6; private const val R_EXHAUSTED_TICK = 7; private const val R_DONE = 8

private val FREEZE_ADD = intArrayOf(1, 2, 3, 0) // preempt / stall / throttle / reorder

fun runSteppedChaos(cfg: ChaosConfig): ChaosVerdict {
    require(cfg.slots in 2..64) { "chaos: slots out of [2,64]" }
    require(cfg.words in 1..64) { "chaos: words out of [1,64]" }
    require(cfg.readers in 1..4) { "chaos: readers out of [1,4]" }
    require(cfg.chaosRate in 0..1000) { "chaos: chaosRate out of [0,1000]" }

    val M = cfg.slots; val W = cfg.words; val R = cfg.readers; val F = cfg.frames

    // Model ring (u64 counters stay < 2^53 in any reachable run).
    val slotSeq = LongArray(M)
    val payload = Array(M) { UIntArray(W) }
    val bracketOpen = BooleanArray(M)
    var latest = 0L
    var publishes = 0L

    // Writer SM
    var ws = W_IDLE
    var wSeq = 1L
    var wSlot = 0
    var wWord = 0
    var wRev = false

    // Reader SMs
    val rs = IntArray(R) { R_IDLE }
    val rLast = LongArray(R)
    val rTarget = Array(R) { LongArray(W) }
    val rL = LongArray(R)
    val rSlot = IntArray(R)
    val rWord = IntArray(R)
    val rAttempts = IntArray(R)
    val rRevActive = BooleanArray(R)

    // Scheduler
    val rng = ChaosRng()
    rng.seed(cfg.seed)
    val freeze = IntArray(R + 1)
    val revFlag = BooleanArray(R + 1)

    // Ledger
    val fresh = LongArray(R)
    val dropped = LongArray(R)
    val skips = LongArray(R)
    val exhausted = LongArray(R)
    var tornAccepted = 0L
    var futureClaims = 0L
    var bracketViolations = 0L
    val injected = LongArray(4)
    var stepsExecuted = 0L

    fun allDone(): Boolean {
        if (ws != W_DONE) return false
        for (i in 0 until R) if (rs[i] != R_DONE) return false
        return true
    }

    fun faultDraw() {
        if ((rng.next() % 1000u).toInt() >= cfg.chaosRate) return
        val victim = (rng.next() % (R.toUInt() + 1u)).toInt()
        val kind = (rng.next() % 4u).toInt()
        freeze[victim] += FREEZE_ADD[kind]
        if (kind == 3) revFlag[victim] = !revFlag[victim]
        injected[kind]++
    }

    fun writerStep() {
        when (ws) {
            W_IDLE -> {
                if (wSeq > F) { ws = W_DONE; return }
                wSlot = ((wSeq - 1) % M).toInt()
                if (bracketOpen[wSlot]) bracketViolations++ // L-C6
                slotSeq[wSlot] = 0                          // FI1a: invalidate FIRST
                bracketOpen[wSlot] = true
                wRev = revFlag[0]                           // fault axis: reorder
                wWord = if (wRev) W - 1 else 0
                ws = W_FILL
            }
            W_FILL -> {
                payload[wSlot][wWord] = tword(wSeq.toInt(), wWord)
                if (wRev) {
                    if (wWord == 0) ws = W_STAMP else wWord--
                } else {
                    wWord++
                    if (wWord == W) ws = W_STAMP
                }
            }
            W_STAMP -> {
                slotSeq[wSlot] = wSeq                       // FI1b: stamp (Release)
                bracketOpen[wSlot] = false
                ws = W_PUBLISH
            }
            W_PUBLISH -> {
                latest = wSeq                               // the publication point
                publishes++
                wSeq++
                ws = W_IDLE
            }
        }
    }

    fun readerStep(i: Int) {
        when (rs[i]) {
            R_IDLE -> {
                val L = latest
                if (L == 0L || L == rLast[i]) { rs[i] = R_TICK_END; return }
                rL[i] = L
                rAttempts[i] = 0
                rs[i] = R_STAMP_B
            }
            R_STAMP_B -> {
                val L = rL[i]
                rSlot[i] = ((L - 1) % M).toInt()
                val sB = slotSeq[rSlot[i]]
                if (sB != L) {
                    val l2 = latest
                    if (l2 == L) { skips[i]++; rs[i] = R_SKIP_TICK; return }
                    rL[i] = l2
                    rAttempts[i]++
                    rs[i] = if (rAttempts[i] >= 4) R_EXHAUSTED_TICK else R_STAMP_B
                    return
                }
                rWord[i] = if (revFlag[i + 1]) W - 1 else 0  // fault axis: reorder
                rRevActive[i] = revFlag[i + 1]
                rs[i] = R_COPY
            }
            R_COPY -> {
                val w = rWord[i]
                rTarget[i][w] = payload[rSlot[i]][w].toLong()
                if (rRevActive[i]) {
                    if (w == 0) rs[i] = R_STAMP_A else rWord[i] = w - 1
                } else {
                    rWord[i]++
                    if (rWord[i] == W) rs[i] = R_STAMP_A
                }
            }
            R_STAMP_A -> {
                val sA = slotSeq[rSlot[i]]
                if (sA == rL[i]) { rs[i] = R_ACCEPT; return }
                rL[i] = latest                               // torn copy — chase newest
                rAttempts[i]++
                rs[i] = if (rAttempts[i] >= 4) R_EXHAUSTED_TICK else R_STAMP_B
            }
            R_ACCEPT -> {
                val L = rL[i]
                if (L > F) futureClaims++                    // L-C2
                for (w in 0 until W) {
                    if (rTarget[i][w] != tword(L.toInt(), w).toLong()) {
                        tornAccepted++; break                // L-C1
                    }
                }
                dropped[i] += L - rLast[i] - 1
                rLast[i] = L
                fresh[i]++
                rs[i] = R_TICK_END
            }
            R_TICK_END -> {
                rs[i] = if (ws == W_DONE && rLast[i] == F.toLong()) R_DONE else R_IDLE
            }
            R_SKIP_TICK -> rs[i] = R_TICK_END
            R_EXHAUSTED_TICK -> { exhausted[i]++; rs[i] = R_TICK_END }
        }
    }

    fun advance(tid: Int) {
        if (tid == 0) writerStep() else readerStep(tid - 1)
    }

    // The scheduler loop (chaos contract shape — identical to the C oracle).
    while (stepsExecuted < cfg.steps && !allDone()) {
        val tid = (rng.next() % (R.toUInt() + 1u)).toInt()
        faultDraw()
        if (freeze[tid] > 0) freeze[tid]--   // scheduled, but made no progress
        else advance(tid)
        stepsExecuted++
    }

    // Drain: round-robin (writer, reader 1..R), no faults, until done.
    val drainBound = 64L * (F + 16L * R * (F + 8)) + 64
    var drainedSteps = 0L
    while (!allDone() && drainedSteps < drainBound) {
        for (tid in 0..R) {
            if (allDone()) break
            if (freeze[tid] > 0) freeze[tid] = 0  // faults end with the budget
            if (!allDone()) advance(tid)
        }
        drainedSteps++
    }
    val drained = allDone()

    // L-C3 telescoping + L-C4 completion (adjudicated once, at drain end).
    var telescopingOk = drained
    for (i in 0 until R) {
        if (dropped[i] != rLast[i] - fresh[i]) telescopingOk = false
        if (drained && rLast[i] != F.toLong()) telescopingOk = false
    }
    if (publishes != F.toLong()) telescopingOk = false // L-C4

    val pass = telescopingOk && tornAccepted == 0L && futureClaims == 0L &&
               bracketViolations == 0L

    // Contract JSON — byte-identical to the C oracle (fixed field order).
    val json = StringBuilder()
    json.append("{\"engine\":\"weft-chaos-stepped\",\"v\":1,\"seed\":${cfg.seed.toUInt()},")
    json.append("\"steps\":${cfg.steps},")
    json.append("\"slots\":$M,\"words\":$W,\"readers\":$R,\"frames\":$F,")
    json.append("\"chaosRate\":${cfg.chaosRate},")
    json.append("\"stepsExecuted\":$stepsExecuted,")
    json.append("\"injections\":{\"preempt\":${injected[0]},\"stall\":${injected[1]},")
    json.append("\"throttle\":${injected[2]},\"reorder\":${injected[3]}},")
    json.append("\"ledger\":{\"publishes\":$publishes,")
    json.append("\"fresh\":[${fresh.joinToString(",")}],")
    json.append("\"dropped\":[${dropped.joinToString(",")}],")
    json.append("\"lastSeq\":[${rLast.joinToString(",")}],")
    json.append("\"skips\":[${skips.joinToString(",")}],")
    json.append("\"exhausted\":[${exhausted.joinToString(",")}],")
    json.append("\"tornAccepted\":$tornAccepted,\"futureClaims\":$futureClaims,")
    json.append("\"bracketViolations\":$bracketViolations,\"drained\":$drained},")
    val telescopingStr = if (pass) "OK" else "VIOLATED"
    val verdictStr = if (pass) "PASS" else "FAIL"
    json.append("\"telescoping\":\"$telescopingStr\",")
    json.append("\"verdict\":\"$verdictStr\"}")

    return ChaosVerdict(pass, "stepped", json.toString())
}

// Pinned vectors — MUST match core/c/fanout_chaos.c's selftest (the parity
// anchor; see tools/chaos-fixtures/).
fun chaosSelftest(): Boolean {
    if (mix32(0xDEADBEEFu) != 3861431939u) return false
    if (tword(1, 0) != 1834104592u) return false
    if (tword(7, 3) != 2500287888u) return false
    if (tword(100, 15) != 4197121613u) return false
    val rng = ChaosRng()
    rng.seed(42)
    val draws = uintArrayOf(1034221180u, 2302191726u, 1921777443u, 3822789115u,
                            4193179225u, 3051818586u, 2559959645u, 2724063783u)
    for (want in draws) {
        if (rng.next() != want) return false
    }
    val v = runSteppedChaos(ChaosConfig(7, 4000, 2, 2, 2, 4, 300))
    return v.pass && v.json.contains("\"verdict\":\"PASS\"")
}

// CLI — stepped mode only (the parity oracle surface; the C runner carries
// the free-mode tier on real OS threads).
fun main(args: Array<String>) {
    if (args.size >= 8 && args[0] == "stepped") {
        val cfg = ChaosConfig(
            seed = args[7].toUInt().toInt(),
            steps = args[1].toLong(),
            slots = args[2].toInt(),
            words = args[3].toInt(),
            readers = args[4].toInt(),
            frames = args[5].toInt(),
            chaosRate = args[6].toInt(),
        )
        val v = runSteppedChaos(cfg)
        println(v.json)
        System.err.println(
            "chaos-stepped(jvm): ${if (v.pass) "PASS" else "FAIL"}")
        exitProcess(if (v.pass) 0 else 1)
    }
    if (args.size == 1 && args[0] == "selftest") {
        val ok = chaosSelftest()
        System.err.println("chaos-selftest(jvm): ${if (ok) "PASS" else "FAIL"}")
        exitProcess(if (ok) 0 else 1)
    }
    System.err.println("usage: FanoutChaos stepped <steps> <slots> <words> <readers> <frames> <chaosRate> <seed> | selftest")
    exitProcess(2)
}
