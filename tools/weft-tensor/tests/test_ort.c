// test_ort.c — the AC-O series: the ONNX bridge's logic battery over the
// mock table (pointer identity, pooling-profile discipline, bind/run call
// shapes, the Law-1 malloc window) + the real-library leg (fixture model,
// DECLARED where libonnxruntime is absent).

#include "weft_test_util.h"
#include "weft/weft_accel_common.h"
#include "weft/weft_tensor_view.h"

#include "backends/onnx/weft_ort_bridge.h"
#include "backends/onnx/tests/mock_ort.h"
#include "backends/onnx/tests/fixtures/fixture_model.h"

#include <stdalign.h>
#include <string.h>

static alignas(64) uint8_t g_slot[4096];

int main(void) {
    // ---- O1: the no-runtime refusal -----------------------------------
    weft_ort_runtime_t* rt = NULL;
    weft_ort_err_t e = weft_ort_load(&rt);
    int have_real = (e == WEFT_ORT_OK);
    if (!have_real) {
        GATE("AC-O1 no-runtime refusal is named",
             e == WEFT_ORT_ERR_NO_RUNTIME);
        printf("  (%s absent — the mock battery carries the logic)\n",
               weft_ort_err_name(e));
    } else {
        GATE("AC-O1 real runtime loaded",
             have_real && rt != NULL);
        printf("  (onnxruntime %s)\n", weft_ort_version(rt));
    }

    // ---- the mock battery (every host) ---------------------------------
    mock_ort_reset();
    const weft_ort_api_t* table = mock_ort_table();
    weft_ort_runtime_t* mrt = NULL;
    GATE("AC-O2 mock table injects through load_table",
         weft_ort_load_table(&mrt, table, "mock-1.16.3") == WEFT_ORT_OK);

    weft_ort_session_t* s = NULL;
    e = weft_ort_session_open(&s, mrt, weft_fixture_identity_onnx,
                              weft_fixture_identity_onnx_len);
    GATE("AC-O3 session opens over in-tree fixture bytes",
         e == WEFT_ORT_OK && s != NULL &&
             strcmp(weft_ort_input_name(s, 0), "X") == 0 &&
             strcmp(weft_ort_output_name(s, 0), "Y") == 0);

    GATE("AC-O4 the pooling profile is enforced",
         g_mock_ort.disable_mem_pattern == 1 &&
             g_mock_ort.disable_cpu_mem_arena == 1);

    // the fixture bytes ride through unmodified
    GATE("AC-O5 model bytes reach the runtime verbatim",
         g_mock_ort.model_bytes == (const void*)weft_fixture_identity_onnx &&
             g_mock_ort.model_len == weft_fixture_identity_onnx_len);

    // ---- O6: the zero-copy wrap (pointer identity is THE gate) ---------
    weft_tensor_view_t v;
    uint32_t dims[2] = {1, 64};
    GATE("AC-O6 wrap_view passes the view's OWN pointer",
         weft_tensor_view_init(&v, WEFT_TENSOR_F32, dims, 2, g_slot, 0) ==
             WEFT_TV_OK);
    void* in_value = NULL;
    e = weft_ort_wrap_view(s, &v, &in_value);
    GATE("AC-O7 CreateTensorWithDataAsOrtValue carries (ptr, len, shape)",
         e == WEFT_ORT_OK && in_value != NULL &&
             g_mock_ort.wrap_data == (void*)g_slot &&
             g_mock_ort.wrap_len == 256 &&
             g_mock_ort.wrap_shape_len == 2 &&
             g_mock_ort.wrap_shape[0] == 1 &&
             g_mock_ort.wrap_shape[1] == 64 &&
             g_mock_ort.wrap_type == 1 /* ONNX FLOAT */);
    // the identity through the runtime's OWN accessor
    void* data_back = NULL;
    {
        const weft_ort_api_t* t =
            (const weft_ort_api_t*)weft_ort_raw_table(mrt);
        t->GetTensorMutableData(in_value, &data_back);
    }
    GATE("AC-O8 GetTensorMutableData returns the SAME pointer (zero-copy)",
         data_back == (void*)g_slot);

    // ---- O9: the refusal ladder ----------------------------------------
    {
        weft_tensor_view_t sv;
        uint32_t sd[2] = {1, 64}, ss[2] = {128, 1};
        void* out = NULL;
        GATE("AC-O9 strided view refuses the wrap",
             weft_tensor_view_init_strided(&sv, WEFT_TENSOR_F32, sd, ss, 2,
                                           g_slot, 0) == WEFT_TV_OK &&
                 weft_ort_wrap_view(s, &sv, &out) == WEFT_ORT_ERR_VIEW);
        weft_tensor_view_t bv;
        uint32_t bd[1] = {64};
        GATE("AC-O10 big-endian view refuses the wrap",
             weft_tensor_view_init(&bv, WEFT_TENSOR_F32, bd, 1, g_slot, 0) ==
                 WEFT_TV_OK &&
                 (bv.flags |= WEFT_TENSOR_VIEW_F_BIG_ENDIAN, true) &&
                 weft_ort_wrap_view(s, &bv, &out) == WEFT_ORT_ERR_VIEW);
    }

    // ---- O11: bind + pooled run (call shapes + the malloc window) ------
    GATE("AC-O11 bind_input reaches IoBinding by name",
         weft_ort_bind_input(s, "X", in_value) == WEFT_ORT_OK &&
             g_mock_ort.bind_input == 1 &&
             strcmp(g_mock_ort.last_input_name, "X") == 0);
    weft_tensor_view_t ov;
    uint32_t od[2] = {1, 64};
    void* out_value = NULL;
    GATE("AC-O12 bind_output_span wraps Weft memory as the output",
         weft_tensor_view_init(&ov, WEFT_TENSOR_F32, od, 2,
                               g_slot + 512, 0) == WEFT_TV_OK &&
             weft_ort_bind_output_span(s, "Y", &ov, &out_value) ==
                 WEFT_ORT_OK &&
             g_mock_ort.bind_output == 1 &&
             strcmp(g_mock_ort.last_output_name, "Y") == 0 &&
             g_mock_ort.wrap_data == (void*)(g_slot + 512));

    // THE LAW-1 WINDOW: zero heap allocations across a pooled run
    if (weft_accel_alloc_audit_install() == 0) {
        uint64_t before = weft_accel_alloc_count();
        e = weft_ort_run_pooled(s);
        uint64_t allocs = weft_accel_alloc_count() - before;
        GATE("AC-O13 pooled run: ONE table call, ZERO heap allocations",
             e == WEFT_ORT_OK && g_mock_ort.run_with_binding == 1 &&
                 allocs == 0);
        weft_accel_alloc_audit_remove();
    } else {
        GATE_SKIP("AC-O13", "audit not compiled in");
    }

    // ---- O14: the dtype map (verified v1.16.3 enum) --------------------
    GATE("AC-O14 WTS1 -> ONNX dtype map",
         weft_ort_onnx_type(WEFT_TENSOR_F32) == 1 &&
             weft_ort_onnx_type(WEFT_TENSOR_U8) == 2 &&
             weft_ort_onnx_type(WEFT_TENSOR_I8) == 3 &&
             weft_ort_onnx_type(WEFT_TENSOR_F16) == 10 &&
             weft_ort_onnx_type(WEFT_TENSOR_F64) == 11 &&
             weft_ort_onnx_type(WEFT_TENSOR_U64) == 13);

    // ---- the real-library leg ------------------------------------------
    if (have_real) {
        weft_ort_session_t* rs = NULL;
        e = weft_ort_session_open(&rs, rt, weft_fixture_identity_onnx,
                                  weft_fixture_identity_onnx_len);
        if (e != WEFT_ORT_OK) {
            printf("  (real leg: fixture refused: %s)\n",
                   weft_ort_last_error(rt));
            GATE_SKIP("AC-O15", "fixture refused by the real runtime");
        } else {
            // zero-copy round trip: input slot -> Identity -> output slot
            weft_tensor_view_t rin, rout;
            uint32_t d64[2] = {1, 64};
            void *rv_in = NULL, *rv_out = NULL;
            alignas(64) static float in_buf[64];
            alignas(64) static float out_buf[64];
            for (int i = 0; i < 64; i++) in_buf[i] = (float)i * 0.25f;
            int ok_wrap = weft_tensor_view_init(&rin, WEFT_TENSOR_F32, d64,
                                                2, in_buf, 0) ==
                              WEFT_TV_OK &&
                          weft_ort_wrap_view(rs, &rin, &rv_in) ==
                              WEFT_ORT_OK &&
                          weft_tensor_view_init(&rout, WEFT_TENSOR_F32, d64,
                                                2, out_buf, 0) ==
                              WEFT_TV_OK &&
                          weft_ort_bind_output_span(rs, "Y", &rout,
                                                    &rv_out) == WEFT_ORT_OK;
            GATE("AC-O15 real wrap: input+output over Weft memory",
                 ok_wrap);
            if (ok_wrap) {
                // pointer identity through the REAL runtime
                void* ptr = NULL;
                // (GetTensorMutableData via the bridge's own table)
                const weft_ort_api_t* t =
                    (const weft_ort_api_t*)weft_ort_raw_table(rt);
                t->GetTensorMutableData(rv_in, &ptr);
                GATE("AC-O16 REAL runtime returns the SAME pointer",
                     ptr == (void*)in_buf);
                weft_ort_bind_input(rs, weft_ort_input_name(rs, 0),
                                    rv_in);
                e = weft_ort_run_pooled(rs);
                GATE("AC-O17 Identity round trip is BIT-EXACT in Weft "
                     "memory",
                     e == WEFT_ORT_OK &&
                         memcmp(in_buf, out_buf, sizeof(in_buf)) == 0);
            }
            weft_ort_value_free(rs, rv_in);
            weft_ort_value_free(rs, rv_out);
            weft_ort_session_close(rs);
        }
    } else {
        GATE_SKIP("AC-O15", "no libonnxruntime (real leg declared)");
        GATE_SKIP("AC-O16", "no libonnxruntime");
        GATE_SKIP("AC-O17", "no libonnxruntime");
    }

    // teardown
    weft_ort_value_free(s, in_value);
    weft_ort_value_free(s, out_value);
    weft_ort_session_close(s);
    weft_ort_unload(mrt);
    if (rt) weft_ort_unload(rt);

    GATE_SUMMARY("AC-O (onnx bridge)");
}
