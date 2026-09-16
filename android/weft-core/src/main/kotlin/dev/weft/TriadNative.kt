// TriadNative.kt — JNI bridge to the litmus-passing C kernel (Kotlin)
//
// WHY EXISTS: Links the Kotlin Steward to the C kernel via JNI for production
// use. Per 02-KERNEL §4: the public API contract. Per WO-P4 decision 1:
// exchange site must map to a single RMW — the C kernel's
// atomic_exchange_explicit(latest, w_work, memory_order_acq_rel) is the
// single RMW; the JNI bridge calls through to it. Every JNI entry is
// panic-shielded (catches Throwable, returns error code). Per I6:
// buffer ownership documented against the caller contract (after ACK,
// the buffer is not yours).
//
// PUBLISH CONTRACT (mirrors weft_jni.c): pass (data, payloadLen, seq).
//   data != null — the buffer's CONTENTS are the frame; exactly payloadLen
//                  bytes are copied into the kernel's writer buffer.
//   data == null — cursor mode: you filled the buffer from weftWriterBuffer().
//   seq < 0      — auto-numbering (t_publish + 1).
//
// All teardown paths (weftRelease / stewardDestroy) run the I6 handshake
// (revoke → bounded epoch-ACK wait → destroy) inside the bridge, so Kotlin
// callers cannot skip it.
//
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

package dev.weft

import java.nio.ByteBuffer

internal object TriadNative {
    init {
        try {
            System.loadLibrary("weft_core")
        } catch (t: Throwable) {
            // Panic shielded: graceful fallback if native library not loaded in JVM host environment
        }
    }

    // --- Steward lifecycle ---
    external fun stewardCreate(): Long
    external fun stewardDestroy(handle: Long)

    // --- Weft lifecycle ---
    external fun stewardWeft(
        stewardHandle: Long, elemSize: Int, capacity: Int, align: Int
    ): Long
    external fun weftRelease(weftHandle: Long)

    // --- Weft operations ---
    external fun weftWriterBuffer(weftHandle: Long): ByteBuffer?
    external fun weftPublish(weftHandle: Long, data: ByteBuffer?, payloadLen: Int, seq: Int): Int
    external fun weftRead(weftHandle: Long, out: ByteBuffer): Boolean

    // --- I6 ---
    external fun weftRevoke(weftHandle: Long)
    external fun weftReclaim(weftHandle: Long, preRevokeEpoch: Int, timeoutMs: Int): Boolean

    // --- Telemetry (advisory per AXIOM T) ---
    external fun weftPublishCount(weftHandle: Long): Long
    external fun weftReadCount(weftHandle: Long): Long

    // --- Fan-out ring (RFC 0004) — binds core/c/fanout.{h,c} via weft_jni.c ---
    // The C ring is BYTE-COMPATIBLE with core/ts/fanout.ts, so a ring
    // produced by any port is consumable here and vice versa. Pure-JVM
    // semantics (no native library) live in Fanout.kt — these entries are
    // the production native path, the same split as Weft.kt vs the kernel
    // entries above.
    external fun fanoutCreate(payloadBytes: Int, slotCount: Int): Long
    external fun fanoutCreateForeign(ringBuf: ByteBuffer, payloadBytes: Int, slotCount: Int): Long
    external fun fanoutRingBytes(payloadBytes: Int, slotCount: Int): Long
    external fun fanoutDestroy(fanoutHandle: Long)
    external fun fanoutBegin(fanoutHandle: Long): ByteBuffer?
    external fun fanoutFill(fanoutHandle: Long, srcBuf: ByteBuffer, len: Int): Int
    external fun fanoutPublish(fanoutHandle: Long): Long
    external fun fanoutLatestSeq(fanoutHandle: Long): Long
    external fun fanoutPublishes(fanoutHandle: Long): Long
    external fun fanoutReaderCreate(fanoutHandle: Long): Long
    external fun fanoutReaderCreateForeign(ringBuf: ByteBuffer, payloadBytes: Int, slotCount: Int): Long
    external fun fanoutReaderDestroy(readerHandle: Long)
    external fun fanoutClaim(readerHandle: Long): Long
    external fun fanoutClaimFresh(readerHandle: Long): Boolean
    external fun fanoutClaimDropped(readerHandle: Long): Long
    external fun fanoutViewBuffer(readerHandle: Long): ByteBuffer?
    external fun fanoutReaderStatsReads(readerHandle: Long): Long
    external fun fanoutReaderStatsFresh(readerHandle: Long): Long
    external fun fanoutReaderStatsDrops(readerHandle: Long): Long
    external fun fanoutReaderStatsSkipped(readerHandle: Long): Long
    external fun fanoutReaderStatsExhausted(readerHandle: Long): Long

    // --- Panic shield wrappers ---
    // Every JNI entry is wrapped: Throwable caught → error code returned.
    // No exception crosses the FFI boundary.
    fun safePublish(weftHandle: Long, data: ByteBuffer?, payloadLen: Int, seq: Int): Int {
        return try {
            weftPublish(weftHandle, data, payloadLen, seq)
        } catch (t: Throwable) {
            System.err.println("Weft: JNI publish panic: ${t.message}")
            -1 // error code
        }
    }

    fun safeRead(weftHandle: Long, out: ByteBuffer): Boolean {
        return try {
            weftRead(weftHandle, out)
        } catch (t: Throwable) {
            System.err.println("Weft: JNI read panic: ${t.message}")
            false
        }
    }

    // Fan-out panic shields (handle 0 / -1 / false = failure, never an
    // exception crossing the boundary). The claim record is reader-owned
    // and stable until that reader's next claim, so claim()/claimFresh()/
    // claimDropped() read back one consistent claim across three calls.
    fun safeFanoutCreate(payloadBytes: Int, slotCount: Int): Long {
        return try {
            fanoutCreate(payloadBytes, slotCount)
        } catch (t: Throwable) {
            System.err.println("Weft: JNI fanoutCreate panic: ${t.message}")
            0L
        }
    }

    fun safeFanoutReaderCreate(fanoutHandle: Long): Long {
        return try {
            fanoutReaderCreate(fanoutHandle)
        } catch (t: Throwable) {
            System.err.println("Weft: JNI fanoutReaderCreate panic: ${t.message}")
            0L
        }
    }

    fun safeFanoutBegin(fanoutHandle: Long): ByteBuffer? {
        return try {
            fanoutBegin(fanoutHandle)
        } catch (t: Throwable) {
            System.err.println("Weft: JNI fanoutBegin panic: ${t.message}")
            null
        }
    }

    fun safeFanoutPublish(fanoutHandle: Long): Long {
        return try {
            fanoutPublish(fanoutHandle)
        } catch (t: Throwable) {
            System.err.println("Weft: JNI fanoutPublish panic: ${t.message}")
            0L
        }
    }

    fun safeFanoutClaim(readerHandle: Long): Long {
        return try {
            fanoutClaim(readerHandle)
        } catch (t: Throwable) {
            System.err.println("Weft: JNI fanoutClaim panic: ${t.message}")
            0L
        }
    }
}
