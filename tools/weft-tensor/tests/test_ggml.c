// test_ggml.c — the AC-G series: the ggml bridge's plan layer (dtype
// map, ggml geometry order, placement planner, audio windowing) + the
// runtime leg (dlopen + ABI probe verdict, DECLARED where libggml is
// absent).

#include "weft_test_util.h"
#include "weft/weft_tensor_view.h"

#include "backends/ggml/weft_ggml_bridge.h"

#include <stdalign.h>
#include <string.h>

static alignas(64) uint8_t g_span[8192];

int main(void) {
    // ---- G1: the dtype map (named refusals, no unsigned fiction) -------
    GATE("AC-G1 WTS1 -> ggml dtype map",
         weft_ggml_type_code(WEFT_TENSOR_F32) == 0 &&
             weft_ggml_type_code(WEFT_TENSOR_F16) == 1 &&
             weft_ggml_type_code(WEFT_TENSOR_I8) == 24 &&
             weft_ggml_type_code(WEFT_TENSOR_I32) == 26 &&
             weft_ggml_type_code(WEFT_TENSOR_F64) == 28);
    GATE("AC-G2 unsigned dtypes refuse (ggml has none)",
         weft_ggml_type_code(WEFT_TENSOR_U8) == -1 &&
             weft_ggml_type_code(WEFT_TENSOR_U32) == -1);

    // ---- G3: the ggml geometry order (dims reversed, nb recurrence) ---
    {
        weft_tensor_view_t v;
        uint32_t dims[3] = {2, 3, 4};   // row-major: 4 is innermost
        weft_ggml_shape_t sh;
        GATE("AC-G3 view dims map to ggml ne[] REVERSED",
             weft_tensor_view_init(&v, WEFT_TENSOR_F32, dims, 3, g_span,
                                   0) == WEFT_TV_OK &&
                 weft_ggml_shape_of_view(&v, &sh) == 0 &&
                 sh.ne[0] == 4 && sh.ne[1] == 3 && sh.ne[2] == 2 &&
                 sh.ne[3] == 1);
        GATE("AC-G4 nb strides follow the ggml recurrence",
             sh.nb[0] == 4 && sh.nb[1] == 16 && sh.nb[2] == 48 &&
                 sh.nb[3] == 96);
        GATE("AC-G5 nbytes == nb[3] * ne[3] == view byte_len",
             sh.nbytes == 96 && sh.nbytes == v.byte_len);
    }
    {
        // f16 stereo audio: [2, 1024] -> ggml ne {1024, 2}
        weft_tensor_view_t v;
        uint32_t dims[2] = {2, 1024};
        weft_ggml_shape_t sh;
        GATE("AC-G6 f16 audio bank maps (ne[0]=1024, ne[1]=2)",
             weft_tensor_view_init(&v, WEFT_TENSOR_F16, dims, 2,
                                   (void*)(g_span + 64), 0) == WEFT_TV_OK &&
                 weft_ggml_shape_of_view(&v, &sh) == 0 &&
                 sh.ne[0] == 1024 && sh.ne[1] == 2 &&
                 sh.nb[1] == 2048 && sh.nbytes == 4096);
    }
    {
        // strided / BE views refuse
        weft_tensor_view_t sv;
        uint32_t sd[2] = {4, 8}, ss[2] = {16, 1};
        weft_ggml_shape_t sh;
        GATE("AC-G7 strided view refuses (the caller's view road)",
             weft_tensor_view_init_strided(&sv, WEFT_TENSOR_F32, sd, ss, 2,
                                           g_span, 0) == WEFT_TV_OK &&
                 weft_ggml_shape_of_view(&sv, &sh) != 0);
    }

    // ---- the placement planner (Law 1: deterministic, zero-alloc) ------
    weft_ggml_planner_t p;
    weft_ggml_planner_init(&p);
    GATE("AC-G8 planner alignment is the 64-byte law", p.align == 64);
    GATE("AC-G9 span registration",
         weft_ggml_planner_register(&p, g_span, sizeof(g_span)) == 0 &&
             p.n == 1);
    GATE("AC-G10 misaligned span registration refuses",
         weft_ggml_planner_register(&p, (void*)(g_span + 8), 4096) == -1);
    {
        int span = -1;
        uint64_t off = 0;
        GATE("AC-G11 first placement lands at offset 0",
             weft_ggml_planner_place(&p, 96, &span, &off) == 0 &&
                 span == 0 && off == 0);
        // 96 used -> next 64-aligned offset is 128
        GATE("AC-G12 second placement 64-aligns",
             weft_ggml_planner_place(&p, 32, &span, &off) == 0 &&
                 off == 128);
        // explicit placement: aligned ok, misaligned refuses
        GATE("AC-G13 explicit placement enforces the alignment law",
             weft_ggml_planner_place_at(&p, 0, 256, 64) == 0 &&
                 weft_ggml_planner_place_at(&p, 0, 260, 64) == -1);
        // out-of-bounds explicit placement refuses
        GATE("AC-G14 out-of-bounds placement refuses",
             weft_ggml_planner_place_at(&p, 0, sizeof(g_span) - 32, 64) ==
                 -1);
        // overflow the span -> refusal counted
        uint64_t refused_before = p.refused;
        GATE("AC-G15 full-span refusal is counted, never silent",
             weft_ggml_planner_place(&p, sizeof(g_span), &span, &off) ==
                     -1 &&
                 p.refused == refused_before + 1);
        // reset rewinds to the epoch start
        weft_ggml_planner_reset(&p);
        GATE("AC-G16 epoch reset rewinds the cursor",
             weft_ggml_planner_place(&p, 96, &span, &off) == 0 && off == 0);
    }

    // ---- the audio windowing (Whisper-style) ---------------------------
    {
        weft_ggml_audio_win_t wins[16];
        uint32_t n = 0;
        // 1000 samples, window 400, hop 200: offsets 0,200,400,600 ->
        // full windows; offset 800 leaves 200 < 400 (partial, dropped)
        GATE("AC-G17 windowing drops the partial tail by policy",
             weft_ggml_audio_plan(1000, 400, 200, 0, wins, 16, &n) == 0 &&
                 n == 4 && wins[0].offset_elems == 0 &&
                 wins[3].offset_elems == 600 && wins[3].partial == 0);
        // allow_partial keeps the tail, marked
        GATE("AC-G18 allow_partial keeps the tail, labeled",
             weft_ggml_audio_plan(1000, 400, 200, 1, wins, 16, &n) == 0 &&
                 n == 5 && wins[4].partial == 1 &&
                 wins[4].n_elems == 200);
        // hop > window refuses (would drop samples)
        GATE("AC-G19 hop > window refuses",
             weft_ggml_audio_plan(1000, 200, 400, 1, wins, 16, &n) == -1);
        // exact-fit windows
        GATE("AC-G20 exact multiples produce clean windows",
             weft_ggml_audio_plan(1600, 400, 400, 0, wins, 16, &n) == 0 &&
                 n == 4 && wins[3].offset_elems == 1200);
    }

    // ---- the runtime leg (dlopen + the ABI probe verdict) ---------------
    weft_ggml_rt_t* rt = NULL;
    weft_ggml_err_t e = weft_ggml_rt_load(&rt);
    if (e != WEFT_GGML_ERR_OK) {
        GATE("AC-G21 no-libggml refusal is named",
             e == WEFT_GGML_ERR_NO_LIB || e == WEFT_GGML_ERR_NO_SYMBOLS);
        printf("  (%s — the plan layer is the gated contract)\n",
               weft_ggml_err_name(e));
        GATE_SKIP("AC-G22", "no libggml (runtime leg declared)");
    } else {
        printf("  (%s)\n", weft_ggml_rt_report(rt));
        GATE("AC-G22 runtime probe reports a DEFINITE verdict",
             rt != NULL && strlen(weft_ggml_rt_report(rt)) > 0);
        // the data-pointer road on a registered span
        weft_ggml_planner_t p2;
        weft_ggml_planner_init(&p2);
        weft_ggml_planner_register(&p2, g_span, sizeof(g_span));
        weft_tensor_view_t v;
        static float samples[64];
        uint32_t dims[1] = {64};
        void* data = NULL;
        weft_ggml_shape_t sh;
        weft_tensor_view_init(&v, WEFT_TENSOR_F32, dims, 1, samples, 0);
        e = weft_ggml_place_view(rt, &p2, &v, &data, &sh);
        GATE("AC-G23 data-pointer road places a view in the span",
             e == WEFT_GGML_ERR_OK && data >= (void*)g_span &&
                 data < (void*)(g_span + sizeof(g_span)) &&
                 sh.nbytes == 256);
        weft_ggml_rt_unload(rt);
    }

    GATE_SUMMARY("AC-G (ggml bridge)");
}
