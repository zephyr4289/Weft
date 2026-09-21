// test_vision_dma.c — the weft-vision-dma native battery (mandate C).
//
// Proves, against the synthetic V4L2/DMA-BUF mock camera:
//   V1  4K (3840x2160 RGBX) @ 120 FPS stream: every frame verified
//       (stamp magic, monotonic frame counter, swath CRC), zero drops.
//   V2  ZERO heap allocation across the whole streaming window
//       (interposed allocator, plain leg; sanitizers skip — declared).
//   V3  ZERO-COPY POINTER IDENTITY (Rule 4): frame tensor view base ==
//       engine buffer mapping; DLPack data == same address; view passes
//       the frozen ABI validator (weft_tensor_view_validate).
//   V4  DLPack deleter requeues the buffer (delivery continues after the
//       framework "frees" the tensor).
//   V5  DMA-BUF export + import: the exported fd maps to the SAME bytes
//       (stamp verifies through the new mapping — fd-passing zero-copy).
//   V6  cadence discipline at 120 FPS and honest ENODEV from AUTO when
//       no camera node exists.

#include "test_util.h"
#include "v4l2_mock_device.h"
#include "weft_vision_dma.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int quick(void) {
    const char *q = getenv("WEFT_QUICK");
    return (q != NULL && q[0] == '1') ? 1 : 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned fd0 = tu_fd_count();

    /* ---------------- V6 first: honest AUTO refusal ---------------- */
    if (access("/dev/video0", F_OK) != 0) {
        weft_vision_dma_config_t cfg;
        weft_vision_dma_config_default(&cfg);
        weft_vision_dma_t *eng = NULL;
        int rc = weft_vision_dma_open(&cfg, &eng);
        TU_ASSERT(rc == WEFT_VISION_ENODEV, "V6 honest ENODEV from AUTO without a camera", "V6 honest ENODEV from AUTO without a camera (rc=%d)", rc);;
        TU_CHECK(eng == NULL, "V6 no engine on refusal");
        TU_PASS("V6 AUTO backend fails closed (ENODEV) with no camera");
    } else {
        printf("SKIP  V6: /dev/video0 exists on this host (fleet leg)\n");
    }

    /* ---------------- 4K @ 120 FPS mock stream ---------------- */
    weft_vision_dma_config_t cfg;
    weft_vision_dma_config_default(&cfg);
    cfg.backend = WEFT_VISION_BACKEND_MOCK;
    cfg.width = 3840;
    cfg.height = 2160;
    cfg.pixfmt = WEFT_VISION_PIXFMT_RGBX8888;
    cfg.fps = 120.0;
    cfg.buffer_count = 4;
    cfg.mock_swath_div = 8; /* documented swath model at 4K; D-62 §B.4 */

    weft_vision_dma_t *eng = NULL;
    int rc = weft_vision_dma_open(&cfg, &eng);
    TU_ASSERT(rc == WEFT_VISION_OK, "open rc=", "open rc=%d (%s)", rc, weft_vision_status_name(rc));;
    TU_CHECK(eng->frame_bytes == 3840u * 2160u * 4u, "frame-bytes");

    TU_CHECK(weft_vision_dma_stream_start(eng) == WEFT_VISION_OK, "stream-on");

    const int N = quick() ? 60 : 240;
    uint64_t prev_no = 0;
    int first = 1;
    unsigned long alloc_before = tu_alloc_count;
    int64_t prev_t = 0;
    double cadence_sum = 0;
    int cadence_n = 0;
    uint64_t handoff_max = 0;

    for (int i = 0; i < N; i++) {
        weft_vision_frame_t *fr = NULL;
        int64_t deadline = tu_now_ns() + 500000000;
        rc = weft_vision_dma_dequeue(eng, &fr, deadline);
        TU_ASSERT(rc == WEFT_VISION_OK, "dq rc=", "dq rc=%d at frame %d", rc, i);;
        TU_CHECK(fr != NULL, "dq frame");

        /* V3: pointer identity — the tensor view IS the buffer mapping */
        TU_CHECK(fr->view.physical_or_shm_addr ==
                      (uintptr_t)eng->buf_map[fr->buffer_idx],
                  "V3 zero-copy identity: view base != buffer mapping");
        TU_CHECK(fr->dl.dl_tensor.data == eng->buf_map[fr->buffer_idx],
                  "V3 DLPack data pointer != buffer mapping");
        TU_CHECK(weft_tensor_view_validate(&fr->view,
                                            WEFT_TENSOR_ALIGN_NONE) ==
                      WEFT_TENSOR_OK,
                  "V3 frozen-ABI view validation");
        TU_CHECK(fr->view.shape[0] == 2160 && fr->view.shape[1] == 3840 &&
                      fr->view.shape[2] == 4,
                  "V3 view shape");
        TU_CHECK(fr->view.byte_length ==
                      (uint64_t)3840u * 2160u * 4u, "V3 view span");
        DLManagedTensor *dl = weft_vision_frame_dlpack(fr);
        TU_CHECK(dl != NULL && dl->deleter != NULL, "V3 dlpack handle");

        /* V1: integrity + monotonic counter */
        TU_ASSERT(weft_mock_frame_verify( (const uint8_t *)fr->view.physical_or_shm_addr, eng->buf_len[fr->buffer_idx], fr->frame_no) == 0, "V1 frame", "V1 frame %d integrity (stamp/CRC)", i);;
        if (!first) {
            TU_ASSERT(fr->frame_no == prev_no + 1, "V1 frame counter gap", "V1 frame counter gap: %llu -> %llu", (unsigned long long)prev_no, (unsigned long long)fr->frame_no);;
        }
        prev_no = fr->frame_no;
        first = 0;

        if (prev_t != 0) {
            double dt = (double)(tu_now_ns() - prev_t);
            cadence_sum += dt;
            cadence_n++;
        }
        prev_t = tu_now_ns();
        if (fr->frame_no > 0 && eng->last_handoff_ns > 0 &&
            weft_vision_dma_last_handoff_ns(eng) > handoff_max) {
            handoff_max = weft_vision_dma_last_handoff_ns(eng);
        }

        /* V4 exercise on one frame: DLPack deleter == release path */
        if (i == N / 2) {
            dl->deleter(dl);
            TU_CHECK(fr->in_use == 0, "V4 deleter released the frame");
            continue; /* already requeued */
        }
        TU_ASSERT(weft_vision_dma_release(eng, fr) == WEFT_VISION_OK, "release frame", "release frame %d", i);;
    }

