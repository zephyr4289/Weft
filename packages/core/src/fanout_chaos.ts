// fanout_chaos.ts — RFC 0011 deterministic chaos engine, TypeScript port.
//
// "chaos contract" — this file mirrors core/c/fanout_chaos.c (the reference
// oracle) micro-step for micro-step, draw for draw. Given the same config,
// the stepped verdict JSON is BYTE-IDENTICAL to the C engine's (verified by
// ci/scripts/run_chaos_parity.sh and by the golden fixture in
// tools/chaos-fixtures/). Read fanout_chaos.h's header for the normative
// contract; this port adds nothing and drops nothing.
//
// u32 discipline: all PRNG/pattern arithmetic stays in the low 32 bits via
// `>>> 0` (logical shift + wrap) and `Math.imul` (u32 multiply) — the exact
// semantics of C's uint32_t. tword's `seq * 2654435761 + w` uses imul
// because the full product exceeds Number's 2^53 exact-integer range (C
// wraps naturally; JS must not lose bits).
//
// LAW 4 honesty: the stepped engine models the RFC 0004 protocol exactly
// (FI1/FI2 brackets, the bounded 4-attempt claim loop, latest-wins). It is
// not a substitute for the real-ring tests — it is the reproducible
// adversarial layer ON TOP of them.

export interface ChaosConfig {
    seed: number;       // master seed (u32)
    steps: number;      // scheduler step budget (stepped mode)
    slots: number;      // M — ring depth
    words: number;      // W — payload words per slot
    readers: number;    // R — concurrent reader SMs
    frames: number;     // F — frames the writer publishes
    chaosRate: number;  // per-mille fault probability per step
}

export interface ChaosVerdict {
    pass: boolean;
    engine: string;     // "stepped"
    json: string;       // the contract JSON (byte-identical across ports)
}

// ---------------------------------------------------------------------------
// The chaos contract: PRNG + pattern (mirror of core/c/fanout_chaos.c)
// ---------------------------------------------------------------------------

function mix32(x0: number): number {
    let x = x0 >>> 0;
    x = (x ^ (x >>> 16)) >>> 0;
    x = Math.imul(x, 0x7FEB352D) >>> 0;
    x = (x ^ (x >>> 15)) >>> 0;
    x = Math.imul(x, 0x846CA68B) >>> 0;
    x = (x ^ (x >>> 16)) >>> 0;
    return x;
}

class ChaosRng {
    a = 0; b = 0; c = 0; d = 0;

    seed(seed: number): void {
        this.a = mix32(seed ^ 0xA341316C);
        this.b = mix32(seed ^ 0xC8013EA4);
        this.c = (this.a ^ 0x9E3779B9) >>> 0;
        this.d = (this.b ^ 0x85EBCA6B) >>> 0;
    }

    next(): number {
        let t = this.d >>> 0;
        const s = this.a >>> 0;
        this.d = this.c; this.c = this.b; this.b = s;
        t = (t ^ ((t << 11) | 0)) >>> 0;
        t = (t ^ (t >>> 8)) >>> 0;
        this.a = (t ^ s ^ (s >>> 10)) >>> 0;
        return this.a;
    }
}

function tword(seq: number, w: number): number {
    return mix32((Math.imul(seq, 2654435761) + w) >>> 0);
}

// ---------------------------------------------------------------------------
// Stepped engine — the deterministic scheduler (mirror of the C oracle)
// ---------------------------------------------------------------------------

// SM states as plain numeric constants (C-enum parity; Node's strip-only
// TS mode does not support enums — and the values are contract anyway).
const W_IDLE = 0, W_FILL = 1, W_STAMP = 2, W_PUBLISH = 3, W_DONE = 4;
const R_IDLE = 0, R_STAMP_B = 1, R_COPY = 2, R_STAMP_A = 3, R_ACCEPT = 4;
const R_TICK_END = 5, R_SKIP_TICK = 6, R_EXHAUSTED_TICK = 7, R_DONE = 8;

const MAX_SLOTS = 64;
const FREEZE_ADD = [1, 2, 3, 0]; // preempt / stall / throttle / reorder

