// RecyclerTests.swift — RFC-0009 Series 7: 0-GC buffer recycler +
// memory-pressure backstop conformance suite, Swift.
//
// The RecyclerTest.kt / recycler_test.dart twin: pool discipline, bounded
// steady state, the trim backstop's LIVE-slot safety, realloc accounting,
// the level mapping, the center fan-out. Swift has no portable allocation
// counter (the per-port honesty wall — the JVM's allocated-bytes audit is
// the Kotlin leg's proof): this battery pins the IDENTITY discipline (a
// released-then-reacquired slot is the SAME array via ===) and the exact
// counter equations; instruments-on-device is the deferred road.
//
// Environment tag: XCTest on macOS/ARM (apple-packages CI); NOT runnable
// in the x86_64 Linux sandbox (no Swift toolchain there) — declared, per
// the repo's per-port honesty culture (PORTS.md §9).

import XCTest
@testable import WeftCore

final class RecyclerTests: XCTestCase {

    // --- R1: pool reuse + counter discipline ---

    func testR1PoolReuseIdentityAndCounters() {
        let pool = WeftBufferRecycler(slotBytes: 256, maxFreeSlots: 2)
        XCTAssertEqual(pool.pooledNow, 0)
        XCTAssertEqual(pool.liveNow, 0)

        var a = pool.acquire()
        XCTAssertEqual(a.count, 256)
        XCTAssertEqual(pool.liveNow, 1)
        XCTAssertEqual(pool.pooledNow, 0)
        XCTAssertEqual(pool.reallocs, 0) // first fill is not a realloc
        a[0] = 0x5A

        XCTAssertTrue(pool.release(a))
        XCTAssertEqual(pool.liveNow, 0)
        XCTAssertEqual(pool.pooledNow, 1)

        // Reuse: the SAME slot identity comes back (zero allocation).
        let b = pool.acquire()
        XCTAssertEqual(b.count, 256)
        XCTAssertEqual(b[0], 0x5A, "pooled slot contents reused")
        XCTAssertEqual(pool.acquires, 2)
        XCTAssertEqual(pool.releases, 1)
        XCTAssertEqual(pool.reallocs, 0)
    }

    // --- R2: bounded steady state (excess releases drop) ---

    func testR2MaxFreeSlotsBoundsSteadyState() {
        let pool = WeftBufferRecycler(slotBytes: 64, maxFreeSlots: 2)
        let x = pool.acquire(); let y = pool.acquire(); let z = pool.acquire()
        XCTAssertTrue(pool.release(x))
        XCTAssertTrue(pool.release(y))
        XCTAssertFalse(pool.release(z), "pool full — z drops")
        XCTAssertEqual(pool.pooledNow, 2)
        XCTAssertEqual(pool.liveNow, 0)
    }

    // --- R3: the backstop touches FREE slots only, never LIVE ---

    func testR3TrimDropsOnlyFreeSlotsLiveSurvivesAndNextAcquireReallocates() {
        let pool = WeftBufferRecycler(slotBytes: 128, maxFreeSlots: 2)
        var live = pool.acquire()          // LIVE: mid-frame raster buffer
        var free1 = pool.acquire()          // two DISTINCT slots pool up
        let free2 = pool.acquire()
        free1[0] = 0x7E
        pool.release(free1)
        pool.release(free2)
        XCTAssertEqual(pool.pooledNow, 2)

        pool.trim(0)
        XCTAssertEqual(pool.pooledNow, 0)
        XCTAssertEqual(pool.trimmedSlots, 2)
        XCTAssertEqual(pool.trims, 1)
        XCTAssertEqual(pool.liveNow, 1, "live slot untouched by the backstop")

        // The LIVE slot is still perfectly usable (no frame dropped).
        live[0] = 0xAB
        XCTAssertEqual(live[0], 0xAB)

        // Next acquire: fresh allocation, COUNTED (pressure's visible cost).
        let fresh = pool.acquire()
        XCTAssertEqual(fresh.count, 128)
        XCTAssertEqual(fresh[0], 0)
        XCTAssertEqual(pool.reallocs, 1)
    }

    // --- R4: realloc accounting (first fill != realloc; keep-all trims
    //         do not mark the pool as trimmed) ---

    func testR4ReallocAccounting() {
        let pool = WeftBufferRecycler(slotBytes: 32, maxFreeSlots: 4)
        for _ in 0..<4 { _ = pool.acquire() }
        XCTAssertEqual(pool.reallocs, 0)
        XCTAssertEqual(pool.liveNow, 4)

        // UI_HIDDEN-class with an empty free list: NOT an effective trim.
        pool.onLowMemory(level: TrimLevel.uiHidden)
        XCTAssertEqual(pool.trims, 1)
        XCTAssertEqual(pool.trimmedSlots, 0)

        // Release all, then a keep-all trim again — still not effective.
        for _ in 0..<4 { _ = pool.release([UInt8](repeating: 0, count: 32)) }
        XCTAssertEqual(pool.pooledNow, 4)
        pool.onLowMemory(level: TrimLevel.uiHidden)
        XCTAssertEqual(pool.pooledNow, 4)
        XCTAssertEqual(pool.trimmedSlots, 0)
        XCTAssertEqual(pool.reallocs, 0)

        // COMPLETE: drops all four; the next four acquires are reallocs.
        pool.onLowMemory(level: TrimLevel.complete)
        XCTAssertEqual(pool.pooledNow, 0)
        XCTAssertEqual(pool.trimmedSlots, 4)
        for _ in 0..<4 { _ = pool.acquire() }
        XCTAssertEqual(pool.reallocs, 4)
    }

    // --- R5: the documented level mapping ---

    func testR5LevelMapping() {
        func keepAfter(_ level: Int) -> Int {
            let p = WeftBufferRecycler(slotBytes: 16, maxFreeSlots: 4)
            let slots = (0..<4).map { _ in p.acquire() } // FOUR distinct
            slots.forEach { _ = p.release($0) }
            XCTAssertEqual(p.pooledNow, 4)
            p.onLowMemory(level: level)
            return p.pooledNow
        }
        XCTAssertEqual(0, keepAfter(TrimLevel.complete))
        XCTAssertEqual(0, keepAfter(TrimLevel.runningCritical))
        XCTAssertEqual(2, keepAfter(TrimLevel.moderate))
        XCTAssertEqual(2, keepAfter(TrimLevel.background))
        XCTAssertEqual(2, keepAfter(TrimLevel.runningLow))
        XCTAssertEqual(4, keepAfter(TrimLevel.uiHidden))
        XCTAssertEqual(4, keepAfter(TrimLevel.runningModerate))
        XCTAssertEqual(4, keepAfter(0)) // unknown -> conservative keep
    }

    // --- R6: the center fan-out ---

    func testR6CenterFanOutAndUnregister() {
        let a = WeftBufferRecycler(slotBytes: 8, maxFreeSlots: 2)
        let b = WeftBufferRecycler(slotBytes: 8, maxFreeSlots: 2)
        _ = a.release(a.acquire())
        _ = b.release(b.acquire())
        let center = WeftMemoryPressureCenter.shared
        let n0 = center.registered
        center.register(a)
        center.register(b)
        XCTAssertEqual(n0 + 2, center.registered)

        center.handleMemoryWarning() // hard event == COMPLETE class
        XCTAssertEqual(a.pooledNow, 0)
        XCTAssertEqual(b.pooledNow, 0)
        XCTAssertEqual(a.trims, 1)
        XCTAssertEqual(b.trims, 1)

        center.unregister(a)
        center.unregister(b)
        XCTAssertEqual(n0, center.registered)
    }
}
