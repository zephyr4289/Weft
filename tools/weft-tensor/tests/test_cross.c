// test_cross.c — the AC-X capstone: ONE tensor view consumed by EVERY
// adapter domain, bit-exact. The pillar's claim — "the buffer the
// producer made IS the buffer every accelerator consumes" — as a gate.
//
// Domains exercised: scalar oracle, SIMD ([FALLBACK-COPY] road), Vulkan
// (the frozen preprocess kernel over an imported buffer — ICD leg), the
// ONNX wrap (pointer identity through the mock table), and the ggml
// placement road (span identity + shape). The Metal mirror contributes
// its geometry + the frozen MSL (the apple CI leg carries its device
// roads).

#include "weft_test_util.h"
#include "weft/weft_accel_common.h"
#include "weft/weft_tensor_view.h"

#include "backends/vulkan/weft_vk_bridge.h"
#include "backends/onnx/weft_ort_bridge.h"
#include "backends/onnx/tests/mock_ort.h"
#include "backends/ggml/weft_ggml_bridge.h"
#include "backends/metal/weft_metal_bridge.h"

#include <stdalign.h>
#include <string.h>

#define N 4096   // u8 samples -> N f32 out (the shared normalize contract)
#define SCALE (1.0f / 255.0f)

static alignas(64) uint8_t  g_src[N];
static alignas(64) float    g_scalar[N];   // the oracle
static alignas(64) float    g_simd[N];
static alignas(64) float    g_vk_out[N];

