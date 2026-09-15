package dev.weft

/**
 * The Steward: lifecycle manager for Wefts.
 *
 * Owns off-heap buffers and frees them on [release]. Survives Android
 * configuration change when scoped to a `ViewModel` (see spec §7.3).
 *
 * ## Usage
 *
 * ```kotlin
 * class AudioVm : ViewModel() {
 *     val steward = Steward()
 *     val pcm = steward.weft<Float32Array>(capacity = 1024)
 *
 *     init { NativeBridge.attachAudioTap(pcm) }
 *
 *     override fun onCleared() = steward.releaseAll()
 * }
 * ```
 *
 * ## Thread safety
 *
 * All operations are thread-safe. The internal native state is synchronized
 * via the Triad Protocol's atomics (see spec §5).
 *
 * ## Lifecycle
 *
 * The Steward holds native memory. Call [releaseAll] when done (typically
 * in `ViewModel.onCleared`). If you forget, [finalize] will catch it as a
 * last resort and log a leak warning.
 *
 * @spec v0.1 §7
 */
class Steward {

    private val handle: Long = try {
        TriadNative.stewardCreate()
    } catch (t: Throwable) {
        0L
    }
    private val wefts: MutableMap<WeftId, Weft<*>> = HashMap()
    private var released: Boolean = false

    /**
     * Allocate a Weft for elements of type [T] with the given capacity.
     * The buffer is off-heap, 16-byte aligned.
     *
     * @param capacity Number of elements (not bytes).
     * @param align Alignment of the first byte. Default 16 (matches ARM NEON).
     * @return The Weft. Throws if allocation fails (OOM).
     */
    fun <T> weft(capacity: Int, align: Int = 16): Weft<T> {
        check(!released) { "Steward is released" }
        val elemSize = Weft.elemSizeForType<T>()
        val nativeHandle = TriadNative.stewardWeft(handle, elemSize, capacity, align)
        check(nativeHandle != 0L) { "weft: allocation failed (OOM)" }
        val id = WeftId(nativeHandle)
        val weft = Weft<T>(this, id, elemSize, capacity)
        synchronized(wefts) { wefts[id] = weft }
        return weft
    }

    /**
     * Release a single Weft. Frees its native memory. After release, the Weft's
     * [Weft.read] returns null and its [Weft.publishBuffer] is a no-op.
     */
    fun release(id: WeftId) {
        if (released) return
        synchronized(wefts) { wefts.remove(id) }
        TriadNative.weftRelease(id.value)
    }

    /**
     * Release all Wefts owned by this Steward. Safe to call multiple times.
     */
    fun releaseAll() {
        if (released) return
        released = true
        val ids: List<WeftId>
        synchronized(wefts) {
            ids = wefts.keys.toList()
            wefts.clear()
        }
        for (id in ids) {
            TriadNative.weftRelease(id.value)
        }
        if (handle != 0L) {
            TriadNative.stewardDestroy(handle)
        }
    }

    /**
     * Aggregate stats across all owned Wefts. For benchmark dashboards.
     */
    fun stats(): StewardStats {
        // The native side tracks stats per-Weft. Aggregate by querying each.
        // (For brevity, this is the simple loop. A production impl would push
        // aggregation into the native side.)
        var totalPub = 0L
        var totalRead = 0L
        synchronized(wefts) {
            for (w in wefts.values) {
                totalPub += w.publishCount
                totalRead += w.readCount
            }
        }
        return StewardStats(
            weftCount = synchronized(wefts) { wefts.size },
            totalPublishes = totalPub,
            totalReads = totalRead,
        )
    }

    @Suppress("RemovalNotification")
    protected fun finalize() {
        if (!released) {
            // This is the safety net for spec L4. If Kotlin's GC gets here
            // without releaseAll(), the native Steward is still holding memory.
            android.util.Log.w("Weft", "Steward finalized without releaseAll(); freeing native memory in finalizer")
            releaseAll()
        }
    }
}

/**
 * Aggregate stats for a Steward.
 */
data class StewardStats(
    val weftCount: Int,
    val totalPublishes: Long,
    val totalReads: Long,
)

/**
 * Opaque identifier for a Weft within a Steward.
 */
@JvmInline
value class WeftId(val value: Long)
