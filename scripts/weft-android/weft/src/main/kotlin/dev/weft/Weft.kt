package dev.weft

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * A continuous-state channel: three off-heap buffers + the Triad Protocol.
 *
 * Hot state flows through this; cold state stays in Compose `MutableState`.
 * See spec §4.
 *
 * ## Writer (native engine)
 *
 * The native engine writes to [publishBuffer], a direct `ByteBuffer` that
 * shares its address with the writer's working buffer in native memory. After
 * writing, the engine calls [publish] (or the native publish function directly)
 * to atomically swap the buffer into the "latest" slot via the Triad Protocol.
 *
 * ## Reader (Draw phase)
 *
 * The Heddle reads via [read] on every VSYNC. The buffer is snapshot into
 * [readBuffer], a pre-allocated direct ByteBuffer; no allocation occurs.
 *
 * ## Type parameter
 *
 * [T] is the JVM-side array type: `FloatArray`, `IntArray`, etc. Used only for
 * the public API; the native side sees raw bytes.
 *
 * @spec v0.1 §4, §5
 */
class Weft<T> internal constructor(
    private val steward: Steward,
    private val id: WeftId,
    val elemSize: Int,
    val capacity: Int,
) {
    /**
     * Byte size of one buffer (`capacity * elemSize`).
     */
    val byteSize: Int = capacity * elemSize

    /**
     * The writer's working buffer, as a direct ByteBuffer. The native engine
     * writes here; [publish] swaps it into "latest".
     *
     * This buffer is owned by the Weft. Do not retain a reference past the
     * Weft's release.
     */
    val publishBuffer: ByteBuffer by lazy {
        val buf = TriadNative.weftWriterBuffer(id.value)
            ?: error("weftWriterBuffer returned null (already released?)")
        buf.order(ByteOrder.LITTLE_ENDIAN)
        buf
    }

    /**
     * The reader's snapshot buffer. Pre-allocated on first access; reused for
     * every read. Zero allocation per frame after warmup.
     */
    val readBuffer: ByteBuffer by lazy {
        val buf = ByteBuffer.allocateDirect(byteSize)
        buf.order(ByteOrder.LITTLE_ENDIAN)
        buf
    }

    /**
     * Publish a frame. Copies [publishBuffer]'s contents into the "latest" slot
     * via the Triad Protocol. Wait-free, O(1).
     *
     * The native engine typically calls the JNI function directly
     * (`Java_dev_weft_TriadNative_weftPublish`) for sub-microsecond latency;
     * this Kotlin wrapper is for less hot paths.
     */
    fun publish() {
        TriadNative.weftPublish(id.value, publishBuffer)
    }

    /**
     * Read the latest frame into [readBuffer]. Returns true if a new frame was
     * read; false if no new data since the last read (latest unchanged or empty).
     *
     * Wait-free: single Acquire load + single CAS + single Release store.
     */
    fun read(): Boolean {
        return TriadNative.weftRead(id.value, readBuffer)
    }

    /**
     * Release this Weft. Frees the native memory. After release, [read] returns
     * false and [publish] is a no-op.
     */
    fun release() = steward.release(id)

    /** Telemetry: number of frames published since creation. */
    val publishCount: Long
        get() = TriadNative.weftPublishCount(id.value)

    /** Telemetry: number of frames read since creation. */
    val readCount: Long
        get() = TriadNative.weftReadCount(id.value)

    override fun toString(): String =
        "Weft(id=${id.value}, elemSize=$elemSize, capacity=$capacity, byteSize=$byteSize)"

    companion object {
        /**
         * Determine the element size (in bytes) for the given JVM array type.
         * Used by the Steward when allocating.
         */
        @Suppress("UNCHECKED_CAST")
        internal inline fun <reified T> elemSizeForType(): Int {
            // Reified T at compile time; the Steward allocates the right
            // native buffer size.
            return when (T::class) {
                FloatArray::class -> 4
                IntArray::class -> 4
                ShortArray::class -> 2
                ByteArray::class -> 1
                DoubleArray::class -> 8
                LongArray::class -> 8
                else -> throw IllegalArgumentException(
                    "Weft<T> supports FloatArray, IntArray, ShortArray, ByteArray, DoubleArray, LongArray; got ${T::class}"
                )
            }
        }
    }
}
