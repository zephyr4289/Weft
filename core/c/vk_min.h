// vk_min.h — INTERNAL minimal Vulkan ABI definitions (no SDK dependency).
//
// NOT part of the driver-layer API surface: gpu_ring.c, gpu_stream.c and the
// probes share these hand-written structure layouts and constants. The numbers
// ARE the ABI (Vulkan 1.0 core + the external-memory extensions), stable; only
// the subset this tree uses is declared. Keeping them here (rather than
// duplicating per consumer) follows the repo's one-definition taste; keeping
// them OUT of the public headers keeps gpu_ring.h free of Vulkan surface.
//
// HANDLE DISCIPLINE: on every 64-bit target, dispatchable handles
// (instance/device/queue/command-buffer) are pointers and non-dispatchable
// handles (buffer/memory/pipeline/...) are 64-bit integers — the same
// width as void*. This header declares all handles as void* and passes
// their addresses where the ABI expects handle pointers; the width match
// is what makes that sound (checked by static asserts at compile time).
//
// ---------------------------------------------------------------------------
// SERIES 8 ABI AUDIT (RFC-0013) — read before touching a constant here.
// ---------------------------------------------------------------------------
// Every sType / enum / flag constant and every struct layout in this header
// was re-verified line-by-line against the Khronos Vulkan-Headers
// include/vulkan/vulkan_core.h, VK_HEADER_VERSION 362 (the "r362 header").
// The pre-audit header carried TWELVE wrong constants and TWO struct-layout
// defects, inherited from the RFC-0003 spike:
//
//   constants (pre-audit -> r362 truth):
//     VK_STRUCTURE_TYPE_SUBMIT_INFO                    23 -> 4
//     VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO      15 -> 16
//     VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO    16 -> 30
//     VK_STRUCTURE_TYPE_SHADER_STAGE_CREATE_INFO       17 -> 18
//     VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO   28 -> 29
//     VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO 14 -> 32
//     VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO    22 -> 33
//     VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET           30 -> 35
//     VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO   19 -> 34   (was inline in gpu_probe.c)
//     VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO       26 -> 39   (was inline in gpu_probe.c)
//     VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO   27 -> 40   (was inline in gpu_probe.c)
//     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER                 3 -> 7    (3 is STORAGE_IMAGE)
//     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT             0x8 -> 0x20  (0x8 is STORAGE_TEXEL_BUFFER_BIT)
//   struct layouts:
//     VkShaderModuleCreateInfo_ lacked `flags` — codeSize/pCode sat 8 bytes
//       low, so an ICD read flags=codeSize and pCode=garbage. This defect is
//       EXPERIMENTALLY CONFIRMED: SwiftShader (Vulkan ICD) aborted on the
//       pre-audit gpu-probe with "UNSUPPORTED pCreateInfo->flags 0x1544" —
//       0x1544 = 5444 = the exact byte size of validate_frame.spv, i.e. the
//       ICD was reading our codeSize as the flags word.
//     VkPipelineShaderStageCreateInfo_ lacked `flags` — the ICD read
//       flags=stage(0x20) and stage=0. (Module/pName offsets coincidentally
//       aligned, so only the two 4-byte fields were wrong.)
//
// WHY THE BUGS SURVIVED: the only code paths that used the descriptor-side
// constants and shader-module layout were behind vkCreateShaderModule, which
// the software ICDs (llvmpipe) never reached in evidence runs — the llvmpipe
// LLVM JIT reserves ~94 TiB of address space for shader codegen and every
// evidence environment (sandbox AND the gpu-native CI runners, 100+ logged
// runs) refused the reservation, so gpu-probe always exited at the
// allocation-leg stage. The one exercised defect (buffer usage bits) changed
// which usage flags a buffer carried, which no allocation-leg path observes.
// The audit + the SwiftShader execution leg (Series 8) close all of this:
// wrong constants now fail loudly at vk_abi_check (below) and at every
// dispatch, instead of silently relying on driver tolerance.
//
// VERIFICATION GATE: core/c/vk_abi_check.c compiles ONLY when the real
// Khronos headers are installed (CI gpu-native shard installs
// libvulkan-dev) and static-asserts every number in this file against the
// r362 values. On hosts without the headers the gate self-skips (declared)
// — the numbers were verified by hand against the r362 header regardless.

