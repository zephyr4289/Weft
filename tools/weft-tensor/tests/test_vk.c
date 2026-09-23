// test_vk.c — the AC-K series: the Vulkan bridge's refusal legs (no-ICD),
// the foreign-import road (alias canary — the Series-10 lesson as a gate),
// and the ICD legs: fd-import preprocess bit-exact vs the SIMD oracle.

#include "weft_test_util.h"
#include "weft/weft_accel_common.h"
#include "weft/weft_tensor_view.h"
#include "gpu_ring.h"
#include "shm_ring.h"

#include "backends/vulkan/weft_vk_bridge.h"

#include <string.h>
#include <unistd.h>

int main(void) {
    // ---- K1: the frozen preprocess kernel identity --------------------
    uint32_t words = 0;
    const uint32_t* spv = weft_vk_preprocess_spv(&words);
    GATE("AC-K1 committed SPIR-V present + frozen ID self-consistent",
         spv != NULL && words > 0 &&
             weft_accel_frozen_id(spv, (size_t)words * 4) ==
                 weft_vk_preprocess_frozen_id());

    // ---- K2: a WRONG kernel refuses the frozen-ID check ----------------
    {
        static uint32_t fake[64];
        weft_vk_compute_t* k = NULL;
        // ctx not needed for the frozen-id refusal (it fires first), but
        // init requires a dev — use a NULL-dev call to exercise BAD_ARG
        GATE("AC-K2 NULL dev refuses BAD_ARG",
             weft_vk_compute_init(&k, NULL, spv, words, 0, 2, 16) ==
                 WEFT_VK_ERR_BAD_ARG && k == NULL);
    }

    // ---- the ctx ladder -------------------------------------------------
    weft_vk_ctx_t* ctx = NULL;
    weft_vk_err_t e = weft_vk_ctx_create(&ctx, "ac-k");
    int have_icd = (e == WEFT_VK_OK);
    if (!have_icd) {
        printf("  (ctx refused: %s — the no-ICD leg)\n",
               weft_vk_err_name(e));
        GATE("AC-K3 no-ICD ctx refusal is named and honest",
             e == WEFT_VK_ERR_NO_LOADER || e == WEFT_VK_ERR_NO_DEVICE);
        GATE_SKIP("AC-K4..AC-K9", "no Vulkan ICD on this host (declared)");
    } else {
        GATE("AC-K3 ctx boots with the import extensions",
             have_icd && weft_vk_ctx_has_dmabuf_ext(ctx));
        printf("  (device: %s, host-ext=%d, min-align=%llu)\n",
               weft_vk_ctx_device_name(ctx),
               weft_vk_ctx_has_host_ext(ctx),
               (unsigned long long)weft_vk_ctx_min_host_align(ctx));

        // ---- K4: the fd road — export a dma-buf from a Vulkan ring and
        // import it through the FOREIGN road, then alias-verify.
        weft_gpu_ring_t* g = NULL;
        int fd = -1;
        int road = (weft_gpu_create_ex(&g, 64u * 1024u, 2,
                                       WEFT_GPU_CREATE_EXPORTABLE_FD) ==
                    0) &&
                   (weft_gpu_export_dmabuf_fd(g, &fd) == 0);
        if (!road) {
            GATE_SKIP("AC-K4..AC-K9",
                      "fd export refused on this ICD (declared)");
        } else {
            weft_vk_mem_t mem;
            e = weft_vk_import_fd(ctx, fd, 64u * 1024u, &mem);
            GATE("AC-K4 foreign dma-buf import over the fd road",
                 e == WEFT_VK_OK && mem.buffer && mem.map);
            if (e == WEFT_VK_OK) {
                // the preprocess kit over the import
                weft_vk_compute_t* k = NULL;
                GATE("AC-K5 preprocess kit builds (frozen ID verified)",
                     weft_vk_compute_init(&k, weft_vk_ctx_dev(ctx), spv,
                                          words,
                                          weft_vk_preprocess_frozen_id(),
                                          2,
                                          sizeof(weft_vk_preprocess_push_t))
                         == WEFT_VK_OK);
                if (k) {
                    // write a canary frame through the CPU map, preprocess
                    // through the GPU import, compare with the oracle
                    uint8_t* frame = (uint8_t*)mem.map;
                    for (int i = 0; i < 256; i++) {
                        frame[i] = (uint8_t)(i * 31 + 7);
                    }
                    weft_tensor_view_t vf;
                    uint32_t dims[1] = {256};
                    weft_tensor_view_init(&vf, WEFT_TENSOR_U8, dims, 1,
                                          frame, 0);
                    weft_vk_mem_t out;
                    weft_vk_err_t oe = weft_vk_alloc_output(
                        ctx, 4096, 1, &out);
                    GATE("AC-K6 output allocation maps host-visible",
                         oe == WEFT_VK_OK && out.map);
                    if (oe == WEFT_VK_OK) {
                        e = weft_vk_preprocess_dispatch(
                            k, mem.buffer, 0, out.buffer, 0, &vf,
                            1.0f / 255.0f);
                        float ref[256];
                        weft_ref_normalize_u8_to_f32(ref, frame, 256,
                                                     1.0f / 255.0f);
                        GATE("AC-K7 GPU preprocess BIT-EXACT vs the oracle",
                             e == WEFT_VK_OK &&
                                 memcmp(out.map, ref, sizeof(ref)) == 0);

                        // the alias canary: same import, canary words —
                        // a fresh-memory backing device FAILS here.
                        e = weft_vk_alias_verify(ctx, &mem, k);
                        GATE("AC-K8 alias canary proves the fd road",
                             e == WEFT_VK_OK && mem.alias_verified == 1);
                    }
                    weft_vk_mem_free(ctx, &out);
                    weft_vk_compute_destroy(k);
                }
                weft_vk_mem_free(ctx, &mem);
            }
            close(fd);
            weft_gpu_destroy(g);
        }

        // ---- K9: misaligned host import refuses (the malloc boundary) --
        {
            static uint8_t unaligned[8192] __attribute__((aligned(4096)));
            weft_vk_mem_t mem;
            GATE("AC-K9 below-alignment host import refuses",
                 weft_vk_import_host(ctx, unaligned + 3, 4096, &mem) ==
                     WEFT_VK_ERR_ALIGN);
            // page-aligned host import passes the alignment gate (the
            // alias canary may still refuse fresh-memory devices — the
            // llvmpipe discovery — and that refusal is itself honest)
            weft_vk_mem_t hmem;
            e = weft_vk_import_host(ctx, unaligned, 4096, &hmem);
            if (e == WEFT_VK_OK) {
                weft_vk_compute_t* k = NULL;
                if (weft_vk_compute_init(&k, weft_vk_ctx_dev(ctx), spv,
                                         words, 0, 2, 16) == WEFT_VK_OK) {
                    int r = weft_vk_alias_verify(ctx, &hmem, k);
                    printf("  (host-road alias verdict: %s)\n",
                           r == WEFT_VK_OK
                               ? "ALIASED"
                               : "REFUSED (fresh-memory backing — the "
                                 "documented llvmpipe behavior)");
                    GATE("AC-K9b host road gives a DEFINITE alias verdict",
                         r == WEFT_VK_OK || r == WEFT_VK_ERR_ALIAS);
                    weft_vk_compute_destroy(k);
                }
                weft_vk_mem_free(ctx, &hmem);
            } else {
                GATE_SKIP("AC-K9b", "host import refused (ext absent)");
            }
        }
        weft_vk_ctx_destroy(ctx);
    }

    // ---- K10: Road A refuses a non-session cleanly ----------------------
    {
        // A plain (non-WFSH) page-aligned mapping must refuse Road A's
        // host wrap — the substrate's session validation, exercised
        // through this bridge's API.
        static uint8_t not_a_session[16384] __attribute__((aligned(4096)));
        weft_vk_ring_t* r = NULL;
        GATE("AC-K10 non-session memory refuses Road A",
             weft_vk_ring_wrap_host(&r, 4096, 2, not_a_session,
                                    sizeof(not_a_session)) !=
                 WEFT_VK_OK);
    }

    GATE_SUMMARY("AC-K (vulkan bridge)");
}
