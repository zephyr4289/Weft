// weft_hw_ahb.h — RFC-0016 §6: the Android camera/compositor seam
// (AHardwareBuffer -> dma-buf fd -> the Weft mesh).
//
// WHY EXISTS: on Android the zero-copy currency between Camera2, SurfaceFlinger,
// and GPU compute is the AHardwareBuffer. Its underlying storage IS a
// dma-buf on every modern Android (AHardwareBuffer_getNativeHandle exposes
// it), which is exactly the fd weft_dmabuf binds and weft_gpu_wrap_dmabuf
// imports. This module is the JNI-side plumbing:
//
//   Camera2 ImageReader (USAGE_GPU_SAMPLED) -> AHardwareBuffer
//       -> weft_hw_ahb_export_fd()  -> dma-buf fd
//       -> weft_gpu_wrap_dmabuf()   -> GPU compute over the live frame
//       -> weft_dmabuf_import_fd()  -> CPU view when one is needed
//
// The Camera2 binding itself (ImageReader callbacks, USAGE flags) is the
// android/ Kotlin layer's policy — this is the mechanism under it, and the
// RFC §6 documents the composition. AVFoundation (macOS/iOS) takes the
// mirror-image road: CVPixelBuffer + IOSurface -> MTLBuffer (the Swift
// bridge in Sources/WeftSwiftUI/WeftMetalZeroCopy.swift) — the same
// "the buffer the producer made IS the buffer the mesh consumes" stance.
//
// LAW 4: non-Android builds compile the refusal stubs (ENOTSUP); Android
// builds link the NDK's libandroid (android/hardware_buffer.h — the ONE
// link-time dependency this driver-layer module carries, and only there).
// LAW 3: driver layer; weft.c/weft.h untouched.

#ifndef WEFT_HW_AHB_H
#define WEFT_HW_AHB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEFT_AHB_UNSUPPORTED = 0,  ///< non-Android build (the CI/sandbox state)
    WEFT_AHB_NATIVE = 1,       ///< AHardwareBuffer APIs linked
} weft_hw_ahb_caps_t;

/// Which build this is (compile-time — the one probe that cannot refuse
/// at runtime). Reported for evidence lines.
weft_hw_ahb_caps_t weft_hw_ahb_caps(void);

/// One capability line for evidence logs.
size_t weft_hw_ahb_report(char* buf, size_t buflen);

/// Allocate an AHardwareBuffer (RGBA_8888 w x h, GPU_SAMPLED|GPU_COLOR
/// usage — the camera/compositor interop set) and export its dma-buf fd.
/// The fd is OWNED BY THE CALLER (close(2)); the AHardwareBuffer is
/// released by this call (the fd holds the storage alive). Returns 0;
/// -1 with errno on refusal (non-Android, allocation failure, or the
/// platform refuses the native-handle export — vendor-specific, honest).
int weft_hw_ahb_alloc_export_fd(uint32_t width, uint32_t height,
                                uint32_t* out_stride_bytes, int* out_fd);

/// Export an EXISTING AHardwareBuffer's dma-buf fd (the Camera2 road:
/// the ImageReader's buffer, borrowed from JNI). The buffer is NOT
/// released (the caller's JNI reference owns it); the fd is the caller's.
/// Returns 0 / -1 with errno. `ahb` is the AHardwareBuffer* opaque handle.
int weft_hw_ahb_export_fd(void* ahb, int* out_fd);

#ifdef __cplusplus
}
#endif

#endif // WEFT_HW_AHB_H