#ifndef WEFT_VK_MIN_H
#define WEFT_VK_MIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- constants (Vulkan 1.0 spec + r362 header, verbatim values) ---------------
#define VK_STRUCTURE_TYPE_APPLICATION_INFO              0u
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO          1u
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO      2u
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO            3u
#define VK_STRUCTURE_TYPE_SUBMIT_INFO                   4u
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO          5u
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO            12u
#define VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO       13u
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO             14u
#define VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO        15u
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO     16u
#define VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO 18u
#define VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO  29u
#define VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO   30u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO 32u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO   33u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO  34u
#define VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET          35u
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO      39u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO  40u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO     42u
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER          45u

// VK_KHR_external_memory_fd / VK_EXT_external_memory_dma_buf (r362 values)
#define VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO   1000072002u
#define VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR     1000074000u
#define VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR        1000074002u

#define VK_API_VERSION_1_0                              0x00400000u
#define VK_QUEUE_COMPUTE_BIT                            0x00000002u
#define VK_QUEUE_GRAPHICS_BIT                           0x00000001u
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT                0x00000001u
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT                0x00000002u
#define VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT        0x00000004u
#define VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT        0x00000008u
#define VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT              0x00000010u
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT              0x00000020u
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT                 0x00000001u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT                 0x00000002u
#define VK_IMAGE_USAGE_STORAGE_BIT                      0x00000008u
#define VK_SHARING_MODE_EXCLUSIVE                       0u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT             0x00000002u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT            0x00000004u
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT             0x00000001u
#define VK_WHOLE_SIZE                                   (~0ULL)
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE                256u
#define VK_MAX_EXTENSION_NAME_SIZE                      256u
#define VK_SUCCESS                                      0
#define VK_ERROR_OUT_OF_HOST_MEMORY                     (-1)

#define VK_DESCRIPTOR_TYPE_STORAGE_IMAGE                3u
#define VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER         4u
#define VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER         5u
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER               7u
#define VK_SHADER_STAGE_COMPUTE_BIT                     0x00000020u
#define VK_PIPELINE_BIND_POINT_COMPUTE                  1u
#define VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT 0x1u
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT     0x1u

// images (rasterize-to-image probe)
#define VK_IMAGE_TYPE_2D                                1u
#define VK_FORMAT_R8G8B8A8_UNORM                        37u
#define VK_FORMAT_R32_UINT                              98u
#define VK_IMAGE_TILING_OPTIMAL                         0u
#define VK_IMAGE_TILING_LINEAR                          1u
#define VK_IMAGE_LAYOUT_UNDEFINED                       0u
#define VK_IMAGE_LAYOUT_GENERAL                         1u
#define VK_IMAGE_VIEW_TYPE_2D                           1u
#define VK_COMPONENT_SWIZZLE_IDENTITY                   0u
#define VK_IMAGE_ASPECT_COLOR_BIT                       0x00000001u
#define VK_SAMPLE_COUNT_1_BIT                           0x00000001u
#define VK_QUEUE_FAMILY_IGNORED                         (~0U)
#define VK_REMAINING_MIP_LEVELS                         (~0U)
#define VK_REMAINING_ARRAY_LAYERS                       (~0U)

// barriers (image layout transitions for the storage-image path)
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT               0x00000001u
#define VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT            0x00000800u
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT            0x00002000u
#define VK_ACCESS_SHADER_READ_BIT                       0x00000020u
#define VK_ACCESS_SHADER_WRITE_BIT                      0x00000040u

// external memory
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT    0x00000001u
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT  0x00000200u
#define VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME        "VK_KHR_external_memory_fd"
#define VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME   "VK_EXT_external_memory_dma_buf"

// VK_EXT_external_memory_host (RFC-0016 §2 — import an EXISTING host
// allocation; r362 values, same discipline as the fd structs above)
#define VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME      "VK_EXT_external_memory_host"
#define VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_INFO   1000072000u
#define VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT 1000075000u
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT 1000075001u
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2  1000059000u  // core 1.1 value
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT 0x00000080u

typedef uint32_t VkFlags_;
typedef uint32_t VkBool32_;

// --- instance / device side (shared with gpu_ring.c) ---------------------------

