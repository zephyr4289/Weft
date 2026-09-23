#include <metal_stdlib>
using namespace metal;

// weft_preprocess.metal — RFC-0017 §4: the frozen camera-preprocess
// kernel (MSL mirror of weft_preprocess.{comp,wgsl}).
//
// THE CONTRACT (bit-exact by construction — weft_accel_common.h): each
// output element is (f32)src_byte * scale — one exact u8->f32 conversion
// plus ONE rounded multiply; no add exists for the compiler to fuse into
// fma, so Metal, SIMD and scalar outputs are bit-identical (gate: ==).
//
// KERNEL FREEZE: this source is the committed artifact; the runtime
// frozen-ID check (FNV-1a over the source string the pool compiles)
// refuses a wrong/old kernel before the Metal compiler runs.

struct WeftPreprocessPush {
    uint src_word_off;   // u32 word offset of the RGBA8 frame in buffer 0
    uint dst_elem_off;   // f32 element offset in buffer 1
    uint n_pixels;       // pixels to convert (4 f32 out each)
    float scale;         // the normalize contract's single multiply
};

kernel void weft_preprocess(device const uint* src_words [[buffer(0)]],
                            device float* dst_elems   [[buffer(1)]],
                            constant WeftPreprocessPush& p [[buffer(2)]],
                            uint id [[thread_position_in_grid]]) {
    if (id >= p.n_pixels) return;
    uint w = src_words[p.src_word_off + id];
    uint o = p.dst_elem_off + id * 4u;
    dst_elems[o + 0u] = float(w & 0xFFu)         * p.scale;
    dst_elems[o + 1u] = float((w >> 8u) & 0xFFu)  * p.scale;
    dst_elems[o + 2u] = float((w >> 16u) & 0xFFu) * p.scale;
    dst_elems[o + 3u] = float((w >> 24u) & 0xFFu) * p.scale;
}
