// SpectrumWireTests.swift — Swift lane: frozen constants, CRC vectors,
// golden record decode, governor vector checkpoints.
//
// The golden fixture bytes are embedded (byte-frozen projections of
// tests/spectrum/managed/fixtures/hw-profile-flagship.bin — the CI parity
// stage cross-checks TS/Python decodes of the SAME fixture against
// expected_profile.json; this suite pins the Swift projection).

import XCTest
@testable import WeftSpectrum

final class SpectrumWireTests: XCTestCase {
    func testFrozenConstants() {
        XCTAssertEqual(shp1RecordSize, 192)
        XCTAssertEqual(shp1CrcOffset, 188)
        XCTAssertEqual(shp1LayoutVersion, 1)
        XCTAssertEqual(cadenceLadder, [240, 120, 60, 30])
        XCTAssertEqual(sustainedTicks, 10)
        XCTAssertEqual(recoveryTicks, 50)
        XCTAssertEqual(backgroundCap, 30)
        XCTAssertEqual(lowBatteryPermille, 150)
        XCTAssertEqual(maxTierStages, 2)
    }

    func testCrcReferenceVectors() {
        XCTAssertEqual(crc32Shp1([]), 0x00000000)
        XCTAssertEqual(crc32Shp1(Array("123456789".utf8)), 0xCBF43926)
    }

    func testErrorTaxonomyComplete() {
        XCTAssertEqual(errorName(4), "E_CRC_MISMATCH")
        XCTAssertEqual(errorName(15), "E_HUD_RECOVERED")
        XCTAssertEqual(errorName(99), "E_UNKNOWN_99")
    }

    func testGoldenFlagshipDecode() {
        // byte-frozen flagship fixture (parity-verified projection)
        var record = [UInt8](repeating: 0, count: 192)
        record[0] = 0x53; record[1] = 0x48; record[2] = 0x50; record[3] = 0x31 // SHP1
        record[4] = 0x01 // version 1 LE
        record[6] = 0xC0; record[7] = 0x00 // record size 192 LE
        // feature flags lo (identical to committed flagship fixture):
        // WASM_SIMD128(0)+SHARED_ARRAY_BUFFER(1)+WEBGPU(2)+WEBGL2(3)+NEON(7)
        // +SVE2(8)+METAL_3(10)+APPLE_MPS(12)+MULTILANE_DMA(16)+BIG_LITTLE(17)
        // +THERMAL_SENSOR(18)+DLPACK_EXPORT(19) = 0x000F158F
        let flags: UInt32 = 0x000F_158F
        withUnsafeBytes(of: flags.littleEndian) { b in
            for (i, byte) in b.enumerated() { record[8 + i] = byte }
        }
        record[16] = 0x01 // tier 1
        // clock 4500000 LE @40
        withUnsafeBytes(of: UInt64(4_500_000).littleEndian) { b in
            for (i, byte) in b.enumerated() { record[40 + i] = byte }
        }
        // budget 8589934592 LE @56
        withUnsafeBytes(of: UInt64(8_589_934_592).littleEndian) { b in
            for (i, byte) in b.enumerated() { record[56 + i] = byte }
        }
        // 240000 mHz @72
        withUnsafeBytes(of: UInt64(240_000).littleEndian) { b in
            for (i, byte) in b.enumerated() { record[72 + i] = byte }
        }
        record[80] = 0xE8; record[81] = 0x03 // battery 1000
        record[84] = 0x01 // charging
        record[92] = 0x04 // dma lanes
        // CRC over [0,188) via the SAME table arithmetic
        var crc: UInt32 = 0xFFFFFFFF
        for i in 0..<188 {
            let idx = Int((crc ^ UInt32(record[i])) & 0xFF)
            crc = crcTableProxy()[idx] ^ (crc >> 8)
        }
        crc ^= 0xFFFFFFFF
        withUnsafeBytes(of: crc.littleEndian) { b in
            for (i, byte) in b.enumerated() { record[188 + i] = byte }
        }

        let view = ProfileView(record)
        XCTAssertEqual(view.validate(), 0)
        let fw = ProfileFlyweight()
        view.snapshot(into: fw)
        XCTAssertEqual(fw.featureFlagsLo, 0x000F_158F, "flag projection parity")
        XCTAssertEqual(fw.siliconTier, tierFlagship)
        XCTAssertEqual(fw.cpuMaxClockKhz, 4_500_000)
        XCTAssertEqual(fw.memoryBudgetBytes, 8_589_934_592)
        XCTAssertEqual(fw.maxFrameRateMilliHz, 240_000)
        XCTAssertTrue(fw.hasFeatureBit(featNeon))
        XCTAssertTrue(fw.hasFeatureBit(featMetal3))
        XCTAssertFalse(fw.hasFeatureBit(featCuda))
    }

