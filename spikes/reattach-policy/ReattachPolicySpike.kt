/**
 * spikes/reattach-policy/ReattachPolicySpike.kt
 * Spike and Decision Model for RFC 0006: Android Process-Death ReattachPolicy
 *
 * Models the memory re-hydration vs clean re-allocation state machine
 * during Android Activity/Service process rebirth.
 */

enum class ProcessDeathState {
    RUNNING,
    KILLED_BACKGROUND,
    RECREATED_FRESH_PROCESS
}

enum class ReattachStrategy {
    CLEAN_REALLOCATE,   // Safest: create fresh ring, reset seq to 1
    REHYDRATE_PERSISTED // Advanced: attach to existing POSIX shm / ashmem if valid
}

data class ReattachResult(
    val strategy: ReattachStrategy,
    val handlesRecovered: Int,
    val droppedFrames: Long,
    val success: Boolean,
    val requiresHardwareTest: Boolean
)

class ReattachPolicyEvaluator {
    fun evaluateProcessRebirth(
        hasSharedMemoryRegion: Boolean,
        shmHeaderValid: Boolean,
        hardwareAvailable: Boolean
    ): ReattachResult {
        return if (hasSharedMemoryRegion && shmHeaderValid && hardwareAvailable) {
            ReattachResult(
                strategy = ReattachStrategy.REHYDRATE_PERSISTED,
                handlesRecovered = 3,
                droppedFrames = 120, // estimated offline frames during rebirth
                success = true,
                requiresHardwareTest = true
            )
        } else {
            // Default safe fallback without hardware AHardwareBuffer/ashmem
            ReattachResult(
                strategy = ReattachStrategy.CLEAN_REALLOCATE,
                handlesRecovered = 0,
                droppedFrames = 0,
                success = true,
                requiresHardwareTest = false
            )
        }
    }
}

fun main() {
    println("=== RFC 0006: ReattachPolicy Spike Simulation ===")
    println("Environment Tag: jvm-simulation / linux-sandbox")
    val evaluator = ReattachPolicyEvaluator()

    val resClean = evaluator.evaluateProcessRebirth(
        hasSharedMemoryRegion = false,
        shmHeaderValid = false,
        hardwareAvailable = false
    )
    println("Scenario 1 (No physical device / default fallback):")
    println("  Strategy: ${resClean.strategy} | Recovered: ${resClean.handlesRecovered} | Success: ${resClean.success}")

    val resHw = evaluator.evaluateProcessRebirth(
        hasSharedMemoryRegion = true,
        shmHeaderValid = true,
        hardwareAvailable = true
    )
    println("Scenario 2 (Hardware AHardwareBuffer / POSIX shm available):")
    println("  Strategy: ${resHw.strategy} | Recovered: ${resHw.handlesRecovered} | Hardware Deferred: ${resHw.requiresHardwareTest}")
}