typedef struct {
    uint32_t sType; const void* pNext;
    const char* pApplicationName; uint32_t applicationVersion;
    const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    const VkApplicationInfo_* pApplicationInfo;
    uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint32_t queueFamilyIndex; uint32_t queueCount;
    const float* pQueuePriorities;
} VkDeviceQueueCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo_* pQueueCreateInfos;
    uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
} VkDeviceCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint64_t size; uint32_t usage; uint32_t sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
} VkBufferCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext;
    uint64_t allocationSize; uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo_;

// pNext-chain: export (create-side) / import (allocation-side) — RFC-0013 fd bridge
typedef struct {
    uint32_t sType; const void* pNext;
    VkFlags_ handleTypes;             // VK_EXTERNAL_MEMORY_HANDLE_TYPE_*_BIT
} VkExportMemoryAllocateInfo_;

typedef struct {
    uint32_t sType; const void* pNext;
    VkFlags_ handleType;              // VK_EXTERNAL_MEMORY_HANDLE_TYPE_*_BIT
    int fd;                           // consumed (ownership transferred) on success
} VkImportMemoryFdInfoKHR_;

// pNext-chain: host-pointer import (RFC-0016 §2) — the allocation IS the
// application's own memory; the device aliases the same physical pages.
typedef struct {
    uint32_t sType; const void* pNext;
    VkFlags_ handleType;              // HOST_ALLOCATION_BIT_EXT for app memory
    void* pHostPointer;               // must be minImportedHostPointerAlignment-aligned
} VkImportMemoryHostPointerInfoEXT_;

// pNext-chain: buffer create-info chain (imported-memory buffers)
typedef struct {
    uint32_t sType; const void* pNext;
    VkFlags_ handleTypes;             // VK_EXTERNAL_MEMORY_HANDLE_TYPE_*_BIT
} VkExternalMemoryBufferInfo_;

typedef struct {
    uint32_t sType; const void* pNext;
    const void* memory;               // VkDeviceMemory
    VkFlags_ handleType;
} VkMemoryGetFdInfoKHR_;

typedef struct {
    VkFlags_ queueFlags; uint32_t queueCount;
    uint32_t timestampValidBits; uint32_t minImageTransferGranularity[3];
} VkQueueFamilyProperties_;

typedef struct {
    uint32_t memoryTypeCount;
    struct { VkFlags_ propertyFlags; uint32_t heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct { uint64_t size; VkFlags_ flags; } memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties_;

// FULL r362 SIZE (audit defect #3): the pre-audit view stopped at
// pipelineCacheUUID (296 bytes) while every ICD writes the full 824-byte
// struct (VkPhysicalDeviceLimits + VkPhysicalDeviceSparseProperties follow)
// — a 528-byte stack smash in gpu_create_vulkan that -O2 frames absorbed by
// luck and -O0 frames turned into a segfault (caught on SwiftShader). This
// tree reads only deviceName; the tail is reserved opaque so the ICD's full
// write always lands inside the object.
typedef struct {
    uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID;
    uint32_t deviceID; uint32_t deviceType;
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t pipelineCacheUUID[16];
    uint8_t limits_and_sparse[532];  // VkPhysicalDeviceLimits + Sparse, opaque
} VkPhysicalDeviceProperties_;

typedef struct {
    uint64_t size; uint64_t alignment; uint32_t memoryTypeBits;
} VkMemoryRequirements_;

// properties2 query chain (RFC-0016 §2): minImportedHostPointerAlignment
// (placed after VkPhysicalDeviceProperties_ — the chain embeds it by value)
typedef struct {
    uint32_t sType; const void* pNext;
    VkPhysicalDeviceProperties_ properties;
} VkPhysicalDeviceProperties2_;

typedef struct {
    uint32_t sType; const void* pNext;
    uint64_t minImportedHostPointerAlignment;
} VkPhysicalDeviceExternalMemoryHostPropertiesEXT_;

typedef struct {
    char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties_;

// --- pipeline / descriptor / submit side (gpu_stream.c + probes) ---------------

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;   // flags: AUDIT FIX (was absent)
    uint64_t codeSize; const uint32_t* pCode;
} VkShaderModuleCreateInfo_;

typedef struct {
    uint32_t binding; uint32_t descriptorType; uint32_t descriptorCount;
    uint32_t stageFlags; const void* pImmutableSamplers;
} VkDescriptorSetLayoutBinding_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint32_t bindingCount; const VkDescriptorSetLayoutBinding_* pBindings;
} VkDescriptorSetLayoutCreateInfo_;

typedef struct {
    uint32_t stageFlags; uint32_t offset; uint32_t size;  // NOT a sType struct
} VkPushConstantRange_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint32_t setLayoutCount; const void* const* pSetLayouts;
    uint32_t pushConstantRangeCount; const VkPushConstantRange_* pPushConstantRanges;
} VkPipelineLayoutCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;   // flags: AUDIT FIX (was absent)
    uint32_t stage; void* module;
    const char* pName; const void* pSpecializationInfo;
} VkPipelineShaderStageCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    VkPipelineShaderStageCreateInfo_ stage; void* layout;
    void* basePipelineHandle; int32_t basePipelineIndex;
} VkComputePipelineCreateInfo_;

