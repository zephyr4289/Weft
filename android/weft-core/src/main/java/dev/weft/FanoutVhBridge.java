package dev.weft;

import java.lang.invoke.MethodHandle;
import java.nio.ByteBuffer;

/**
 * FanoutVhBridge — the signature-polymorphic bridge for the fan-out ring's
 * VarHandle accessors (Series 7, the 0-GC drawing-loop work).
 *
 * WHY A JAVA FILE IN THE KOTLIN PORT: Kotlin cannot emit signature-
 * polymorphic {@code MethodHandle.invokeExact} call sites (KT-20871) —
 * every Kotlin call was going through {@code invokeWithArguments}, which
 * BOXES each primitive argument and allocates an Object[] per invocation.
 * On the reader's claim path that is ~2 boxed longs + ~words boxed ints +
 * words arrays PER CLAIM (~4.6 KB/claim at 64 words — measured by the C5
 * allocated-bytes audit) — a hidden per-frame allocation stream the
 * drawing loop must not have (Law 2). javac compiles invokeExact with
 * exact static types: zero boxing, and the JIT inlines the full access.
 *
 * The MethodHandles are shaped ONCE in FanoutVh
 * ({@code VarHandle.toMethodHandle(mode)} — type {@code (ByteBuffer,int)long}
 * / {@code (ByteBuffer,int,int)void}); the bridge invokes them EXACTLY.
 * Same access modes, same ordering semantics — only the invocation
 * mechanics change (the F-series battery pins the semantics).
 */
final class FanoutVhBridge {

    private FanoutVhBridge() {}

    /** getAcquire(long view): exact invoke — no boxing. */
    static long getAcquireLong(MethodHandle mh, ByteBuffer buf, int byteOffset) {
        try {
            return (long) mh.invokeExact(buf, byteOffset);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException("fanout vh bridge", t);
        }
    }

    /** setRelease(long view): exact invoke — no boxing. */
    static void setReleaseLong(MethodHandle mh, ByteBuffer buf, int byteOffset, long v) {
        try {
            mh.invokeExact(buf, byteOffset, v);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException("fanout vh bridge", t);
        }
    }

    /** setVolatile(long view): exact invoke — no boxing. */
    static void setVolatileLong(MethodHandle mh, ByteBuffer buf, int byteOffset, long v) {
        try {
            mh.invokeExact(buf, byteOffset, v);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException("fanout vh bridge", t);
        }
    }

    /** getAndAdd(long view): exact invoke — no boxing. */
    static long getAndAddLong(MethodHandle mh, ByteBuffer buf, int byteOffset, long delta) {
        try {
            return (long) mh.invokeExact(buf, byteOffset, delta);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException("fanout vh bridge", t);
        }
    }

    /** getOpaque(int view): exact invoke — no boxing. */
    static int getOpaqueInt(MethodHandle mh, ByteBuffer buf, int byteOffset) {
        try {
            return (int) mh.invokeExact(buf, byteOffset);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException("fanout vh bridge", t);
        }
    }

    /** setOpaque(int view): exact invoke — no boxing. */
    static void setOpaqueInt(MethodHandle mh, ByteBuffer buf, int byteOffset, int v) {
        try {
            mh.invokeExact(buf, byteOffset, v);
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException("fanout vh bridge", t);
        }
    }
}
