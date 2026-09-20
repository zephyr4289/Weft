// fft_radix2.wgsl — RFC-0016 §5.2, the WebGPU mirror of
// probes/compute/fft_radix2.comp (same 512-point radix-2 DIT FFT, same
// result-word layout, same DECLARED tolerances — see the .comp's honest
// numerics note). The browser seam is the SAB-ring -> storage-buffer
// copy documented in RFC-0016 §6 (the platform forces exactly one copy;
// the protocol stays identical).

struct Params {
    slots: u32,
    words: u32,
};

@group(0) @binding(0) var<storage, read> ring: array<u32>;
@group(0) @binding(1) var<storage, read_write> result: array<u32>;
@group(0) @binding(2) var<uniform> p: Params;

const WTS1_MAGIC: u32 = 0x31535457u;
const WTS1_HDR: u32 = 8u;
const DTYPE_F32: u32 = 9u;
const N: u32 = 512u;

var<workgroup> sre: array<f32, 512>;
var<workgroup> sim: array<f32, 512>;

fn bitrev(x: u32, bits: u32) -> u32 {
    var y: u32 = 0u;
    var xx = x;
    for (var i: u32 = 0u; i < bits; i++) {
        y = (y << 1u) | (xx & 1u);
        xx >>= 1u;
    }
    return y;
}

@compute @workgroup_size(512)
fn main(@builtin(local_invocation_index) li: u32) {
    let seq = ring[16u];
    let k = (seq - 1u) % p.slots;
    let base = 20u + 2u * p.slots + k * p.words;

    var mismatch: u32 = 0u;
    if (ring[base] != WTS1_MAGIC) { mismatch += 1u; }
    let w1 = ring[base + 1u];
    if ((w1 & 0xFFu) != 1u) { mismatch += 1u; }
    if ((w1 >> 8u & 0xFFu) != DTYPE_F32) { mismatch += 1u; }
    if ((w1 >> 24u & 0xFFu) != 0u) { mismatch += 1u; }
    if (ring[base + 2u] != N) { mismatch += 1u; }

    sre[bitrev(li, 9u)] = bitcast<f32>(ring[base + WTS1_HDR + li]);
    sim[li] = 0.0;
    workgroupBarrier();

    var j: u32 = 1u;
    loop {
        if (j >= N) { break; }
        let len = j << 1u;
        let blk = (li / len) * len;
        let off = li % len;
        if (off < j) {
            let ang = -6.283185307179586 * f32(off) / f32(len);
            let tw = cos(ang);
            let tih = sin(ang);
            let ar = sre[blk + off];
            let ai = sim[blk + off];
            let br = sre[blk + off + j];
            let bi = sim[blk + off + j];
            let tr = br * tw - bi * tih;
            let ti = br * tih + bi * tw;
            sre[blk + off] = ar + tr;
            sim[blk + off] = ai + ti;
            sre[blk + off + j] = ar - tr;
            sim[blk + off + j] = ai - ti;
        }
        workgroupBarrier();
        j <<= 1u;
    }

    if (li == 0u) {
        var m: f32 = 0.0;
        var peak_bin: u32 = 0u;
        for (var i: u32 = 0u; i < N; i++) {
            let v = sqrt(sre[i] * sre[i] + sim[i] * sim[i]);
            if (v > m) { m = v; peak_bin = i; }
        }
        var sum_x2: f32 = 0.0;
        for (var i: u32 = 0u; i < N; i++) {
            let x = bitcast<f32>(ring[base + WTS1_HDR + i]);
            sum_x2 += x * x;
        }
        var sum_X2: f32 = 0.0;
        for (var i: u32 = 0u; i < N; i++) {
            sum_X2 += sre[i] * sre[i] + sim[i] * sim[i];
        }
        let denom = f32(N) * sum_x2;
        let perr = abs(sum_X2 - denom) / denom;
        var flags = mismatch;
        if (perr > 1e-4) { flags |= 2u; }  // DECLARED tolerance

        result[0] = flags;
        result[1] = peak_bin;
        result[2] = bitcast<u32>(m);
        result[3] = bitcast<u32>(perr);
        result[4] = 0x54464557u;
        result[5] = N;
        result[6] = seq;
        result[7] = bitcast<u32>(sum_x2);
    }
}