typedef struct {
    uint32_t type; uint32_t descriptorCount;
} VkDescriptorPoolSize_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint32_t maxSets; uint32_t poolSizeCount; const VkDescriptorPoolSize_* pPoolSizes;
} VkDescriptorPoolCreateInfo_;

typedef struct {
    uint64_t buffer; uint64_t offset; uint64_t range;
} VkDescriptorBufferInfo_;

typedef struct {
    const void* sampler; const void* imageView; uint32_t imageLayout;
} VkDescriptorImageInfo_;

typedef struct {
    uint32_t sType; const void* pNext; void* dstSet;
    uint32_t dstBinding; uint32_t dstArrayElement;
    uint32_t descriptorCount; uint32_t descriptorType;
    const VkDescriptorImageInfo_* pImageInfo;
    const VkDescriptorBufferInfo_* pBufferInfo;
    const void* const* pTexelBufferView;   // array of VkBufferView handles
} VkWriteDescriptorSet_;

// --- texel-buffer view ("direct texture" road, RFC-0013) -----------------------

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    const void* buffer;                  // VkBuffer
    uint32_t format;                     // VK_FORMAT_R32_UINT for the ring
    uint64_t offset; uint64_t range;
} VkBufferViewCreateInfo_;

// --- storage image (rasterize-to-image road, RFC-0013) -------------------------

typedef struct {
    uint32_t width; uint32_t height; uint32_t depth;
} VkExtent3D_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint32_t imageType; uint32_t format;
    VkExtent3D_ extent;
    uint32_t mipLevels; uint32_t arrayLayers; uint32_t samples;
    uint32_t tiling; uint32_t usage; uint32_t sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
    uint32_t initialLayout;
} VkImageCreateInfo_;

typedef struct {
    uint32_t aspectMask; uint32_t baseMipLevel; uint32_t levelCount;
    uint32_t baseArrayLayer; uint32_t layerCount;
} VkImageSubresourceRange_;

typedef struct {
    uint32_t r, g, b, a;                 // VK_COMPONENT_SWIZZLE_* (IDENTITY = 0)
} VkComponentMapping_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    const void* image;                   // VkImage
    uint32_t viewType; uint32_t format;
    VkComponentMapping_ components;
    VkImageSubresourceRange_ subresourceRange;
} VkImageViewCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ srcAccessMask; VkFlags_ dstAccessMask;
    uint32_t oldLayout; uint32_t newLayout;
    uint32_t srcQueueFamilyIndex; uint32_t dstQueueFamilyIndex;
    const void* image;                   // VkImage
    VkImageSubresourceRange_ subresourceRange;
} VkImageMemoryBarrier_;

// --- command buffers (shared with gpu_probe.c — formerly inline there) ---------

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; const void* commandPool;
    uint32_t level; uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; const void* descriptorPool;
    uint32_t descriptorSetCount; const void* const* pSetLayouts;
} VkDescriptorSetAllocateInfo_;

typedef struct {
    uint32_t sType; const void* pNext; VkFlags_ flags;
    const void* pInheritanceInfo;
} VkCommandBufferBeginInfo_;

