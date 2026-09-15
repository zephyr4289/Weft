package dev.weft

/**
 * Bridge for native engines (Rust/C++ audio taps, physics solvers) to attach
 * to a [Weft] and publish frames at native rates.
 *
 * ## Usage from Rust
 *
 * ```rust
 * // Native side: hold the Weft's publish buffer address.
 * let ptr = unsafe { weft.buffer_ptr(weft.writer_idx()) };
 * // Write into ptr directly (no JNI round-trip per frame).
 * // Then call publish via the Triad Protocol.
 * unsafe { weft.publish(ptr) };
 * ```
 *
 * ## Usage from Kotlin (less hot paths)
 *
 * ```kotlin
 * val pcm = steward.weft<FloatArray>(1024)
 * NativeBridge.attach(pcm)  // writes to pcm.publishBuffer
 * // Native engine calls pcm.publish() when ready (or via the JNI path)
 * ```
 *
 * @spec v0.1 §4.3
 */
object NativeBridge {

    /**
     * Get the raw address (as a `Long`) of the Weft's writer buffer.
     * The native engine writes to this address directly, no JNI round-trip
     * per frame.
     *
     * Returns 0 if the Weft has been released.
     */
    fun writerAddress(weft: Weft<*>): Long {
        // The publishBuffer is a direct ByteBuffer; get its address via
        // JNI's GetDirectBufferAddress. We expose this through the JVM's
        // internal sun.misc.Unsafe or via a tiny JNI helper.
        // For simplicity in v0.1: rely on the ByteBuffer's native address
        // being stable; the native engine queries it once on attach.
        //
        // NOTE: This requires either:
        //   (a) A small JNI helper weftBufferAddress(weftHandle) -> jlong
        //       that returns the raw pointer (the engine passes it to Rust).
        //   (b) Using sun.misc.Unsafe (deprecated, may break in future JVMs).
        //
        // For v0.1 we ship (a). See TriadNative.weftWriterBuffer for the
        // ByteBuffer wrapper; this method returns the raw pointer.
        //
        // Pseudocode; the actual function would be a JNI entry
        //   Java_dev_weft_TriadNative_weftBufferAddress(weftHandle) -> jlong
        // that returns g.buffer_ptr(g.writer_idx()) as a jlong.
        TODO("Requires weftBufferAddress JNI entry; see TriadNative docs")
    }
}
