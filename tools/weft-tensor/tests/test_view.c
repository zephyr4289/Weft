// test_view.c — the AC-V series: the weft_tensor_view refusal ladder,
// construction, geometry math, GPU-readiness, and the WTS1 compose seam.

#include "weft_test_util.h"
#include "weft/weft_tensor_view.h"
#include "weft/weft_accel_common.h"

#include <stdalign.h>
#include <string.h>

static alignas(64) uint8_t g_buf[4096];

int main(void) {
    weft_tensor_view_t v;

    // ---- construction + the happy ladder ------------------------------
    uint32_t dims[3] = {4, 8, 4};   // rank-3, 128 elements
    GATE("AC-V1 sizeof/align ABI v1",
         sizeof(weft_tensor_view_t) == 128 &&
             alignof(weft_tensor_view_t) == 64);
    GATE("AC-V2 init builds a PACKED view",
         weft_tensor_view_init(&v, WEFT_TENSOR_F32, dims, 3, g_buf,
                               0x8F4C1120A9B30012ull) == WEFT_TV_OK &&
             (v.flags & WEFT_TENSOR_VIEW_F_PACKED) &&
             v.elem_count == 128 && v.byte_len == 512 &&
             v.schema_id == 0x8F4C1120A9B30012ull);
    GATE("AC-V3 canonical strides are row-major",
         v.strides[0] == 32 && v.strides[1] == 4 && v.strides[2] == 1);

    // ---- the refusal ladder (every anomaly named) ---------------------
    weft_tensor_view_t bad = v;
    bad.magic = 0xDEADBEEF;
    GATE("AC-V4 bad magic refuses", weft_tensor_view_validate(&bad, 0) ==
                                       WEFT_TV_ERR_MAGIC);
    bad = v; bad.abi_version = 99;
    GATE("AC-V5 future ABI refuses",
         weft_tensor_view_validate(&bad, 0) == WEFT_TV_ERR_ABI);
    bad = v; bad.dtype = 200;
    GATE("AC-V6 dtype outside the dialect refuses",
         weft_tensor_view_validate(&bad, 0) == WEFT_TV_ERR_DTYPE);
    bad = v; bad.rank = 0;
    GATE("AC-V7 rank 0 refuses",
         weft_tensor_view_validate(&bad, 0) == WEFT_TV_ERR_RANK);
    bad = v; bad.elem_count = 127;
    GATE("AC-V8 elem_count != prod(dims) refuses",
         weft_tensor_view_validate(&bad, 0) == WEFT_TV_ERR_COUNT);
    bad = v; bad.data = NULL;
    GATE("AC-V9 NULL data refuses",
         weft_tensor_view_validate(&bad, 0) == WEFT_TV_ERR_PTR);
    bad = v; bad.flags |= 0x80u;
    GATE("AC-V10 unknown flag bits refuse",
         weft_tensor_view_validate(&bad, 0) == WEFT_TV_ERR_FLAGS);
    bad = v; bad.flags |= WEFT_TENSOR_VIEW_F_BIG_ENDIAN;
    GATE("AC-V11 big-endian refuses gpu_ready",
         weft_tensor_view_gpu_ready(&bad) == WEFT_TV_ERR_ENDIAN);
    bad = v; bad.byte_len = 16;  // span exceeds the claimed length
    GATE("AC-V12 geometry beyond byte_len refuses",
         weft_tensor_view_validate(&bad, 0) == WEFT_TV_ERR_SPAN);
    GATE("AC-V13 capacity gate refuses over-run",
         weft_tensor_view_validate(&v, 256) == WEFT_TV_ERR_CAPACITY);
    GATE("AC-V14 full capacity passes",
         weft_tensor_view_validate(&v, 512) == WEFT_TV_OK);

    // PACKED claimed but strides are not canonical (a hand-built lie —
    // the constructors never produce this; the ladder still catches it)
    {
        weft_tensor_view_t lie;
        weft_tensor_view_init(&lie, WEFT_TENSOR_F32, dims, 3, g_buf, 0);
        lie.strides[0] = 999;  // keep the PACKED flag, break the strides
        GATE("AC-V15 PACKED flag with non-canonical strides refuses",
             weft_tensor_view_validate(&lie, 0) == WEFT_TV_ERR_STRIDES);
    }
    // strided-but-honest views pass and drop the PACKED claim (their
    // byte_len is the strided REACH, not the packed product)
    uint32_t plane_dims[2] = {4, 8};
    uint32_t plane_strides[2] = {64, 1};   // a plane inside a bigger bank
    weft_tensor_view_t sv;
    GATE("AC-V16 honest strided view passes (byte_len = strided reach)",
         weft_tensor_view_init_strided(&sv, WEFT_TENSOR_F32, plane_dims,
                                       plane_strides, 2, g_buf, 0) ==
             WEFT_TV_OK &&
             !(sv.flags & WEFT_TENSOR_VIEW_F_PACKED) &&
             sv.byte_len == ((4 - 1) * 64 + (8 - 1) * 1 + 1) * 4);

    // ---- geometry math --------------------------------------------------
    weft_tensor_view_t r4;
    uint32_t d4[4] = {2, 3, 4, 5};   // 120 elements
    weft_tensor_view_init(&r4, WEFT_TENSOR_U16, d4, 4, g_buf, 0);
    GATE("AC-V17 rank-4 canonical strides",
         r4.strides[0] == 60 && r4.strides[1] == 20 &&
             r4.strides[2] == 5 && r4.strides[3] == 1);
    GATE("AC-V18 offset math (strided multiply-add)",
         weft_tensor_view_offset(&r4, 1, 2, 3, 4) ==
             (size_t)(60 + 2 * 20 + 3 * 5 + 4) * 2);
    GATE("AC-V19 byte stride derivation",
         weft_tensor_view_byte_stride(&r4, 1) == 40);

    // ---- the gpu_ready alignment law (Law 2) ---------------------------
    weft_tensor_view_t mis;
    uint32_t d1[1] = {16};
    GATE("AC-V20 misaligned f32 pointer refuses gpu_ready",
         weft_tensor_view_init(&mis, WEFT_TENSOR_F32, d1, 1,
                               (void*)(g_buf + 8), 0) == WEFT_TV_OK &&
             weft_tensor_view_gpu_ready(&mis) != WEFT_TV_OK);
    weft_tensor_view_t al;
    GATE("AC-V21 16-byte-aligned f32 passes gpu_ready",
         weft_tensor_view_init(&al, WEFT_TENSOR_F32, d1, 1,
                               (void*)(g_buf + 16), 0) == WEFT_TV_OK &&
             weft_tensor_view_gpu_ready(&al) == WEFT_TV_OK);
    weft_tensor_view_t u8v;
    uint32_t d2[1] = {64};
    GATE("AC-V22 u8 rides the raw-byte road (alignment floor only)",
         weft_tensor_view_init(&u8v, WEFT_TENSOR_U8, d2, 1,
                               (void*)(g_buf + 1), 0) == WEFT_TV_OK &&
             weft_tensor_view_gpu_ready(&u8v) == WEFT_TV_OK);

    // ---- the dtype dialect seam (WTS1 numeric identity) ----------------
    GATE("AC-V23 dialect codes are the frozen WTS1 space",
         weft_tensor_view_elem_size(WEFT_TENSOR_U8) == 1 &&
             weft_tensor_view_elem_size(WEFT_TENSOR_F16) == 2 &&
             weft_tensor_view_elem_size(WEFT_TENSOR_F32) == 4 &&
             weft_tensor_view_elem_size(WEFT_TENSOR_F64) == 8 &&
             weft_tensor_view_elem_size((weft_tensor_dtype_t)11) == 0);

    // ---- WTS1 compose (the ring's in-band tensor stream) ----------------
#ifdef WEFT_TENSOR_HAVE_WTS1
    {
        // Build a WTS1 frame in a buffer, then view it.
        uint8_t frame[32 + 256];
        memset(frame, 0, sizeof(frame));
        uint32_t magic = WEFT_TENSOR_MAGIC;
        memcpy(frame, &magic, 4);
        frame[4] = 1;   // version
        frame[5] = 9;   // f32
        frame[6] = 1;   // rank
        frame[7] = 0;   // flags
        uint32_t elems = 64, words = 64;
        memcpy(frame + 8, &elems, 4);
        memcpy(frame + 12, &words, 4);
        uint32_t dim0 = 64;
        memcpy(frame + 16, &dim0, 4);
        weft_tensor_view_t wv;
        GATE("AC-V24 WTS1 frame -> view (zero transforms)",
             weft_tensor_view_of_wts1(&wv, frame, sizeof(frame)) ==
                 WEFT_TV_OK &&
                 wv.dtype == (uint8_t)WEFT_TENSOR_F32 &&
                 wv.elem_count == 64 && wv.byte_len == 256 &&
                 (wv.flags & WEFT_TENSOR_VIEW_F_PINNED) &&
                 wv.data == (void*)(frame + 32));
        // A corrupt frame refuses with the WTS1 ladder.
        frame[0] = 'X';
        GATE("AC-V25 corrupt WTS1 refuses (never a guess)",
             weft_tensor_view_of_wts1(&wv, frame, sizeof(frame)) ==
                 WEFT_TV_ERR_MAGIC);
    }
#else
    GATE_SKIP("AC-V24", "WTS1 module absent (pre-Series-10 base tree)");
    GATE_SKIP("AC-V25", "WTS1 module absent (pre-Series-10 base tree)");
#endif

    // ---- accel common: stats + frozen id + SIMD oracle -------------------
    {
        weft_accel_stats_t st, sc;
        weft_accel_stats_reset(&st);
        for (uint32_t i = 0; i < 999; i++) weft_accel_stats_add(&st, i + 1);
        GATE("AC-V26 stats percentiles (p50/p95/p99)",
             weft_accel_stats_count(&st) == 999 &&
                 weft_accel_stats_pct(&st, &sc, 50) == 500 &&
                 weft_accel_stats_pct(&st, &sc, 95) == 949 &&
                 weft_accel_stats_pct(&st, &sc, 99) == 989);
        uint64_t fid = weft_accel_frozen_id("abc", 3);
        GATE("AC-V27 frozen id is deterministic FNV-1a 64",
             fid == weft_accel_frozen_id("abc", 3) && fid != 0);

        // SIMD == scalar reference, BIT-EXACT (the one-multiply contract)
        static uint8_t src[1024];
        static float dst_simd[1024], dst_ref[1024];
        for (int i = 0; i < 1024; i++) src[i] = (uint8_t)(i * 7 + 3);
        weft_simd_normalize_u8_to_f32(dst_simd, src, 1024, 1.0f / 255.0f);
        weft_ref_normalize_u8_to_f32(dst_ref, src, 1024, 1.0f / 255.0f);
        GATE("AC-V28 SIMD normalize bit-exact vs scalar oracle",
             memcmp(dst_simd, dst_ref, sizeof(dst_simd)) == 0);
        weft_simd_copy_f32(dst_simd, dst_ref, 1024);
        GATE("AC-V29 SIMD copy round-trips",
             memcmp(dst_simd, dst_ref, sizeof(dst_simd)) == 0);
        printf("  (simd-isa=%s)\n", weft_simd_isa_name());
    }

    GATE_SUMMARY("AC-V (tensor view ABI)");
}
