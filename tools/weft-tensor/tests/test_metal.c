// test_metal.c — the AC-M series: the Metal adapter's portable core
// (geometry ladder, frozen MSL identity, weak-stub refusals). The device
// roads are the apple-CI leg — DECLARED here, MEASURED there (the
// WeftMetalZeroCopy/gpu_ring-METAL precedent; Law 4's honesty map).

#include "weft_test_util.h"
#include "weft/weft_tensor_view.h"

#include "backends/metal/weft_metal_bridge.h"

#include <stdalign.h>
#include <string.h>

static alignas(64) uint8_t g_buf[8192];

int main(void) {
    // ---- the geometry ladder -------------------------------------------
    weft_tensor_view_t v;
    uint32_t dims[3] = {256, 256, 4};   // a camera frame (RGBA8, NHWC)
    weft_metal_geometry_t geo;
    GATE("AC-M1 RGBA8 camera view maps to 64-aligned geometry",
         weft_tensor_view_init(&v, WEFT_TENSOR_U8, dims, 3, g_buf, 0) ==
             WEFT_TV_OK &&
             weft_metal_geometry_for_view(&v, &geo) == 0 &&
             geo.width == 256 && geo.height == 256 &&
             geo.bytes_per_element == 4 &&
             geo.bytes_per_row == 1024 &&       // 256*4, already 64-aligned
             geo.span_bytes == 1024ull * 256ull);

    // rows that need padding: W*C = 100 bytes -> 128-byte rows
    uint32_t odd_dims[2] = {8, 100};
    weft_tensor_view_t odd;
    GATE("AC-M2 odd-width rows pad to the 64-byte law",
         weft_tensor_view_init(&odd, WEFT_TENSOR_U8, odd_dims, 2, g_buf,
                               0) == WEFT_TV_OK &&
             weft_metal_geometry_for_view(&odd, &geo) == 0 &&
             geo.bytes_per_row == 128 && geo.width == 25 &&
             geo.height == 8);

    // the f32 spectrogram road
    uint32_t spec_dims[2] = {64, 128};
    weft_tensor_view_t spec;
    GATE("AC-M3 f32 spectrogram view maps (the 128RGBAFloat road)",
         weft_tensor_view_init(&spec, WEFT_TENSOR_F32, spec_dims, 2,
                               g_buf, 0) == WEFT_TV_OK &&
             weft_metal_geometry_for_view(&spec, &geo) == 0 &&
             geo.bytes_per_row == 512);

    // refusals: dtype outside the two roads, strided views, tiny rows
    uint32_t d16[1] = {4};
    weft_tensor_view_t bad;
    GATE("AC-M4 i16 view refuses (outside the v1 wrap roads)",
         weft_tensor_view_init(&bad, WEFT_TENSOR_I16, d16, 1, g_buf, 0) ==
             WEFT_TV_OK &&
             weft_metal_geometry_for_view(&bad, &geo) == -1 &&
             geo.view_err == WEFT_TV_ERR_DTYPE);
    uint32_t sdim[2] = {4, 8}, sstr[2] = {16, 1};
    weft_tensor_view_t sv;
    GATE("AC-M5 strided view refuses (wraps need contiguous rows)",
         weft_tensor_view_init_strided(&sv, WEFT_TENSOR_F32, sdim, sstr, 2,
                                       g_buf, 0) == WEFT_TV_OK &&
             weft_metal_geometry_for_view(&sv, &geo) == -1 &&
             geo.view_err == WEFT_TV_ERR_STRIDES);
    uint32_t tiny[1] = {2};   // 2 f32 = 8 bytes < 16
    weft_tensor_view_t tv;
    GATE("AC-M6 sub-16-byte rows refuse (the bytesNoCopy floor)",
         weft_tensor_view_init(&tv, WEFT_TENSOR_F32, tiny, 1, g_buf, 0) ==
             WEFT_TV_OK &&
             weft_metal_geometry_for_view(&tv, &geo) == -1);

    // u8 rows that are not whole pixels
    uint32_t np[1] = {6};
    weft_tensor_view_t npv;
    GATE("AC-M7 non-pixel-multiple u8 rows refuse",
         weft_tensor_view_init(&npv, WEFT_TENSOR_U8, np, 1, g_buf, 0) ==
             WEFT_TV_OK &&
             weft_metal_geometry_for_view(&npv, &geo) == -1 &&
             geo.view_err == WEFT_TV_ERR_DIMS);

    // ---- the frozen MSL identity ----------------------------------------
    {
        size_t msl_len = 0;
        const char* msl = weft_metal_preprocess_msl(&msl_len);
        GATE("AC-M8 committed MSL present + frozen ID deterministic",
             msl != NULL && msl_len > 100 &&
                 weft_metal_preprocess_frozen_id() != 0);
        // the MSL mirror carries the same one-multiply contract as the
        // GLSL/WGSL pair: exactly one FLOAT multiply per element and no
        // float add (nothing to fuse into an FMA — the ==-gate's
        // precondition). Integer address arithmetic may use '+'.
        GATE("AC-M9 MSL is the normalize contract (single float multiply)",
             strstr(msl, "* p.scale") != NULL &&
                 strstr(msl, "+ p.scale") == NULL &&
                 strstr(msl, "fma") == NULL);
    }

    // ---- the non-Apple truth (weak stubs refuse honestly) ---------------
    weft_metal_caps_t caps = weft_metal_probe();
    printf("  (probe: %s)\n", weft_metal_caps_name(caps));
#ifdef __APPLE__
    GATE("AC-M10 Apple host probes a device",
         caps != WEFT_METAL_UNSUPPORTED);
    // The device roads run on the apple CI leg (MEASURED there).
    if (caps == WEFT_METAL_UNIFIED) {
        void* buf = NULL;
        GATE("AC-M11 MTLBuffer wrap (zero-copy, bytesNoCopy)",
             weft_metal_wrap_mtlbuffer(&v, &buf) == 0 && buf != NULL);
        if (buf) weft_metal_release(buf);
        void* pb = NULL;
        GATE("AC-M12 CVPixelBuffer wrap (the CoreML/ANE road)",
             weft_metal_wrap_cvpixelbuffer(&v, &pb) == 0 && pb != NULL);
        if (pb) weft_metal_release(pb);
        void* surf = NULL; void* bytes = NULL; uint64_t span = 0;
        GATE("AC-M13 IOSurface span (reverse ownership)",
             weft_metal_iosurface_span(256, 256, 4, &surf, &bytes,
                                       &span) == 0 &&
                 surf && bytes && span >= 1024 * 256);
        if (surf) weft_metal_release(surf);
    } else {
        GATE_SKIP("AC-M11..13", "discrete Mac — wraps refuse by design");
    }
#else
    GATE("AC-M10 non-Apple probe reports UNSUPPORTED",
         caps == WEFT_METAL_UNSUPPORTED);
    void* h = NULL;
    GATE("AC-M11 weak stub refuses the MTLBuffer wrap",
         weft_metal_wrap_mtlbuffer(&v, &h) == -1 && h == NULL);
    GATE("AC-M12 weak stub refuses the CVPixelBuffer wrap",
         weft_metal_wrap_cvpixelbuffer(&v, &h) == -1 && h == NULL);
    {
        void* surf = NULL; void* bytes = NULL; uint64_t span = 0;
        GATE("AC-M13 weak stub refuses the IOSurface span",
             weft_metal_iosurface_span(256, 256, 4, &surf, &bytes,
                                       &span) == -1 &&
                 surf == NULL && bytes == NULL && span == 0);
    }
#endif

    GATE_SUMMARY("AC-M (metal adapter core)");
    return 0;
}