export function runSteppedChaos(cfg: ChaosConfig): ChaosVerdict {
    if (cfg.slots < 2 || cfg.slots > MAX_SLOTS) throw new Error("chaos: slots out of [2,64]");
    if (cfg.words < 1 || cfg.words > 64) throw new Error("chaos: words out of [1,64]");
    if (cfg.readers < 1 || cfg.readers > 4) throw new Error("chaos: readers out of [1,4]");
    if (cfg.chaosRate > 1000) throw new Error("chaos: chaosRate out of [0,1000]");

    const M = cfg.slots, W = cfg.words, R = cfg.readers, F = cfg.frames;

    // Model ring (u64-safe: all counters < 2^53 in any reachable run).
    const slotSeq: number[] = new Array(M).fill(0);
    const payload: number[][] = [];
    for (let k = 0; k < M; k++) payload.push(new Array(W).fill(0));
    const bracketOpen: boolean[] = new Array(M).fill(false);
    let latest = 0;
    let publishes = 0;

    // Writer SM
    let ws = W_IDLE;
    let wSeq = 1;
    let wSlot = 0;
    let wWord = 0;
    let wRev = false;

    // Reader SMs
    const rs: number[] = new Array(R).fill(R_IDLE);
    const rLast: number[] = new Array(R).fill(0);
    const rTarget: number[][] = [];
    for (let i = 0; i < R; i++) rTarget.push(new Array(W).fill(0));
    const rL: number[] = new Array(R).fill(0);
    const rSlot: number[] = new Array(R).fill(0);
    const rWord: number[] = new Array(R).fill(0);
    const rAttempts: number[] = new Array(R).fill(0);
    const rRevActive: boolean[] = new Array(R).fill(false);

    // Scheduler
    const rng = new ChaosRng();
    rng.seed(cfg.seed);
    const freeze: number[] = new Array(R + 1).fill(0);
    const revFlag: boolean[] = new Array(R + 1).fill(false);

    // Ledger
    const fresh = new Array(R).fill(0);
    const dropped = new Array(R).fill(0);
    const skips = new Array(R).fill(0);
    const exhausted = new Array(R).fill(0);
    let tornAccepted = 0;
    let futureClaims = 0;
    let bracketViolations = 0;
    const injected = [0, 0, 0, 0];
    let stepsExecuted = 0;

    const allDone = (): boolean => {
        if (ws !== W_DONE) return false;
        for (let i = 0; i < R; i++) if (rs[i] !== R_DONE) return false;
        return true;
    };

    const faultDraw = (): void => {
        if (rng.next() % 1000 >= cfg.chaosRate) return;
        const victim = rng.next() % (R + 1);
        const kind = rng.next() % 4;
        freeze[victim] += FREEZE_ADD[kind];
        if (kind === 3) revFlag[victim] = !revFlag[victim];
        injected[kind]++;
    };

    const writerStep = (): void => {
        switch (ws) {
        case W_IDLE:
            if (wSeq > F) { ws = W_DONE; return; }
            wSlot = (wSeq - 1) % M;
            if (bracketOpen[wSlot]) bracketViolations++; // L-C6
            slotSeq[wSlot] = 0;                          // FI1a: invalidate FIRST
            bracketOpen[wSlot] = true;
            wRev = revFlag[0];                           // fault axis: reorder
            wWord = wRev ? (W - 1) : 0;
            ws = W_FILL;
            return;
        case W_FILL:
            payload[wSlot][wWord] = tword(wSeq, wWord);
            if (wRev) {
                if (wWord === 0) ws = W_STAMP;
                else wWord--;
            } else {
                wWord++;
                if (wWord === W) ws = W_STAMP;
            }
            return;
        case W_STAMP:
            slotSeq[wSlot] = wSeq;                       // FI1b: stamp (Release)
            bracketOpen[wSlot] = false;
            ws = W_PUBLISH;
            return;
        case W_PUBLISH:
            latest = wSeq;                               // the publication point
            publishes++;
            wSeq++;
            ws = W_IDLE;
            return;
        case W_DONE:
            return;
        }
    };

    const readerStep = (i: number): void => {
        switch (rs[i]) {
        case R_IDLE: {
            const L = latest;
            if (L === 0 || L === rLast[i]) { rs[i] = R_TICK_END; return; }
            rL[i] = L;
            rAttempts[i] = 0;
            rs[i] = R_STAMP_B;
            return;
        }
        case R_STAMP_B: {
            const L = rL[i];
            rSlot[i] = (L - 1) % M;
            const sB = slotSeq[rSlot[i]];
            if (sB !== L) {
                const l2 = latest;
                if (l2 === L) { skips[i]++; rs[i] = R_SKIP_TICK; return; }
                rL[i] = l2;
                rAttempts[i]++;
                rs[i] = rAttempts[i] >= 4 ? R_EXHAUSTED_TICK : R_STAMP_B;
                return;
            }
            rWord[i] = revFlag[i + 1] ? (W - 1) : 0;     // fault axis: reorder
            rRevActive[i] = revFlag[i + 1];
            rs[i] = R_COPY;
            return;
        }
        case R_COPY: {
            const w = rWord[i];
            rTarget[i][w] = payload[rSlot[i]][w];
            if (rRevActive[i]) {
                if (w === 0) rs[i] = R_STAMP_A;
                else rWord[i] = w - 1;
            } else {
                rWord[i]++;
                if (rWord[i] === W) rs[i] = R_STAMP_A;
            }
            return;
        }
        case R_STAMP_A: {
            const sA = slotSeq[rSlot[i]];
            if (sA === rL[i]) { rs[i] = R_ACCEPT; return; }
            rL[i] = latest;                              // torn copy — chase newest
            rAttempts[i]++;
            rs[i] = rAttempts[i] >= 4 ? R_EXHAUSTED_TICK : R_STAMP_B;
            return;
        }
        case R_ACCEPT: {
            const L = rL[i];
            if (L > F) futureClaims++;                   // L-C2
            for (let w = 0; w < W; w++) {
                if (rTarget[i][w] !== tword(L, w)) { tornAccepted++; break; } // L-C1
            }
            dropped[i] += L - rLast[i] - 1;
            rLast[i] = L;
            fresh[i]++;
            rs[i] = R_TICK_END;
            return;
        }
        case R_TICK_END:
            if (ws === W_DONE && rLast[i] === F) rs[i] = R_DONE;
            else rs[i] = R_IDLE;
            return;
        case R_SKIP_TICK:
            rs[i] = R_TICK_END;
            return;
        case R_EXHAUSTED_TICK:
            exhausted[i]++;
            rs[i] = R_TICK_END;
            return;
        case R_DONE:
            return;
        }
    };

    const advance = (tid: number): void => {
        if (tid === 0) writerStep();
        else readerStep(tid - 1);
    };

    // The scheduler loop (chaos contract shape — identical to the C oracle).
    while (stepsExecuted < cfg.steps && !allDone()) {
        const tid = rng.next() % (R + 1);
        faultDraw();
        if (freeze[tid] > 0) freeze[tid]--;  // scheduled, but made no progress
        else advance(tid);
        stepsExecuted++;
    }

    // Drain: round-robin (writer, reader 1..R), no faults, until done.
    const drainBound = 64 * (F + 16 * R * (F + 8)) + 64;
    let drainedSteps = 0;
    while (!allDone() && drainedSteps < drainBound) {
        for (let tid = 0; tid <= R && !allDone(); tid++) {
            if (freeze[tid] > 0) freeze[tid] = 0; // faults end with the budget
            if (!allDone()) advance(tid);
        }
        drainedSteps++;
    }
    const drained = allDone();

    // L-C3 telescoping + L-C4 completion (adjudicated once, at drain end).
    let telescopingOk = drained;
    for (let i = 0; i < R; i++) {
        if (dropped[i] !== rLast[i] - fresh[i]) telescopingOk = false;
        if (drained && rLast[i] !== F) telescopingOk = false;
    }
    if (publishes !== F) telescopingOk = false; // L-C4

    const pass = telescopingOk && tornAccepted === 0 && futureClaims === 0 &&
                 bracketViolations === 0;
    const telescopingStr = pass ? "OK" : "VIOLATED";
    const verdictStr = pass ? "PASS" : "FAIL";

    // Contract JSON — byte-identical to the C oracle (fixed field order).
    const json =
        `{"engine":"weft-chaos-stepped","v":1,"seed":${cfg.seed},` +
        `"steps":${cfg.steps},` +
        `"slots":${M},"words":${W},"readers":${R},"frames":${F},` +
        `"chaosRate":${cfg.chaosRate},` +
        `"stepsExecuted":${stepsExecuted},` +
        `"injections":{"preempt":${injected[0]},"stall":${injected[1]},` +
        `"throttle":${injected[2]},"reorder":${injected[3]}},` +
        `"ledger":{"publishes":${publishes},` +
        `"fresh":[${fresh.join(",")}],` +
        `"dropped":[${dropped.join(",")}],` +
        `"lastSeq":[${rLast.join(",")}],` +
        `"skips":[${skips.join(",")}],` +
        `"exhausted":[${exhausted.join(",")}],` +
        `"tornAccepted":${tornAccepted},"futureClaims":${futureClaims},` +
        `"bracketViolations":${bracketViolations},"drained":${drained}},` +
        `"telescoping":"${telescopingStr}",` +
        `"verdict":"${verdictStr}"}`;

    return { pass, engine: "stepped", json };
}

// Pinned vectors — MUST match core/c/fanout_chaos.c's selftest (the parity
// anchor; see tools/chaos-fixtures/).
export function selftest(): boolean {
    if (mix32(0xDEADBEEF) !== 3861431939) return false;
    if (tword(1, 0) !== 1834104592) return false;
    if (tword(7, 3) !== 2500287888) return false;
    if (tword(100, 15) !== 4197121613) return false;
    const rng = new ChaosRng();
    rng.seed(42);
    const draws = [1034221180, 2302191726, 1921777443, 3822789115,
                   4193179225, 3051818586, 2559959645, 2724063783];
    for (const want of draws) {
        if (rng.next() !== want) return false;
    }
    const v = runSteppedChaos({ seed: 7, steps: 4000, slots: 2, words: 2,
                                readers: 2, frames: 4, chaosRate: 300 });
    return v.pass && v.json.includes('"verdict":"PASS"');
}
