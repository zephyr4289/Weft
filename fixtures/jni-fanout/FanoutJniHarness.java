package dev.weft;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.ArrayList;
import java.util.List;

/**
 * FanoutJniHarness — JVM torture harness for the weft_jni.c fan-out surface.
 *
 * WHY EXISTS: the repo's Android unit tests run on the host JVM via Gradle,
 * but the JNI surface itself had no executable spec outside a device build.
 * This harness IS that spec: it compiles the exact C sources the Android
 * build compiles (weft_jni.c + core/c/weft.c + core/c/fanout.c) into a host
 * .so and drives the full RFC-0004 contract through JNI — the F-series
 * semantics AND a 1-writer x 3-reader threaded torture with the kernel's
 * shared payload pattern (pat) validation and exact telescoping accounting.
 * The Kotlin FanoutTest (android/weft-core) mirrors these assertions for
 * the pure-JVM ring; this file pins the NATIVE path. Same discipline as
 * fixtures/xlang-fanout: independent implementation, committed evidence.
 *
 * Run: bash fixtures/jni-fanout/run.sh   (from the repo root)
 * Exit: 0 all green; 1 any failure. Each assertion prints one line.
 *
 * STATUS: source + committed evidence log (litmus/evidence/fanout/
 * jni-harness.log, written by run.sh); not part of gradlew test — it needs
 * the host gcc-built .so, which the Android build system does not produce.
 */

// Native declarations mirror android/.../TriadNative.kt (the Kotlin object's
// external funs bind the same symbols; here they are plain instance methods).
class TriadNative {
    static {
        String p = System.getProperty("weft.lib");
        if (p == null || p.isEmpty()) p = System.getenv().getOrDefault("WEFT_LIB", "libweft_core.so");
        System.load(p.startsWith("/") ? p : System.getProperty("user.dir") + "/" + p);
    }

    // --- Fan-out (RFC 0004) ---
    native long fanoutCreate(int payloadBytes, int slotCount);
    native long fanoutCreateForeign(ByteBuffer ringBuf, int payloadBytes, int slotCount);
    native long fanoutRingBytes(int payloadBytes, int slotCount);
    native void fanoutDestroy(long fanoutHandle);

    native ByteBuffer fanoutBegin(long fanoutHandle);
    native int fanoutFill(long fanoutHandle, ByteBuffer srcBuf, int len);
    native long fanoutPublish(long fanoutHandle);

    native long fanoutLatestSeq(long fanoutHandle);
    native long fanoutPublishes(long fanoutHandle);

    native long fanoutReaderCreate(long fanoutHandle);
    native long fanoutReaderCreateForeign(ByteBuffer ringBuf, int payloadBytes, int slotCount);
    native void fanoutReaderDestroy(long readerHandle);

    native long fanoutClaim(long readerHandle);
    native boolean fanoutClaimFresh(long readerHandle);
    native long fanoutClaimDropped(long readerHandle);
    native ByteBuffer fanoutViewBuffer(long readerHandle);

    native long fanoutReaderStatsReads(long readerHandle);
    native long fanoutReaderStatsFresh(long readerHandle);
    native long fanoutReaderStatsDrops(long readerHandle);
    native long fanoutReaderStatsSkipped(long readerHandle);
    native long fanoutReaderStatsExhausted(long readerHandle);
}

public class FanoutJniHarness {

    static int checks = 0;
    static final List<String> failures = new ArrayList<>();

    static void check(boolean cond, String msg) {
        if (cond) { checks++; System.out.println("ok: " + msg); }
        else { failures.add(msg); System.out.println("FAIL: " + msg); }
    }

    // --- The kernel's shared payload pattern (04-LITMUS §0.1), Java port ---
    static int mix32(int x) {
        int v = x;
        v ^= v >>> 16;
        v *= 0x7FEB352D;
        v ^= v >>> 15;
        v *= 0x846CA68B;
        v ^= v >>> 16;
        return v;
    }
    static int pat(int seq, int i) {
        int x = (int) (seq * 2654435761L + i * 2246822519L);
        return mix32(x) & 0xFF;
    }

    static final int PAYLOAD = 256; // bytes; %4 == 0
    static final int SLOTS = 4;
    static final TriadNative N = new TriadNative();

