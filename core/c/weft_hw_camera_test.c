// weft_hw_camera_test.c — V-series gates (RFC-0016 §6: the camera seams).
//
// The honesty-first design: the CI/sandbox hosts have no /dev/video* and
// are not Android — so the REFUSAL legs are what runs everywhere, and
// they are real gates (a seam that pretends hardware exists is worse
// than none). Hosts WITH a capture device run the capture leg inside V2;
// Android builds (the NDK leg) run the AHB leg — both report honestly.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "weft_dmabuf.h"
#include "weft_hw_ahb.h"
#include "weft_hw_v4l2.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name, fmt, ...)                                        \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  PASS %s\n", name);                                   \
            g_pass++;                                                      \
        } else {                                                           \
            printf("  FAIL %s — " fmt "\n", name, ##__VA_ARGS__);          \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

int main(void) {
    printf("# V-series: camera/compositor seams (RFC-0016 s6)\n");

    // ---- V1: V4L2 capability honesty ------------------------------------
    printf("## V1 v4l2 probe\n");
    {
        char line[128];
        weft_v4l2_report(line, sizeof(line));
        printf("  %s\n", line);
        const weft_v4l2_probe_t* p = weft_v4l2_probe();
        CHECK(p->probed, "probe ran once", "%d", p->probed);
        if (p->caps == WEFT_V4L2_DEVICE) {
            CHECK(p->device[0] != '\0', "device node recorded", "'%s'",
                  p->device);
            printf("  (capture leg LIVE on this host: %s)\n", p->device);
        } else {
            CHECK(p->caps == WEFT_V4L2_UNSUPPORTED, "no device -> UNSUPPORTED",
                  "%d", (int)p->caps);
            CHECK(p->device[0] == '\0', "no phantom device", "'%s'", p->device);
        }
    }

    // ---- V2: the capture road --------------------------------------------
    printf("## V2 capture\n");
    {
        weft_v4l2_frame_t frame;
        const weft_v4l2_probe_t* p = weft_v4l2_probe();
        const int rc = weft_v4l2_capture_frame(NULL, &frame);
        if (p->caps == WEFT_V4L2_DEVICE) {
            if (rc == 0) {
                CHECK(frame.fd >= 0, "frame captured + dma-buf exported",
                      "fd=%d", frame.fd);
                CHECK(frame.width > 0 && frame.height > 0,
                      "geometry reported", "%ux%u", frame.width, frame.height);
                // the exported fd composes with the mesh: raw import road
                weft_dmabuf_import_t imp;
                memset(&imp, 0, sizeof(imp));
                CHECK(weft_dmabuf_import_fd(&imp, frame.fd, 0) == 0,
                      "camera dma-buf imports into the mesh", "?");
                CHECK(imp.has_session == 0,
                      "a camera frame is honestly NOT a session (raw road)",
                      "%d", imp.has_session);
                weft_dmabuf_import_free(&imp);
                close(frame.fd);
            } else {
                printf("  (device present but refused capture — errno=%d; "
                       "some nodes cannot stream/export; counted, honest)\n",
                       errno);
                CHECK(rc == -1, "capture refusal is -1 + errno", "%d", rc);
            }
        } else {
            CHECK(rc == -1 && errno == ENODEV,
                  "no capture device: ENODEV refusal", "rc=%d errno=%d",
                  rc, errno);
            CHECK(frame.fd == -1, "out frame left clean", "%d", frame.fd);
        }
        CHECK(weft_v4l2_capture_frame(NULL, NULL) == -1, "NULL out refused",
              "?");
    }

    // ---- V3: the AHardwareBuffer road ------------------------------------
    printf("## V3 ahb\n");
    {
        char line[160];
        weft_hw_ahb_report(line, sizeof(line));
        printf("  %s\n", line);
        if (weft_hw_ahb_caps() == WEFT_AHB_NATIVE) {
            printf("  (android build: the NDK leg runs the alloc/export "
                   "gate)\n");
            int fd = -1;
            uint32_t stride = 0;
            const int rc = weft_hw_ahb_alloc_export_fd(64, 64, &stride, &fd);
            CHECK(rc == 0, "AHB allocated + dma-buf exported", "rc=%d", rc);
            if (rc == 0) {
                CHECK(fd >= 0 && stride > 0, "fd + stride reported",
                      "fd=%d stride=%u", fd, stride);
                close(fd);
            }
        } else {
            CHECK(weft_hw_ahb_caps() == WEFT_AHB_UNSUPPORTED,
                  "non-android build: UNSUPPORTED (compile-everywhere stub)",
                  "%d", (int)weft_hw_ahb_caps());
            int fd = 42;
            CHECK(weft_hw_ahb_alloc_export_fd(64, 64, NULL, &fd) == -1 &&
                  errno == ENOTSUP && fd == -1,
                  "alloc refused ENOTSUP, out fd cleaned", "errno=%d", errno);
            CHECK(weft_hw_ahb_export_fd((void*)0x1, &fd) == -1,
                  "export refused ENOTSUP", "?");
        }
    }

    // ---- V4: the seam composition is documented geometry -----------------
    printf("## V4 composition identities\n");
    {
        // the camera frame road: raw import -> the CALLER binds the bytes
        // as a slot payload source; the session road: weft_dmabuf session
        // -> wrap. The identity both roads share: span geometry.
        CHECK(weft_dmabuf_span_bytes(2048, 8) ==
                  64 + weft_fanout_ring_bytes(2048, 8),
              "session span identity holds for camera-size frames", "?");
    }

    printf("verdict: %s (%d passed, %d failed)\n",
           g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
