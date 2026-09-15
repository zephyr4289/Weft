package dev.weft

import java.nio.ByteBuffer

/**
 * The JNI surface. Every function corresponds to a panic-shielded `#[no_mangle]`
 * entry point in `rust/src/jni_bridge.rs`.
 *
 * ## Panic safety
 *
 * Every function is panic-shielded on the native side (see `shield()` in
 * `jni_bridge.rs`). A native panic returns a sentinel (0 / false / null) and
 * logs to logcat. The JVM does not crash.
 *
 * ## Naming convention
 *
 * Functions are named `dev_weft_TriadNative_<name>` on the native side, where
 * `<name>` matches the Kotlin function name here. The `_` underscores in
 * `dev_weft` are the JNI name mangling for the dotted package path
 * `dev.weft.TriadNative`.
 *
 * @spec v0.1 §7.4
 */
internal object TriadNative {

    // --- Steward lifecycle ---

    /** Create a Steward. Returns a `jlong` handle, or 0 on failure. */
    external fun stewardCreate(): Long

    /** Destroy a Steward by handle. Frees all owned Wefts. */
    external fun stewardDestroy(handle: Long)

    /**
     * Allocate a Weft for elements of [elemSize] bytes with the given [capacity]
     * and [align]. Returns a `jlong` Weft handle, or 0 on failure.
     */
    external fun stewardWeft(
        stewardHandle: Long, elemSize: Int, capacity: Int, align: Int,
    ): Long

    // --- Weft operations ---

    /** Release a Weft by handle. */
    external fun weftRelease(weftHandle: Long)

    /**
     * Get the writer's working buffer as a direct [ByteBuffer].
     *
     * The returned ByteBuffer is backed by native memory owned by the Weft.
     * Do not retain past the Weft's lifetime.
     */
    external fun weftWriterBuffer(weftHandle: Long): ByteBuffer?

    /**
     * Publish a frame. Copies [data]'s contents into the writer's working
     * buffer, then atomically publishes it via the Triad Protocol.
     *
     * [data] must be a direct ByteBuffer of size `buffer_byte_size()`.
     */
    external fun weftPublish(weftHandle: Long, data: ByteBuffer)

    /**
     * Read the latest frame into [out]. Returns true if a frame was read;
     * false if no new data.
     *
     * [out] must be a direct ByteBuffer of size `buffer_byte_size()`.
     */
    external fun weftRead(weftHandle: Long, out: ByteBuffer): Boolean

    // --- Telemetry ---

    /** Number of frames published since the Weft was created. */
    external fun weftPublishCount(weftHandle: Long): Long

    /** Number of frames read since the Weft was created. */
    external fun weftReadCount(weftHandle: Long): Long

    companion object {
        init {
            // Load the native library. The .so must be packaged in the APK
            // under jniLibs/{arm64-v8a,armeabi-v7a}/libweft_core.so.
            System.loadLibrary("weft_core")
        }
    }
}
