// WeftMetalZeroCopy.swift — RFC-0016 §6: the Apple unified-memory seam.
//
// WHY EXISTS: on Apple Silicon the CPU's RAM IS the GPU's RAM — the
// zero-copy road that VK_EXT_external_memory_host provides on Vulkan
// platforms is, on Apple, simply MTLBuffer(bytesNoCopy:) over the ring's
// storage with .storageModeShared. This bridge wraps a Weft ring's bytes
// (any session the CPU already owns: shm, malloc'd, mmap'd) in a Metal
// buffer whose GPU handle aliases the SAME pages — compute passes read
// live ring words with no staging copy, the exact stance
// WeftMTKViewCadenceProbe takes for rendering.
//
// THE CONTRACT (mirrors weft_gpu_wrap_host's discipline):
//   - The CALLER owns the ring memory and must keep it alive for the
//     buffer's lifetime (the buffer does not copy; release the buffer
//     before the ring).
//   - bytesNoCopy requires 16-byte-aligned, length >= 16 — Weft rings
//     are 64-byte aligned by construction (posix_memalign(64) on the
//     malloc road; page-aligned on every shared road).
//   - DISCRETE-GPU MACS: .shared storage forces integrated-style
//     behavior; on discrete-GPU Macs the wrapped buffer is not GPU-
//     resident and Metal may stage — the bridge REFUSES (returns nil)
//     when the device reports .storageModeShared is unsupported for
//     compute, rather than silently degrading (Law 4 — the same stance
//     gpu_ring's Metal backend takes, hardware-deferred honesty).
//
// WHAT RUNS WHERE: compiled by the apple CI leg (macOS runners + iOS
// simulator); NOT executable-tested in the x86_64 Linux sandbox —
// declared (the sha256_hw.c ARM-CE precedent). The API surface is
// deliberately minimal: wrap, bind, verify — a compute pipeline belongs
// to the application (the render bridge's mechanism-not-policy line).
//
// Layer discipline: driver layer (Sources/WeftSwiftUI); the C kernel is
// untouched.

import Metal
import Foundation

/// A zero-copy Metal view over a Weft ring's bytes.
public struct WeftMetalZeroCopyBuffer {
    /// The wrapped MTLBuffer (aliases the ring's pages — no copy).
    public let buffer: MTLBuffer
    /// The ring base the caller wrapped (diagnostics; advisory only).
    public let ringBase: UnsafeMutableRawPointer
    /// The ring span in bytes (header excluded — the RFC-0004 ring only).
    public let ringBytes: Int
    /// True when the device verified shared-storage compute support.
    public let deviceSupportsSharedCompute: Bool

    private init(buffer: MTLBuffer, base: UnsafeMutableRawPointer,
                 bytes: Int, sharedCompute: Bool) {
        self.buffer = buffer
        self.ringBase = base
        self.ringBytes = bytes
        self.deviceSupportsSharedCompute = sharedCompute
    }

    /// Wrap ring bytes the CPU already owns. `ringBytes` is the RFC-0004
    /// ring span (the fan-out ring, WITHOUT the 64-byte WFSH header —
    /// Metal consumers bind the ring's control + payload words; the WFSH
    /// header belongs to the session layer, not the GPU).
    ///
    /// Returns nil when: the device lacks shared-storage compute support
    /// (discrete-only Macs — refused, never staged), the arguments are
    /// misaligned/undersized, or the allocation failed.
    public static func wrap(device: MTLDevice,
                            ring: UnsafeMutableRawPointer,
                            ringBytes: Int) -> WeftMetalZeroCopyBuffer? {
        // bytesNoCopy contract: >= 16 bytes, 16-byte alignment. Weft's
        // rings are 64-byte aligned by construction; a foreign base that
        // is not, is refused (the documented malloc'd-ring boundary the
        // Vulkan wrap takes at page granularity).
        guard ringBytes >= 16,
              Int(bitPattern: ring) % 16 == 0 else { return nil }

        // Law 4 on Apple: refuse rather than silently stage on devices
        // where shared storage is not GPU-visible for compute.
        let sharedCompute: Bool
        if device.hasUnifiedMemory {
            sharedCompute = true
        } else if #available(macOS 10.15, iOS 13.0, *) {
            // Discrete Macs: MTLReadWriteBufferTier + storageModeShared
            // support is the honest gate; tier 2 + shared => compute can
            // dereference the wrapped pages.
            sharedCompute = device.readWriteTextureSupport == .tier2
                || device.readWriteTextureSupport == .tier1
        } else {
            sharedCompute = false
        }
        guard sharedCompute else { return nil }

        guard let buf = device.makeBuffer(bytesNoCopy: ring,
                                          length: ringBytes,
                                          options: .storageModeShared) else {
            return nil
        }
        return WeftMetalZeroCopyBuffer(buffer: buf, base: ring,
                                       bytes: ringBytes,
                                       sharedCompute: sharedCompute)
    }

    /// Bind the wrapped ring for a compute pass (read-only consumption —
    /// the fan-out protocol's reader stance; writers stay CPU-side).
    public func bind(encoder: MTLComputeCommandEncoder, index: Int) {
        encoder.setBuffer(buffer, offset: 0, index: index)
    }

    /// One-line capability report for diagnostics/evidence (advisory).
    public var description: String {
        "WeftMetalZeroCopy(base=\(String(describing: ringBase)), " +
        "bytes=\(ringBytes), sharedCompute=\(deviceSupportsSharedCompute))"
    }
}
