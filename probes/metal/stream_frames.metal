// stream_frames.metal — RFC-0013 whole-ring streaming consumer, Metal road.
//
// MSL mirror of probes/compute/stream_frames.comp: the SAME word map
// (WFSH header words[0..16), latestSeq words[16..18), publishes
// words[18..20), slotSeq[k] words[20+2k], payload word i of slot k at
// words[20+2M + k*W + i]) — one thread per slot, every resident slot
// validated against the 04-LITMUS mixer family, result packed into one
// uint4.
//
// ZERO-COPY ON APPLE: unified memory means the CPU mapping and the GPU view
// are the same physical RAM by construction. The session span (allocated by
// gpu_ring's METAL road as anonymous MAP_SHARED memory) is wrapped WITHOUT
// copying:
//
//     id<MTLBuffer> ring = [device newBufferWithBytesNoCopy:span_ptr
//                                  length:span
//                                  options:MTLResourceStorageModeShared];
//
// newBufferWithBytesNoCopy adopts the caller's pages (no copy, no new
// allocation); the CPU's fan-out pointer and the shader's buffer argument
// alias one allocation — the exact zero-copy claim RFC-0003 made for
// Vulkan, realized through Metal's unified-memory road. The wrap is
// Apple-side glue (Objective-C/Swift) — see probes/metal/README.md.
//
// HONESTY BOUNDARY: this kernel is the MSL reference for the RFC-0013
// consumer contract; it is NOT compile-tested in the x86_64 sandbox (no
// Metal toolchain exists off Apple hardware). The apple CI leg can gate it
// with: xcrun metal -c stream_frames.metal -o /dev/null
// (proposed as an optional step in apple-packages; declared, not claimed).
//
// The "GPU ran this" magic (0x54464557 "WEFT" LE) travels in out[3].

#include <metal_stdlib>
using namespace metal;

constant uint WEFT_MAGIC_RAN = 0x54464557u;  // "WEFT" LE

uint mix32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

kernel void stream_frames(device const uint* r  [[buffer(0)]],   // session span
                          device uint4*      out [[buffer(1)]],  // result
                          constant uint2&   geo [[buffer(2)]]) { // slots, words
    uint slots = geo.x;
    uint words = geo.y;
    uint k = (uint)thread_position_in_grid.x;

    uint count = 0u;
    uint xf = 0u;
    uint nvalid = 0u;
    if (k < slots) {
        uint stamp_lo = r[20u + 2u * k];
        uint stamp_hi = r[21u + 2u * k];
        if (stamp_lo != 0u || stamp_hi != 0u) {
            nvalid = 1u;
            if (stamp_hi != 0u) count += 1u;
            if (stamp_lo > r[16]) count += 1u;   // stamp ran past latestSeq
            uint base = 20u + 2u * slots + k * words;
            for (uint i = 0u; i < words; i++) {
                if (r[base + i] != mix32(stamp_lo * 2654435761u + i)) {
                    count += 1u;
                }
            }
            xf = r[base];
        }
    }
    // One result uint4: (mismatches, latestSeq, xor-fingerprint, magic).
    // The naive packing writes from thread 0 only (the reference shape);
    // the simdgroup-reduction variant lives in README.md.
    if (k == 0u) {
        out[0] = uint4(count, r[16], xf, WEFT_MAGIC_RAN);
    }
}