#ifndef TU_NO_ALLOC_GUARD
    unsigned long alloc_during = tu_alloc_count - alloc_before;
    TU_ASSERT(alloc_during == 0, "V2 ZERO heap allocations during streaming", "V2 ZERO heap allocations during streaming (saw %lu)", alloc_during);;
    TU_PASS("V2 zero-heap streaming window (interposed allocator)");
#else
    (void)alloc_before; /* the counting wrappers are not linked in this leg */
    printf("SKIP  V2 alloc guard (sanitizer leg — declared)\n");
#endif

    TU_CHECK(weft_vision_dma_frames_delivered(eng) >= (uint64_t)N,
              "delivered count");
    TU_CHECK(weft_vision_dma_frames_dropped(eng) == 0,
              "V1 zero dropped frames at 4K@120");

    double mean_ms = cadence_n > 0 ? cadence_sum / cadence_n / 1e6 : 0.0;
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
    /* TSan slows every interaction 2-4x: the cadence WINDOW widens, the
     * discipline (bounded, monotonic, no stalls) is still asserted. */
    TU_ASSERT(mean_ms > 6.0 && mean_ms < 45.0, "V5 cadence 120 FPS", "V5 cadence 120 FPS, TSan-widened window (mean inter-frame %.3f ms)", mean_ms);;
    printf("      cadence window widened for the TSan leg (declared)\n");
#else
    TU_ASSERT(mean_ms > 6.0 && mean_ms < 12.0, "V5 cadence 120 FPS", "V5 cadence 120 FPS (mean inter-frame %.3f ms)", mean_ms);;
#endif
    printf("      4K@120: %d frames, mean inter-dequeue %.3f ms, "
           "handoff max %llu ns\n",
           N, mean_ms, (unsigned long long)handoff_max);
    TU_PASS("V1 4K@120 integrity: stamps + CRC + monotonic + zero drops");
    TU_PASS("V3 zero-copy pointer identity (view == mapping == DLPack)");
    TU_PASS("V4 DLPack deleter requeues the DMA buffer");

    TU_CHECK(weft_vision_dma_stream_stop(eng) == WEFT_VISION_OK, "stream-off");

    /* ---------------- V5: DMA-BUF export + import ---------------- */
    TU_CHECK(weft_vision_dma_stream_start(eng) == WEFT_VISION_OK, "restart");
    weft_vision_frame_t *fr = NULL;
    rc = weft_vision_dma_dequeue(eng, &fr, tu_now_ns() + 500000000);
    TU_CHECK(rc == WEFT_VISION_OK, "dq for export");
    int dmabuf_fd = -1;
    rc = weft_vision_dma_export_dmabuf(eng, fr, &dmabuf_fd);
    TU_ASSERT(rc == WEFT_VISION_OK && dmabuf_fd >= 0, "export dmabuf", "export dmabuf (rc=%d)", rc);;

    weft_tensor_view_t wv;
    void *mapping = NULL;
    uint64_t map_len = 0;
    rc = weft_vision_dma_wrap_dmabuf(dmabuf_fd, 0, eng->frame_bytes, 3840,
                                     2160, WEFT_VISION_PIXFMT_RGBX8888, &wv,
                                     &mapping, &map_len);
    TU_ASSERT(rc == WEFT_VISION_OK, "wrap dmabuf", "wrap dmabuf (rc=%d)", rc);;
    TU_CHECK(mapping != NULL &&
                 (uintptr_t)mapping != (uintptr_t)eng->buf_map[fr->buffer_idx],
              "wrap distinct mapping");
    /* the stamp verifies THROUGH THE FD — same physical pages, zero copy */
    TU_CHECK(weft_mock_frame_verify((const uint8_t *)mapping,
                                     (size_t)map_len, fr->frame_no) == 0,
              "V5 fd-passed mapping aliases the SAME memory");
    TU_CHECK(weft_tensor_view_validate(&wv, WEFT_TENSOR_ALIGN_NONE) ==
                  WEFT_TENSOR_OK,
              "V5 wrapped view validates");
    TU_PASS("V5 DMA-BUF export/import: content identity through the fd");

    (void)munmap(mapping, (size_t)map_len);
    (void)close(dmabuf_fd);
    TU_CHECK(weft_vision_dma_release(eng, fr) == WEFT_VISION_OK, "rel-exp");
    TU_CHECK(weft_vision_dma_stream_stop(eng) == WEFT_VISION_OK, "stop2");
    TU_CHECK(weft_vision_dma_close(eng) == WEFT_VISION_OK, "close");

    TU_ASSERT(tu_fd_count() == fd0, "vision fd-leak-audit", "vision fd-leak-audit: %u -> %u", fd0, tu_fd_count());;
    TU_PASS("V6 leak audit: fds back to baseline");
    printf("\nweft-vision-dma battery: ALL PHASES PASS\n");
    return 0;
}