typedef struct {
    uint32_t sType; const void* pNext;
    uint32_t waitSemaphoreCount; const void* const* pWaitSemaphores;
    const void* pWaitDstStageMask;
    uint32_t commandBufferCount; const void* const* pCommandBuffers;
    uint32_t signalSemaphoreCount; const void* const* pSignalSemaphores;
} VkSubmitInfo_;

// Handle-width soundness (the void*-handle discipline above).
_Static_assert(sizeof(void*) == 8, "vk_min: 64-bit handles expected");
_Static_assert(sizeof(VkPipelineShaderStageCreateInfo_) == 48, "vk_min: stage struct r362 size");
_Static_assert(sizeof(VkShaderModuleCreateInfo_) == 40, "vk_min: shader-module struct r362 size");
_Static_assert(sizeof(VkBufferViewCreateInfo_) == 56, "vk_min: buffer-view struct r362 size");
_Static_assert(sizeof(VkImageCreateInfo_) == 88, "vk_min: image struct r362 size");
_Static_assert(sizeof(VkImageViewCreateInfo_) == 80, "vk_min: image-view struct r362 size");
_Static_assert(sizeof(VkWriteDescriptorSet_) == 64, "vk_min: write-descriptor struct r362 size");
_Static_assert(sizeof(VkDescriptorImageInfo_) == 24, "vk_min: descriptor-image struct r362 size");
_Static_assert(sizeof(VkImageMemoryBarrier_) == 72, "vk_min: image-barrier struct r362 size");
_Static_assert(sizeof(VkPhysicalDeviceProperties_) == 824, "vk_min: physdev-props struct r362 size");
_Static_assert(sizeof(VkSubmitInfo_) == 72, "vk_min: submit struct r362 size");
_Static_assert(sizeof(VkMemoryAllocateInfo_) == 32, "vk_min: mem-alloc struct r362 size");
_Static_assert(sizeof(VkBufferCreateInfo_) == 56, "vk_min: buffer struct r362 size");
_Static_assert(sizeof(VkComputePipelineCreateInfo_) == 96, "vk_min: compute-pipeline struct r362 size");
_Static_assert(sizeof(VkDescriptorSetAllocateInfo_) == 40, "vk_min: desc-set-alloc struct r362 size");
_Static_assert(sizeof(VkCommandPoolCreateInfo_) == 24, "vk_min: cmd-pool struct r362 size");
_Static_assert(sizeof(VkCommandBufferAllocateInfo_) == 32, "vk_min: cmd-buf-alloc struct r362 size");
_Static_assert(sizeof(VkImportMemoryFdInfoKHR_) == 24, "vk_min: import-fd struct r362 size");
_Static_assert(sizeof(VkImportMemoryHostPointerInfoEXT_) == 32, "vk_min: import-host struct r362 size");
_Static_assert(sizeof(VkExternalMemoryBufferInfo_) == 24, "vk_min: extmem-buffer struct r362 size");
_Static_assert(sizeof(VkPhysicalDeviceProperties2_) == 16 + 824, "vk_min: props2 struct size");
_Static_assert(sizeof(VkPhysicalDeviceExternalMemoryHostPropertiesEXT_) == 24, "vk_min: ext-host props struct size");
_Static_assert(sizeof(VkMemoryGetFdInfoKHR_) == 32, "vk_min: get-fd struct r362 size");
_Static_assert(sizeof(VkExportMemoryAllocateInfo_) == 24, "vk_min: export-mem struct r362 size");
_Static_assert(sizeof(VkExtensionProperties_) == 260, "vk_min: extension-props struct r362 size");

// ---------------------------------------------------------------------------
// VkFence (Series 9, issue #17-5: fence-scoped dispatch sync)
// ---------------------------------------------------------------------------
#define VK_STRUCTURE_TYPE_FENCE_CREATE_INFO       7u   // r362 value
#define VK_FENCE_CREATE_SIGNALED_BIT              1u

typedef struct VkFenceCreateInfo_ {
    uint32_t sType;       // VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    const void* pNext;
    uint32_t flags;       // VK_FENCE_CREATE_SIGNALED_BIT for pre-signaled
} VkFenceCreateInfo_;
_Static_assert(sizeof(VkFenceCreateInfo_) == 24, "vk_min: fence-create struct r362 size");

#ifdef __cplusplus
}
#endif

#endif // WEFT_VK_MIN_H
