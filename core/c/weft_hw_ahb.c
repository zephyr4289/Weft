// weft_hw_ahb.c — the Android AHardwareBuffer seam (RFC-0016 §6).

#include "weft_hw_ahb.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__ANDROID__)

#include <android/hardware_buffer.h>

weft_hw_ahb_caps_t weft_hw_ahb_caps(void) { return WEFT_AHB_NATIVE; }

size_t weft_hw_ahb_report(char* buf, size_t buflen) {
    int n = snprintf(buf, buflen, "ahb: native (libandroid linked)");
    return (n < 0) ? 0 : (size_t)n;
}

int weft_hw_ahb_export_fd(void* ahb, int* out_fd) {
    if (ahb == NULL || out_fd == NULL) { errno = EINVAL; return -1; }
    const native_handle_t* h = AHardwareBuffer_getNativeHandle(
        (const AHardwareBuffer*)ahb);
    // The native handle wraps the dma-buf fd(s); the first fd is the
    // buffer storage on every vendor implementation we target (the GBM/
    // gralloc contract). A handle with no fds is a vendor outlier —
    // refused honestly, never guessed at.
    if (h == NULL || h->numFds < 1) {
        errno = ENOTSUP;
        return -1;
    }
    *out_fd = dup(h->data[0]);  // ours to own; the caller closes it
    return (*out_fd >= 0) ? 0 : -1;
}

int weft_hw_ahb_alloc_export_fd(uint32_t width, uint32_t height,
                                uint32_t* out_stride_bytes, int* out_fd) {
    if (out_fd == NULL || width == 0 || height == 0) {
        errno = EINVAL;
        return -1;
    }
    AHardwareBuffer_Desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

    AHardwareBuffer* ahb = NULL;
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || ahb == NULL) {
        errno = ENOMEM;
        return -1;
    }
    int fd = -1;
    const int rc = weft_hw_ahb_export_fd(ahb, &fd);
    if (rc != 0) {
        AHardwareBuffer_release(ahb);
        return -1;
    }
    if (out_stride_bytes) {
        AHardwareBuffer_Desc got;
        memset(&got, 0, sizeof(got));
        AHardwareBuffer_describe(ahb, &got);
        *out_stride_bytes = got.stride * 4u;  // RGBA8888
    }
    AHardwareBuffer_release(ahb);  // the fd holds the storage alive
    *out_fd = fd;
    return 0;
}

#else  // !__ANDROID__ — the refusal stub (compile-everywhere road)

weft_hw_ahb_caps_t weft_hw_ahb_caps(void) { return WEFT_AHB_UNSUPPORTED; }

size_t weft_hw_ahb_report(char* buf, size_t buflen) {
    int n = snprintf(buf, buflen,
                     "ahb: unsupported (non-android build; the NDK leg "
                     "compiles this module via android/weft-core)");
    return (n < 0) ? 0 : (size_t)n;
}

int weft_hw_ahb_alloc_export_fd(uint32_t width, uint32_t height,
                                uint32_t* out_stride_bytes, int* out_fd) {
    (void)width; (void)height; (void)out_stride_bytes;
    if (out_fd) *out_fd = -1;
    errno = ENOTSUP;
    return -1;
}

int weft_hw_ahb_export_fd(void* ahb, int* out_fd) {
    (void)ahb;
    if (out_fd) *out_fd = -1;
    errno = ENOTSUP;
    return -1;
}

#endif  // __ANDROID__