    public static void main(String[] args) throws Exception {
        long t0 = System.nanoTime();
        jf1_geometry();
        jf2_roundtrip();
        jf3_telescoping();
        jf4_skipAndNoop();
        jf5_multiReader();
        jf6_foreignRing();
        jf7_fillPath();
        jf8_torture();
        long ms = (System.nanoTime() - t0) / 1_000_000;

        System.out.println();
        System.out.println((failures.isEmpty() ? "PASS" : "FAIL") + ": " + failures.size()
                + " failure(s), " + checks + " check(s), " + ms + " ms total");
        System.out.println("env: java " + System.getProperty("java.version") + " ("
                + System.getProperty("os.arch") + "), host gcc .so, -O2");
        if (!failures.isEmpty()) System.exit(1);
    }

    // JF1: geometry validation refuses bad pairs (panic-shielded 0 handle).
    static void jf1_geometry() {
        check(N.fanoutCreate(0, 4) == 0, "JF1 payloadBytes=0 rejected");
        check(N.fanoutCreate(6, 4) == 0, "JF1 payload not multiple of 4 rejected");
        check(N.fanoutCreate(64, 1) == 0, "JF1 slotCount<2 rejected");
        check(N.fanoutCreate(64, 65) == 0, "JF1 slotCount>64 rejected");
        check(N.fanoutRingBytes(64, 4) == 16 + 8 * 4 + 4 * 64,
                "JF1 ring_bytes = 16 + 8M + M*payload (the interop contract)");
        check(N.fanoutRingBytes(6, 4) == 0, "JF1 ring_bytes rejects bad geometry");
    }

    // JF2: begin/publish/claim roundtrip through direct ByteBuffers.
    static void jf2_roundtrip() {
        long f = N.fanoutCreate(PAYLOAD, SLOTS);
        check(f != 0, "JF2 fanoutCreate handle");
        ByteBuffer cur = N.fanoutBegin(f);
        check(cur != null && cur.capacity() == PAYLOAD, "JF2 begin: direct BB cursor, cap=payload");
        for (int i = 0; i < PAYLOAD; i++) cur.put(i, (byte) pat(1, i));
        long seq = N.fanoutPublish(f);
        check(seq == 1, "JF2 publish returns frame seq 1");

        long r = N.fanoutReaderCreate(f);
        check(r != 0, "JF2 readerCreate on the broadcaster's ring");
        long got = N.fanoutClaim(r);
        check(N.fanoutClaimFresh(r) && got == 1 && N.fanoutClaimDropped(r) == 0,
                "JF2 first claim: fresh, seq 1, dropped 0");
        ByteBuffer view = N.fanoutViewBuffer(r);
        check(view != null && view.capacity() == PAYLOAD, "JF2 view buffer: cap=payload");
        boolean bytesOk = true;
        for (int i = 0; i < PAYLOAD; i++) if ((view.get(i) & 0xFF) != pat(1, i)) bytesOk = false;
        check(bytesOk, "JF2 claimed bytes match pat(1,i) — no tear on the happy path");

        check(N.fanoutLatestSeq(f) == 1 && N.fanoutPublishes(f) == 1,
                "JF2 advisory latestSeq/publishes (AXIOM T)");
        N.fanoutReaderDestroy(r);
        N.fanoutDestroy(f);
    }

    // JF3: drop accounting telescopes exactly (FI3).
    static void jf3_telescoping() {
        long f = N.fanoutCreate(PAYLOAD, SLOTS);
        long r = N.fanoutReaderCreate(f);
        long fresh = 0, sumDropped = 0, lastSeq = 0;
        for (int seq = 1; seq <= 40; seq++) {
            ByteBuffer cur = N.fanoutBegin(f);
            for (int i = 0; i < PAYLOAD; i++) cur.put(i, (byte) pat(seq, i));
            N.fanoutPublish(f);
            if (seq % 4 == 0) { // claim every 4th frame -> 3 dropped each cycle
                long claimed = N.fanoutClaim(r);
                if (N.fanoutClaimFresh(r)) {
                    fresh++;
                    sumDropped += N.fanoutClaimDropped(r);
                    lastSeq = claimed;
                }
            }
        }
        check(fresh == 10 && lastSeq == 40, "JF3 10 fresh claims, last frame 40");
        check(sumDropped == 30, "JF3 telescoping: sum(dropped) == 30 exactly");
        check(lastSeq - fresh == sumDropped, "JF3 identity: lastSeq - freshClaims == sum(dropped)");
        N.fanoutReaderDestroy(r);
        N.fanoutDestroy(f);
    }

