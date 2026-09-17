// vk_min.h — INTERNAL minimal Vulkan ABI definitions (no SDK dependency).
//
// NOT part of the driver-layer API surface: gpu_ring.c and gpu_probe.c
// share these hand-written structure layouts and constants. The numbers
// ARE the ABI (Vulkan 1.0, stable); only the subset this tree uses is
// declared. Keeping them here (rather than duplicating in the probe)
// follows the repo's one-definition taste; keeping them OUT of the public
// headers keeps gpu_ring.h free of Vulkan surface.
//
// HANDLE DISCIPLINE: on every 64-bit target, dispatchable handles
// (instance/device/queue/command-buffer) are pointers and non-dispatchable
// handles (buffer/memory/pipeline/...) are 64-bit integers — the same
// width as void*. This header declares all handles as void* and passes
// their addresses where the ABI expects handle pointers; the width match
// is what makes that sound (checked by static asserts at compile time).

#ifndef WEFT_VK_MIN_H
#define WEFT_VK_MIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- constants (Vulkan 1.0 spec, verbatim values) -----------------------------
#define VK_STRUCTURE_TYPE_APPLICATION_INFO            0u
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO        1u
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO    2u
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO          3u
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO        5u
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO          12u
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO   15u
#define VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO 16u
#define VK_STRUCTURE_TYPE_SHADER_STAGE_CREATE_INFO    17u
#define VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO 28u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO 14u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO 22u
#define VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET        30u
#define VK_STRUCTURE_TYPE_SUBMIT_INFO                 23u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO   42u

#define VK_API_VERSION_1_0                            0x00400000u
#define VK_QUEUE_COMPUTE_BIT                          0x00000002u
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT              0x00000001u
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT              0x00000002u
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT            0x00000008u
#define VK_SHARING_MODE_EXCLUSIVE                     0u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT           0x00000002u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT          0x00000004u
#define VK_WHOLE_SIZE                                 (~0ULL)
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE              256u
#define VK_SUCCESS                                    0

#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER             3u
#define VK_SHADER_STAGE_COMPUTE_BIT                   0x00000020u
#define VK_PIPELINE_BIND_POINT_COMPUTE                1u
#define VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT 0x1u
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT   0x1u

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

typedef struct {
    uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID;
    uint32_t deviceID; uint32_t deviceType;
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t pipelineCacheUUID[16];
} VkPhysicalDeviceProperties_;

typedef struct {
    uint64_t size; uint64_t alignment; uint32_t memoryTypeBits;
} VkMemoryRequirements_;

// --- pipeline / descriptor / submit side (probe) --------------------------------

typedef struct {
    uint32_t sType; const void* pNext; uint64_t codeSize; const uint32_t* pCode;
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
    uint32_t sType; const void* pNext; uint32_t stage; void* module;
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
    uint32_t sType; const void* pNext; void* dstSet;
    uint32_t dstBinding; uint32_t dstArrayElement;
    uint32_t descriptorCount; uint32_t descriptorType;
    const void* pImageInfo; const VkDescriptorBufferInfo_* pBufferInfo;
    const void* pTexelBufferView;
} VkWriteDescriptorSet_;

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

#ifdef __cplusplus
}
#endif

#endif // WEFT_VK_MIN_H