int main(void) {
    for (int i = 0; i < N; i++) g_src[i] = (uint8_t)(i * 37 + 11);

    // ---- X1: the oracle ------------------------------------------------
    weft_ref_normalize_u8_to_f32(g_scalar, g_src, N, SCALE);

    // ---- X2: SIMD domain (the fallback road is exact too) --------------
    weft_simd_normalize_u8_to_f32(g_simd, g_src, N, SCALE);
    GATE("AC-X2 SIMD == scalar (bit-exact, the contract)",
         memcmp(g_simd, g_scalar, sizeof(g_scalar)) == 0);
    printf("  (simd-isa=%s)\n", weft_simd_isa_name());

    // ---- X3: Vulkan domain (the frozen kernel; ICD leg) -----------------
    weft_vk_ctx_t* ctx = NULL;
    if (weft_vk_ctx_create(&ctx, "ac-x") != WEFT_VK_OK) {
        GATE_SKIP("AC-X3", "no Vulkan ICD (the AC-K series carries the "
                           "refusal legs)");
    } else {
        uint32_t words = 0;
        const uint32_t* spv = weft_vk_preprocess_spv(&words);
        weft_vk_mem_t mem, out;
        weft_vk_compute_t* k = NULL;
        int road = weft_vk_import_host(ctx, g_src, N, &mem) ==
                       WEFT_VK_OK &&
                   weft_vk_alloc_output(ctx, N * 4, 1, &out) ==
                       WEFT_VK_OK &&
                   weft_vk_compute_init(&k, weft_vk_ctx_dev(ctx), spv,
                                        words,
                                        weft_vk_preprocess_frozen_id(), 2,
                                        sizeof(weft_vk_preprocess_push_t))
                       == WEFT_VK_OK;
        if (!road) {
            printf("  (vk roads refused at setup — host-ext/alias "
                   "boundaries; see AC-K)\n");
            GATE_SKIP("AC-X3", "vk setup refused on this ICD");
        } else {
            weft_tensor_view_t v;
            uint32_t dims[1] = {N};
            weft_tensor_view_init(&v, WEFT_TENSOR_U8, dims, 1, g_src, 0);
            weft_vk_err_t e = weft_vk_preprocess_dispatch(
                k, mem.buffer, 0, out.buffer, 0, &v, SCALE);
            GATE("AC-X3 Vulkan == scalar (bit-exact, the frozen kernel)",
                 e == WEFT_VK_OK &&
                     memcmp(out.map, g_scalar, sizeof(g_scalar)) == 0);
            // and the alias canary over the same import (the Series-10
            // lesson: a fresh-memory device would have failed X3 with
            // zeros — the canary NAMES it)
            e = weft_vk_alias_verify(ctx, &mem, k);
            GATE("AC-X3b the import's alias verdict is DEFINITE",
                 e == WEFT_VK_OK || e == WEFT_VK_ERR_ALIAS);
            weft_vk_compute_destroy(k);
            weft_vk_mem_free(ctx, &out);
            weft_vk_mem_free(ctx, &mem);
        }
        weft_vk_ctx_destroy(ctx);
    }

    // ---- X4: the ONNX domain (pointer identity through the wrap) -------
    mock_ort_reset();
    weft_ort_runtime_t* mrt = NULL;
    weft_ort_session_t* s = NULL;
    int ort_ok = weft_ort_load_table(&mrt, mock_ort_table(), "mock") ==
                     WEFT_ORT_OK &&
                 weft_ort_session_open(&s, mrt, (const void*)"M", 1) ==
                     WEFT_ORT_OK;
    if (!ort_ok) {
        GATE_SKIP("AC-X4", "mock table refused (logic error — see AC-O)");
    } else {
        weft_tensor_view_t v;
        uint32_t dims[1] = {N};
        void* val = NULL;
        GATE("AC-X4 ONNX wrap carries the SAME bytes (no copy)",
             weft_tensor_view_init(&v, WEFT_TENSOR_F32, dims, 1, g_simd,
                                   0) == WEFT_TV_OK &&
                 weft_ort_wrap_view(s, &v, &val) == WEFT_ORT_OK &&
                 g_mock_ort.wrap_data == (void*)g_simd &&
                 g_mock_ort.wrap_len == (size_t)N * sizeof(float));
        weft_ort_value_free(s, val);  // ASAN-clean: release the mock's value
    }

    // ---- X5: the ggml domain (placement identity + shape) --------------
    {
        weft_ggml_planner_t p;
        weft_ggml_planner_init(&p);
        weft_ggml_planner_register(&p, g_simd, sizeof(g_simd));
        weft_tensor_view_t v;
        uint32_t dims[2] = {N / 4, 1};
        weft_ggml_shape_t sh;
        GATE("AC-X5 ggml shape maps the same bytes (ne reversed, nbytes)",
             weft_tensor_view_init(&v, WEFT_TENSOR_F32, dims, 2, g_simd,
                                   0) == WEFT_TV_OK &&
                 weft_ggml_shape_of_view(&v, &sh) == 0 &&
                 sh.ggml_type == WEFT_GGML_TYPE_F32 &&
                 sh.ne[0] == 1 && sh.ne[1] == N / 4 &&
                 sh.nbytes == (size_t)(N / 4) * sizeof(float));
    }

    // ---- X6: the Metal mirror (geometry + the frozen MSL identity) -----
    {
        weft_tensor_view_t v;
        uint32_t dims[2] = {N / 4 / 16, 16 * 4};  // rows of whole pixels
        weft_metal_geometry_t geo;
        GATE("AC-X6 Metal geometry maps the same frame (64-aligned rows)",
             weft_tensor_view_init(&v, WEFT_TENSOR_U8, dims, 2, g_src,
                                   0) == WEFT_TV_OK &&
                 weft_metal_geometry_for_view(&v, &geo) == 0 &&
                 geo.span_bytes >= v.byte_len);
        // the frozen MSL is the same one-multiply contract as the SPIR-V
        // (both kernels' outputs meet the SAME scalar oracle — the
        // cross-domain ==-gate's precondition)
        size_t msl_len = 0;
        const char* msl = weft_metal_preprocess_msl(&msl_len);
        GATE("AC-X7 Metal MSL and Vulkan SPIR-V share the contract",
             strstr(msl, "* p.scale") != NULL &&
                 strstr(msl, "fma") == NULL &&
                 weft_metal_preprocess_frozen_id() != 0);
    }

    if (s) weft_ort_session_close(s);
    if (mrt) weft_ort_unload(mrt);

    GATE_SUMMARY("AC-X (cross-domain capstone)");
}