    // JF4: graceful skip — the LATEST frame's slot mid-overwrite while the
    // reader holds an OLDER frame (skip path needs L != lastSeq AND sB != L,
    // with latestSeq unchanged on the re-read). Also: publish-no-begin.
    static void jf4_skipAndNoop() {
        long f = N.fanoutCreate(PAYLOAD, SLOTS);
        long r = N.fanoutReaderCreate(f);
        ByteBuffer cur = N.fanoutBegin(f);
        for (int i = 0; i < PAYLOAD; i++) cur.put(i, (byte) pat(1, i));
        N.fanoutPublish(f);
        long held = N.fanoutClaim(r);
        check(N.fanoutClaimFresh(r) && held == 1, "JF4 baseline: reader holds frame 1");

        // Publish 2..5 (latest = 5, slot (5-1)%4 = 0), reader does NOT claim.
        for (int seq = 2; seq <= SLOTS + 1; seq++) {
            cur = N.fanoutBegin(f);
            for (int i = 0; i < PAYLOAD; i++) cur.put(i, (byte) pat(seq, i));
            N.fanoutPublish(f);
        }
        // M begins WITHOUT publishing: the M-th re-opens slot 0 (frame 5's
        // own slot), invalidating its stamp — the mid-overwrite window.
        for (int b = 0; b < SLOTS; b++) N.fanoutBegin(f);
        long seq = N.fanoutClaim(r);
        check(!N.fanoutClaimFresh(r) && seq == 1,
                "JF4 mid-overwrite claim: graceful skip, keeps frame 1 (Law 1: bounded, counted)");
        check(N.fanoutReaderStatsSkipped(r) == 1, "JF4 skip counted in reader stats (never silent)");

        long seq9 = N.fanoutPublish(f); // publish the begun frame (wSeq = 5+4)
        check(seq9 == 2 * SLOTS + 1, "JF4 publish after the skip publishes frame " + (2 * SLOTS + 1));
        N.fanoutClaim(r);
        check(N.fanoutClaimFresh(r) && N.fanoutClaimDropped(r) == 2 * SLOTS + 1 - 1 - 1,
                "JF4 next claim is fresh with exact telescoped drops");

        // A publish without a begin re-stamps the SAME frame (idempotent,
        // returns the last frame seq — TS-port parity).
        long noop = N.fanoutPublish(f);
        check(noop == 2 * SLOTS + 1, "JF4 publish-without-begin returns the last frame seq (no-op)");
        N.fanoutReaderDestroy(r);
        N.fanoutDestroy(f);
    }

    // JF5: N readers on one ring are fully independent (RFC-0004 motivation).
    static void jf5_multiReader() {
        long f = N.fanoutCreate(PAYLOAD, SLOTS);
        final int R = 3;
        long[] readers = new long[R];
        int[] cadence = {1, 3, 7};
        long[] fresh = new long[R], dropped = new long[R];
        for (int i = 0; i < R; i++) readers[i] = N.fanoutReaderCreate(f);

        for (int seq = 1; seq <= 70; seq++) {
            ByteBuffer cur = N.fanoutBegin(f);
            for (int i = 0; i < PAYLOAD; i++) cur.put(i, (byte) pat(seq, i));
            N.fanoutPublish(f);
            for (int i = 0; i < R; i++) {
                if (seq % cadence[i] == 0) {
                    long claimed = N.fanoutClaim(readers[i]);
                    if (N.fanoutClaimFresh(readers[i])) {
                        fresh[i]++;
                        dropped[i] += N.fanoutClaimDropped(readers[i]);
                    }
                }
            }
        }
        for (int i = 0; i < R; i++) {
            long lastClaimed = (70 / cadence[i]) * cadence[i]; // highest claimed frame
            check(fresh[i] == 70 / cadence[i],
                    "JF5 reader " + i + " fresh claims == 70/" + cadence[i]);
            check(dropped[i] == lastClaimed - fresh[i],
                    "JF5 reader " + i + " telescoping exact: sum(dropped) == lastClaimed(" + lastClaimed + ") - fresh");
            check(N.fanoutReaderStatsReads(readers[i]) == 70 / cadence[i],
                    "JF5 reader " + i + " reads == claim calls");
            N.fanoutReaderDestroy(readers[i]);
        }
        N.fanoutDestroy(f);
    }

