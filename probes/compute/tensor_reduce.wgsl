// tensor_reduce.wgsl — RFC-0016 §5.2, the WebGPU mirror of
// probes/compute/tensor_reduce.comp (same algorithm, same determinism
// contract, same result-word layout).
//
// THE WEBGPU SEAM (declared in RFC-0016 §6): the browser platform cannot
// import a dma-buf — its storage buffers live in the JS heap's GPU
// arena. The seam is the PROTOCOL, not the memory: a worker publishes
// WTS1 frames into a SharedArrayBuffer ring (the existing SAB kernel,
// demos/web), the main thread copies the span into a GPU storage buffer
// (one copy — the platform forces it; DECLARED), and THIS shader reduces
// the same words with the same lane partition + fixed tree, producing
// bit-identical results to the Vulkan pass (adds-only f32, no FMA
// ambiguity, IEEE-exact f16 unpacking via unpack2x16float).
//
// Bindings (the WGSL convention for the demos/web integration):
//   @group(0) @binding(0): ring span, storage, read — u32 words
//   @group(0) @binding(1): result[8], storage, read_write
// Push-constant equivalents travel via a small uniform at @binding(2):
//   { slots: u32, words: u32 }

struct Params {
    slots: u32,
    words: u32,
};

@group(0) @binding(0) var<storage, read> ring: array<u32>;
@group(0) @binding(1) var<storage, read_write> result: array<u32>;
@group(0) @binding(2) var<uniform> p: Params;

const WTS1_MAGIC: u32 = 0x31535457u;
const WTS1_HDR: u32 = 8u;
const DTYPE_F16: u32 = 8u;

var<workgroup> sh_sum: array<u32, 64>;
var<workgroup> sh_max: array<u32, 64>;
var<workgroup> sh_arg: array<u32, 64>;

@compute @workgroup_size(64)
fn main(@builtin(local_invocation_index) li: u32) {
    let seq = ring[16u];
    let k = (seq - 1u) % p.slots;
    let base = 20u + 2u * p.slots + k * p.words;

    var mismatch: u32 = 0u;
    if (ring[base] != WTS1_MAGIC) { mismatch += 1u; }
    let w1 = ring[base + 1u];
    if ((w1 & 0xFFu) != 1u) { mismatch += 1u; }                  // version
    if ((w1 >> 8u & 0xFFu) != DTYPE_F16) { mismatch += 1u; }     // dtype
    if ((w1 >> 24u & 0xFFu) != 0u) { mismatch += 1u; }           // flags
    let count = ring[base + 2u];
    let pwords = ring[base + 3u];
    if (count > 0u && pwords != (count * 2u + 3u) / 4u) { mismatch += 1u; }
    if (pwords + WTS1_HDR > p.words) { mismatch += 1u; }
    let elems = count;

    // the lane partition (identical to the .comp — the determinism pair)
    var lane_sum: f32 = 0.0;
    var lane_max: f32 = 0.0;
    var lane_arg: u32 = 0u;
    let chunk = (elems + 63u) / 64u;
    let lo = li * chunk;
    var hi = lo + chunk;
    if (hi > elems) { hi = elems; }
    var first = true;
    if (lo < hi) {
        for (var i = lo; i < hi; i++) {
            let w = ring[base + WTS1_HDR + i / 2u];
            let h = select(w >> 16u, w & 0xFFFFu, i % 2u == 0u);
            let v = unpack2x16float(h).x;   // IEEE-exact f16 -> f32
            lane_sum += v;                  // adds only: FMA-proof
            if (first || v > lane_max) {
                lane_max = v;
                lane_arg = i;
            }
            first = false;
        }
    }

    sh_sum[li] = bitcast<u32>(lane_sum);
    sh_max[li] = bitcast<u32>(lane_max);
    sh_arg[li] = lane_arg;
    workgroupBarrier();

    // the FIXED combine tree (mirrored by the CPU reference)
    var s: u32 = 1u;
    loop {
        if (s >= 64u) { break; }
        if (li % (2u * s) == 0u && li + s < 64u) {
            let a = bitcast<f32>(sh_sum[li]);
            let b = bitcast<f32>(sh_sum[li + s]);
            sh_sum[li] = bitcast<u32>(a + b);
            let ma = bitcast<f32>(sh_max[li]);
            let mb = bitcast<f32>(sh_max[li + s]);
            if (mb > ma) {
                sh_max[li] = sh_max[li + s];
                sh_arg[li] = sh_arg[li + s];
            }
        }
        workgroupBarrier();
        s <<= 1u;
    }

    if (li == 0u) {
        result[0] = mismatch;
        result[1] = sh_arg[0];
        result[2] = sh_sum[0];
        result[3] = sh_max[0];
        result[4] = 0x54464557u;   // "WEFT" LE — the "GPU ran this" magic
        result[5] = elems;
        result[6] = seq;
        result[7] = pwords;
    }
}
