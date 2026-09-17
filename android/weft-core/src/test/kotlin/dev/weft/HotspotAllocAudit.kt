package dev.weft

internal object HotspotAllocAudit {
    private val pair by lazy {
        try {
            val factoryClass = Class.forName("java.lang.management.ManagementFactory")
            val getBeanMethod = factoryClass.getMethod("getThreadMXBean")
            val bean = getBeanMethod.invoke(null)
            val isSupportedMethod = bean.javaClass.getMethod("isThreadAllocatedMemorySupported")
            if (isSupportedMethod.invoke(bean) == true) {
                val allocMethod = bean.javaClass.getMethod("getThreadAllocatedBytes", Long::class.javaPrimitiveType)
                Pair(bean, allocMethod)
            } else null
        } catch (_: Throwable) {
            null
        }
    }

    val isAvailable: Boolean get() = pair != null

    fun getThreadAllocatedBytes(threadId: Long): Long {
        val p = pair ?: return -1L
        return p.second.invoke(p.first, threadId) as Long
    }
}