    // JF6: broadcaster + readers on FOREIGN (Java-allocated) ring bytes — the
    // byte-layout contract exercised from the outside, exactly as a
    // TS-produced SharedArrayBuffer session would ride in.
    static void jf6_foreignRing() {
        long ringBytes = N.fanoutRingBytes(PAYLOAD, SLOTS);
        ByteBuffer ring = ByteBuffer.allocateDirect((int) ringBytes);
        // allocateDirect zeroes memory — the fresh-ring invariants (latestSeq=0,
        // publishes=0, every slotSeq invalidated) hold by construction.
        long f = N.fanoutCreateForeign(ring, PAYLOAD, SLOTS);
        check(f != 0, "JF6 foreign broadcaster attaches by geometry alone");

        ByteBuffer cur = N.fanoutBegin(f);
        for (int i = 0; i < PAYLOAD; i++) cur.put(i, (byte) pat(1, i));
        check(N.fanoutPublish(f) == 1, "JF6 publish on foreign ring (numbering from latestSeq=0)");

        long r = N.fanoutReaderCreateForeign(ring, PAYLOAD, SLOTS);
        check(r != 0, "JF6 foreign reader attaches");
        long got = N.fanoutClaim(r);
        check(N.fanoutClaimFresh(r) && got == 1,
                "JF6 foreign reader claims frame 1 — one ring, two entry points");

        // Geometry mismatch must fail fast (a wrong pair never tears).
        check(N.fanoutCreateForeign(ring, PAYLOAD, 3) == 0,
                "JF6 foreign attach rejects ring_bytes mismatch");
        check(N.fanoutReaderCreateForeign(ring, PAYLOAD, 3) == 0,
                "JF6 foreign reader rejects ring_bytes mismatch");
        N.fanoutReaderDestroy(r);
        N.fanoutDestroy(f); // foreign ring: destroy frees nothing the JVM owns
    }

    // JF7: the fanoutFill data path (buffer-copy mode, the weftPublish
    // data!=null analog for the fan-out surface).
    static void jf7_fillPath() {
        long f = N.fanoutCreate(PAYLOAD, SLOTS);
        long r = N.fanoutReaderCreate(f);
        ByteBuffer src = ByteBuffer.allocateDirect(PAYLOAD);
        for (int i = 0; i < PAYLOAD; i++) src.put(i, (byte) pat(9, i));
        N.fanoutBegin(f);
        check(N.fanoutFill(f, src, PAYLOAD) == PAYLOAD / 4, "JF7 fill copies payload/4 words");
        check(N.fanoutPublish(f) == 1, "JF7 publish after fill");
        N.fanoutClaim(r);
        ByteBuffer view = N.fanoutViewBuffer(r);
        boolean ok = true;
        for (int i = 0; i < PAYLOAD; i++) if ((view.get(i) & 0xFF) != pat(9, i)) ok = false;
        check(N.fanoutClaimFresh(r) && ok, "JF7 fill path delivers intact bytes");
        check(N.fanoutFill(f, src, 3) == -1, "JF7 fill rejects len%4!=0");
        N.fanoutReaderDestroy(r);
        N.fanoutDestroy(f);
    }