    func testReservedDirtyFailsClosed() {
        var record = [UInt8](repeating: 0, count: 192)
        record[0] = 0x53; record[1] = 0x48; record[2] = 0x50; record[3] = 0x31
        record[4] = 0x01
        record[6] = 0xC0; record[7] = 0x00
        record[120] = 1
        XCTAssertEqual(ProfileView(record).validate(), eReservedDirty)
    }

    private func crcTableProxy() -> [UInt32] {
        // Same table construction as Crc32 (mirror for test-time CRC rebuild)
        var t = [UInt32](repeating: 0, count: 256)
        for n in 0..<256 {
            var c: UInt32 = UInt32(n)
            for _ in 0..<8 {
                c = (c & 1) != 0 ? (0xEDB88320 ^ (c >> 1)) : (c >> 1)
            }
            t[n] = c
        }
        return t
    }
}

final class SpectrumGovernorTests: XCTestCase {
    private func tick(_ st: CadenceState, _ inp: GovernorInput,
                      _ thermal: UInt32, _ batt: Int, _ charging: UInt32,
                      _ visibility: UInt32, _ heap: Int = 0) {
        inp.thermalState = thermal
        inp.batteryPermille = batt
        inp.batteryCharging = charging
        inp.visibility = visibility
        inp.heapPressure = heap
        cadenceTick(st, inp)
    }

    func testFrozenVectorCheckpoints() {
        // Checkpoints from tests/spectrum/managed/fixtures/governor_vector.json
        let st = CadenceState()
        let inp = GovernorInput()
        inp.tierMaxHzCap = 240

        for _ in 0..<9 { tick(st, inp, 3, 900, chargingYes, visVisible) }
        expect(st.capHz, 240)
        tick(st, inp, 3, 900, chargingYes, visVisible)
        expect(st.capHz, 120) // severe sustained 10 -> step down

        for _ in 0..<19 { tick(st, inp, 2, 900, chargingYes, visVisible) }
        expect(st.capHz, 120)
        tick(st, inp, 2, 900, chargingYes, visVisible)
        expect(st.capHz, 60) // moderate 2x window -> step down

        for _ in 0..<49 { tick(st, inp, 0, 900, chargingYes, visVisible) }
        expect(st.capHz, 60)
        tick(st, inp, 0, 900, chargingYes, visVisible)
        expect(st.capHz, 120) // recovery 50 -> step up

        tick(st, inp, 0, 120, chargingNo, visVisible)
        expect(st.capHz, 60) // low battery forces <= 60, rung untouched
        XCTAssertEqual(st.rung, 1)

        tick(st, inp, 0, 900, chargingYes, visHidden)
        expect(st.capHz, 30) // background (rule 1 wins)

        tick(st, inp, 0, 900, chargingYes, visVisible)
        expect(st.capHz, 120)
    }

    func testTierStagingHold() {
        let st = CadenceState()
        let inp = GovernorInput()
        inp.profileBudgetBytes = 8_589_934_592
        inp.heapPressure = 1
        tierTick(st, inp) // stage 1
        tierTick(st, inp) // stage 2
        XCTAssertEqual(st.tierStage, 2)
        XCTAssertEqual(st.effTier, tierBudget)
        XCTAssertEqual(st.budgetBytes, 2_147_483_648)
        XCTAssertEqual(tierTick(st, inp), eTierExhausted)
        XCTAssertEqual(st.tierStage, 2)
    }

    func testMetalPipelineSelection() {
        let ultra = MetalPipelineSelector.select(
            gpuFamily: 1, thermalCode: thermalNominal, cadenceCap: 240, dmaLanes: 4)
        XCTAssertEqual(ultra.pipelineClass, .ultra)
        XCTAssertEqual(ultra.maxFrameRate, 240)
        let throttled = MetalPipelineSelector.select(
            gpuFamily: 1, thermalCode: thermalSevere, cadenceCap: 60, dmaLanes: 4)
        XCTAssertEqual(throttled.pipelineClass, .standard)
        let critical = MetalPipelineSelector.select(
            gpuFamily: 1, thermalCode: thermalCritical, cadenceCap: 240, dmaLanes: 4)
        XCTAssertEqual(critical.pipelineClass, .conservative)
        XCTAssertEqual(critical.dmaLaneBudget, 1)
    }

    private func expect(_ actual: Int, _ expected: Int,
                        file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(actual, expected, file: file, line: line)
    }
}
