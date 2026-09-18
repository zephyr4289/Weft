// stream_frames.wgsl — RFC-0013 whole-ring streaming consumer, WebGPU road.
//
// WGSL mirror of probes/compute/stream_frames.comp (same word map).
//
// THE HONEST WEBGPU BOUNDARY (Law 4 — stated, not worked around): WebGPU's
// security model sandboxes GPU memory away from the page granularity the
// other roads alias. WASM cannot mmap a GPUBuffer onto the SharedArrayBuffer
// a Weft ring lives in — the platform does not offer the primitive. The
// integration is therefore ONE copy at the WASM/GPU boundary, and cannot
// structurally be fewer:
//
//   ring (SharedArrayBuffer, the fan-out ring the CPU writes)
//     -> queue.writeBuffer(gpu_ring_mirror, 0, ringView)   // the one copy
//     -> compute pass over gpu_ring_mirror (this kernel)
//     -> result staging buffer, MAP_READ                    // report card
//
// That single `writeBuffer` is the minimum the platform permits; every
// other byte of the pipeline (consume, validate, reduce) is GPU-side. The
// pattern keeps the Weft protocol byte-identical across all four roads —
// the same WFSH header, the same stamps, the same mixer family — so a
// capture validates identically whichever road consumed it.
//
// The kernel itself is the same whole-ring consumer: one workgroup over the
// slots, every resident slot validated, window fingerprint + width in the
// result buffer. Result layout: 8 u32 words (matches the gpu_stream kit).

struct Params {
    slots: u32,
    words: u32,
};

@group(0) @binding(0) var<storage, read> ring: array<u32>;
@group(0) @binding(1) var<storage, read_write> result: array<u32>;
@group(0) @binding(2) var<uniform> params: Params;

fn mix32(x: u32) -> u32 {
    var v = x;
    v ^= v >> 16u;
    v = v * 0x7feb352du;
    v ^= v >> 15u;
    v = v * 0x846ca68bu;
    v ^= v >> 16u;
    return v;
}

var<workgroup> mism: array<u32, 64>;
var<workgroup> xors: array<u32, 64>;
var<workgroup> cnts: array<u32, 64>;

@compute @workgroup_size(64)
fn stream_frames(@builtin(local_invocation_index) li: u32) {
    let seq = ring[16u];
    let seq_hi = ring[17u];

    var count: u32 = 0u;
    var xf: u32 = 0u;
    var nvalid: u32 = 0u;
    if (li < params.slots) {
        let k = li;
        let stamp_lo = ring[20u + 2u * k];
        let stamp_hi = ring[21u + 2u * k];
        if (stamp_lo != 0u || stamp_hi != 0u) {
            nvalid = 1u;
            if (stamp_hi != 0u) { count += 1u; }
            if (stamp_lo > seq) { count += 1u; }
            let base = 20u + 2u * params.slots + k * params.words;
            for (var i: u32 = 0u; i < params.words; i = i + 1u) {
                if (ring[base + i] != mix32(stamp_lo * 2654435761u + i)) {
                    count += 1u;
                }
            }
            xf = ring[base];
        }
    }
    mism[li] = count;
    xors[li] = xf;
    cnts[li] = nvalid;
    workgroupBarrier();

    if (li == 0u) {
        var total: u32 = 0u;
        var xorv: u32 = 0u;
        var nslot: u32 = 0u;
        for (var i: u32 = 0u; i < 64u; i = i + 1u) {
            total = total + mism[i];
            xorv = xorv ^ xors[i];
            nslot = nslot + cnts[i];
        }
        if (ring[0u] != 0x48534657u) { total += 1u; }
        if (seq_hi != 0u) { total += 1u; }
        result[0u] = total;
        result[1u] = seq;
        result[2u] = xorv;
        result[3u] = nslot;
        result[4u] = 0x54464557u;  // "WEFT" LE — the GPU ran this
    }
}