    // JF8: torture — 1 writer thread x 3 reader threads, pat validation on
    // every fresh claim, exact telescoping per reader, self-terminating.
    static void jf8_torture() throws Exception {
        final int FRAMES = 200_000;
        final int R = 3;
        long f = N.fanoutCreate(PAYLOAD, SLOTS);
        final long[] readers = new long[R];
        final ByteBuffer[] views = new ByteBuffer[R];
        for (int i = 0; i < R; i++) {
            readers[i] = N.fanoutReaderCreate(f);
            views[i] = N.fanoutViewBuffer(readers[i]);
        }

        final AtomicBoolean running = new AtomicBoolean(true);
        final AtomicLong[] statFresh = {new AtomicLong(), new AtomicLong(), new AtomicLong()};
        final AtomicLong[] statDropped = {new AtomicLong(), new AtomicLong(), new AtomicLong()};
        final List<String> violations = new ArrayList<>();
        final long[] lastSeqs = new long[R];

        Thread[] threads = new Thread[R];
        for (int t = 0; t < R; t++) {
            final int idx = t;
            final ByteBuffer view = views[idx];
            threads[t] = new Thread(() -> {
                try {
                    long fresh = 0, sumDropped = 0, lastSeq = 0, torn = 0, claims = 0;
                    // Self-terminating: run until the final frame is observed.
                    while (lastSeq < FRAMES && claims < 100_000_000L) {
                        long seq = N.fanoutClaim(readers[idx]);
                        claims++;
                        if (N.fanoutClaimFresh(readers[idx])) {
                            fresh++;
                            sumDropped += N.fanoutClaimDropped(readers[idx]);
                            // FI1: every accepted frame's bytes must match pat.
                            for (int i = 0; i < PAYLOAD; i += 8) {
                                if ((view.get(i) & 0xFF) != pat((int) seq, i)) { torn++; break; }
                            }
                            if (seq <= lastSeq) { synchronized (violations) {
                                violations.add("reader " + idx + ": non-monotonic seq " + seq);
                            } }
                            lastSeq = seq;
                        }
                    }
                    if (lastSeq < FRAMES) { synchronized (violations) {
                        violations.add("reader " + idx + ": starved before frame " + FRAMES
                                + " (last=" + lastSeq + ")");
                    } }
                    statFresh[idx].set(fresh);
                    statDropped[idx].set(sumDropped);
                    lastSeqs[idx] = lastSeq;
                    if (torn > 0) { synchronized (violations) {
                        violations.add("reader " + idx + ": " + torn + " TORN accepted frames");
                    } }
                    // FI3 per reader: lastSeq - fresh == sum(dropped).
                    if (lastSeq - fresh != sumDropped) { synchronized (violations) {
                        violations.add("reader " + idx + ": telescoping violated (last=" + lastSeq
                                + " fresh=" + fresh + " dropped=" + sumDropped + ")");
                    } }
                } finally {
                    running.set(false);
                }
            }, "fanout-reader-" + idx);
            threads[t].setDaemon(true);
            threads[t].start();
        }

        long w0 = System.nanoTime();
        for (int seq = 1; seq <= FRAMES; seq++) {
            ByteBuffer cur = N.fanoutBegin(f);
            for (int i = 0; i < PAYLOAD; i++) cur.put(i, (byte) pat(seq, i));
            N.fanoutPublish(f);
        }
        long wMs = Math.max(1, (System.nanoTime() - w0) / 1_000_000);
        for (Thread th : threads) th.join(30_000);
        for (Thread th : threads) check(!th.isAlive(), "JF8 reader thread " + th.getName() + " terminated");

        check(violations.isEmpty(), "JF8 torture: zero protocol violations across "
                + FRAMES + " frames x " + R + " readers (violations: " + violations + ")");
        long totalFresh = 0;
        for (int i = 0; i < R; i++) {
            totalFresh += statFresh[i].get();
            check(N.fanoutReaderStatsReads(readers[i]) > 0,
                    "JF8 reader " + i + " stats populated (reads=" + N.fanoutReaderStatsReads(readers[i])
                            + " fresh=" + statFresh[i].get() + " dropped=" + statDropped[i].get()
                            + " skipped=" + N.fanoutReaderStatsSkipped(readers[i])
                            + " exhausted=" + N.fanoutReaderStatsExhausted(readers[i]) + ")");
            check(lastSeqs[i] == FRAMES, "JF8 reader " + i + " converged to the final frame");
            N.fanoutReaderDestroy(readers[i]);
        }
        check(totalFresh > 0, "JF8 readers observed frames (" + totalFresh + " fresh claims total)");
        System.out.println("       torture perf: ~" + (FRAMES / wMs) + "K publishes/s through the "
                + "JNI zero-copy cursor path (informative, host JVM)");
        N.fanoutDestroy(f);
    }
}
