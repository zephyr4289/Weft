// weft_gpu_loader.h — Vulkan 1.3 compute discovery API (Pillar 5, D-52).
//
// The GPU-row drivers (qualcomm Adreno, mediatek Mali, nvidia_pc) consult
// this loader at INIT time (cold path) when deciding whether a Vulkan 1.3
// timeline-semaphore compute engine exists on this host. See .c for the
// honesty boundary (headless CI: loader reports 0; drivers degrade).

#ifndef WEFT_GPU_LOADER_H
#define WEFT_GPU_LOADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFT_GPU_MAX_NAME 128

/// One-time probe (idempotent, cached): dlopen libvulkan, enumerate
/// physical devices, require apiVersion >= 1.3 + a compute queue family +
/// the timelineSemaphore feature. Returns 1 when a qualifying device was
/// found. Cold path ONLY (driver init); zero heap on any hot path.
int weft_gpu_vulkan13_probe(void);

/// Cached answer to the probe (re-probes once if not yet run).
/// out_dev_name (optional): the honest device identity string (stable
/// pointer, valid for the process lifetime).
int weft_gpu_vulkan13_available(const char** out_dev_name);

/// The device name of the qualifying Vulkan 1.3 device ("" if none).
const char* weft_gpu_vulkan13_device_name(void);

/// Test seam: forget the cached probe (re-probe on next query).
void weft_gpu_vulkan13_reset_probe(void);

#ifdef __cplusplus
}
#endif

#endif // WEFT_GPU_LOADER_H
