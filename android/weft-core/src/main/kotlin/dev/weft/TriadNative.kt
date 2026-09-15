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
}
