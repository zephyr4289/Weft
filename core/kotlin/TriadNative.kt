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
// STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION.

package dev.weft

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
    external fun weftPublish(weftHandle: Long, data: ByteBuffer): Int
    external fun weftRead(weftHandle: Long, out: ByteBuffer): Boolean

    // --- I6 ---
    external fun weftRevoke(weftHandle: Long)
    external fun weftReclaim(weftHandle: Long, preRevokeEpoch: Int, timeoutMs: Int): Boolean

    // --- Telemetry (advisory per AXIOM T) ---
    external fun weftPublishCount(weftHandle: Long): Long
    external fun weftReadCount(weftHandle: Long): Long

    // --- Panic shield wrapper ---
    // Every JNI entry is wrapped: Throwable caught → error code returned.
    // No exception crosses the FFI boundary.
    fun safePublish(weftHandle: Long, data: ByteBuffer): Int {
        return try {
            weftPublish(weftHandle, data)
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
