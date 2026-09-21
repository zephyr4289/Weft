// weft_dlpack.h — vendored DLPack v1.0 ABI (dmlc/dlpack @ v1.0), subset.
//
// WHY EXISTS: Rule 1 — the frames weft-vision-dma captures must be
// consumable by PyTorch / ONNX Runtime without memcpy, which means
// handing framework code a `DLManagedTensor` whose data pointer IS the
// kernel DMA buffer mapping. The struct layouts, member orders, and
// enum values below match the official DLPack v1.0 specification
// exactly for the surface this pillar implements (CPU tensors — the
// intra-host ingestion story; GPU/Vulkan device import is Engineer 3's
// consumer seam). Versioned tensors (DLPack 1.1 `DLManagedTensorVersioned`)
// are a declared omission, inventoried in D-62 §A.3.

#ifndef WEFT_DLPACK_H_
#define WEFT_DLPACK_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kDLCPU = 1,
    kDLCUDA = 2,
    kDLCUDAHost = 3,
    kDLOpenCL = 4,
    kDLVulkan = 7,
    kDLMetal = 8,
    kDLVPI = 9,
    kDLROCM = 10,
    kDLROCMHost = 11,
    kDLExtDev = 12,
    kDLCUDAManaged = 13,
    kDLOpenMAX = 15,
    kDLOneDNN = 16,
    kDLWebGPU = 18,
} DLDeviceType;

typedef struct DLContext {
    DLDeviceType device_type;
    int device_id;
} DLContext;

typedef enum {
    kDLInt = 0,
    kDLUInt = 1,
    kDLFloat = 2,
    kDLComplex = 3,
    kDLBfloat = 4,
    kDLBool = 6,
} DLDataTypeCode;

typedef struct DLDataType {
    uint8_t code;
    uint8_t bits;
    uint16_t lanes;
} DLDataType;

typedef struct DLTensor {
    void *data;
    DLContext ctx;
    int ndim;
    DLDataType dtype;
    int64_t *shape;
    int64_t *strides;
    uint64_t byte_offset;
} DLTensor;

typedef struct DLManagedTensor {
    DLTensor dl_tensor;
    void *manager_ctx;
    void (*deleter)(struct DLManagedTensor *);
} DLManagedTensor;

#ifdef __cplusplus
}
#endif

#endif  // WEFT_DLPACK_H_
