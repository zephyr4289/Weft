// gpu_stream.c — zero-copy GPU streaming kit implementation (RFC-0013).
//
// The binding convention, frozen in gpu_stream.h:
//   binding 0: ring SSBO (the session span's own VkBuffer — zero copy)
//   binding 1: result SSBO (32 B, HOST_VISIBLE|COHERENT, persistent map)
//   binding 2: rgba8ui storage image (optional; barriered to GENERAL once)
//   binding 3: R32_UINT texel buffer view over the span (optional)
//
// Every device-level entry point resolves through weft_gpu_vk_proc (the
// ring's loader discipline — no link-time Vulkan dependency). The pipeline
// count bug class the RFC-0013 audit found (createInfoCount=0) is defended
// here structurally: the single create call passes 1 and rejects NULL.
//
// Layer discipline: driver layer; no allocation on the dispatch path.

#include "gpu_stream.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_min.h"

#define VK_FORMAT_R8G8B8A8_UINT_ 41u  // r362 (verified; see vk_min.h audit)

#define STREAM_RESULT_WORDS 8

struct weft_gpu_stream {
    weft_gpu_ring_t* ring;
    void* device;

    // resolved device-level procs (the RESOLVE pattern from gpu_probe.c)
    int (*vkCreateShaderModule)(void*, const VkShaderModuleCreateInfo_*, const void*, void**);
    void (*vkDestroyShaderModule)(void*, void*, const void*);
    int (*vkCreateBuffer)(void*, const VkBufferCreateInfo_*, const void*, void**);
    void (*vkDestroyBuffer)(void*, void*, const void*);
    void (*vkGetBufferMemoryRequirements)(void*, void*, VkMemoryRequirements_*);
    int (*vkAllocateMemory)(void*, const VkMemoryAllocateInfo_*, const void*, void**);
    void (*vkFreeMemory)(void*, void*, const void*);
    int (*vkBindBufferMemory)(void*, void*, void*, uint64_t);
    int (*vkMapMemory)(void*, void*, uint64_t, uint64_t, VkFlags_, void**);
    void (*vkUnmapMemory)(void*, void*);
    int (*vkCreateBufferView)(void*, const VkBufferViewCreateInfo_*, const void*, void**);
    void (*vkDestroyBufferView)(void*, void*, const void*);
    int (*vkCreateImage)(void*, const VkImageCreateInfo_*, const void*, void**);
    void (*vkDestroyImage)(void*, void*, const void*);
    void (*vkGetImageMemoryRequirements)(void*, void*, VkMemoryRequirements_*);
    int (*vkBindImageMemory)(void*, void*, void*, uint64_t);
    int (*vkCreateImageView)(void*, const VkImageViewCreateInfo_*, const void*, void**);
    void (*vkDestroyImageView)(void*, void*, const void*);
    int (*vkCreateDescriptorSetLayout)(void*, const VkDescriptorSetLayoutCreateInfo_*, const void*, void**);
    void (*vkDestroyDescriptorSetLayout)(void*, void*, const void*);
    int (*vkCreatePipelineLayout)(void*, const VkPipelineLayoutCreateInfo_*, const void*, void**);
    void (*vkDestroyPipelineLayout)(void*, void*, const void*);
    int (*vkCreateComputePipelines)(void*, void*, uint32_t, const VkComputePipelineCreateInfo_*, const void*, void**);
    void (*vkDestroyPipeline)(void*, void*, const void*);
    int (*vkCreateDescriptorPool)(void*, const VkDescriptorPoolCreateInfo_*, const void*, void**);
    void (*vkDestroyDescriptorPool)(void*, void*, const void*);
    int (*vkAllocateDescriptorSets)(void*, const VkDescriptorSetAllocateInfo_*, void**);
    void (*vkUpdateDescriptorSets)(void*, uint32_t, const VkWriteDescriptorSet_*, uint32_t, const void*);
    int (*vkCreateCommandPool)(void*, const VkCommandPoolCreateInfo_*, const void*, void**);
    void (*vkDestroyCommandPool)(void*, void*, const void*);
    int (*vkAllocateCommandBuffers)(void*, const VkCommandBufferAllocateInfo_*, void**);
    int (*vkBeginCommandBuffer)(void*, const VkCommandBufferBeginInfo_*);
    void (*vkCmdPipelineBarrier)(void*, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, const void*, uint32_t, const void*);
    void (*vkCmdBindPipeline)(void*, uint32_t, void*);
    void (*vkCmdBindDescriptorSets)(void*, uint32_t, void*, uint32_t, uint32_t, const void* const*, uint32_t, const uint32_t*);
    void (*vkCmdPushConstants)(void*, void*, uint32_t, uint32_t, uint32_t, const void*);
    void (*vkCmdDispatch)(void*, uint32_t, uint32_t, uint32_t);
    int (*vkEndCommandBuffer)(void*);
    void (*vkGetDeviceQueue)(void*, uint32_t, uint32_t, void**);
    int (*vkQueueSubmit)(void*, uint32_t, const VkSubmitInfo_*, void*);
    int (*vkDeviceWaitIdle)(void*);
    void (*vkDestroyDevice)(void*, const void*);
    // issue #17-5: fence-scoped sync (dispatch waits ITS fence, not the
    // whole device; async pipelines 4 deep).
    int (*vkCreateFence)(void*, const VkFenceCreateInfo_*, const void*, void**);
    void (*vkDestroyFence)(void*, void*, const void*);
    int (*vkResetFences)(void*, uint32_t, void* const*);
    int (*vkWaitForFences)(void*, uint32_t, void* const*, uint32_t, uint64_t);
    void* fences[4];      // submit-fence ring (async depth 4)
    unsigned fence_next;

    // kit-owned objects
    void* module;
    void* res_buffer;
    void* res_memory;
    uint32_t* res_mapped;
    void* texel_view;        // binding 3 (optional)
    void* image;             // binding 2 (optional)
    void* image_memory;
    void* image_view;
    void* set_layout;
    void* pipeline_layout;
    void* pipeline;
    void* desc_pool;
    void* set;
    void* cmd_pool;
    void* cmd;
    void* queue;
    int image_barriered;     // first dispatch transitions UNDEFINED -> GENERAL
    unsigned img_w, img_h;
};

#define KRESOLVE(field, name) \
    s->field = (__typeof__(s->field))weft_gpu_vk_proc(g, name); \
    if (s->field == NULL) return WEFT_GPU_STREAM_ERR_NO_VULKAN;

/// HOST_VISIBLE|COHERENT memory type usable by `req` (ring's discipline).
static int32_t stream_find_host_memory(const VkPhysicalDeviceMemoryProperties_* mp,
                                       const VkMemoryRequirements_* req,
                                       uint32_t* out_type) {
    for (uint32_t m = 0; m < mp->memoryTypeCount && m < 32; m++) {
        if (!(req->memoryTypeBits & (1u << m))) continue;
        const VkFlags_ want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((mp->memoryTypes[m].propertyFlags & want) == want) {
            *out_type = m;
            return (int32_t)m;
        }
    }
    return -1;
}

weft_gpu_stream_err_t weft_gpu_stream_init(weft_gpu_stream_t** out,
                                           weft_gpu_ring_t* g,
                                           const void* spv, size_t spv_bytes,
                                           unsigned img_w, unsigned img_h,
                                           int want_texel) {
    *out = NULL;
    if (g == NULL || spv == NULL || spv_bytes == 0 ||
        (spv_bytes & 3u) != 0u) {
        return WEFT_GPU_STREAM_ERR_BAD_ARG;
    }
    if (weft_gpu_backend(g) != WEFT_GPU_BACKEND_VULKAN) {
        return WEFT_GPU_STREAM_ERR_NO_VULKAN;
    }

    weft_gpu_stream_t* s = calloc(1, sizeof(weft_gpu_stream_t));
    if (s == NULL) return WEFT_GPU_STREAM_ERR_MEMORY;
    s->ring = g;
    s->device = (void*)weft_gpu_vk_device(g);
    s->img_w = img_w;
    s->img_h = img_h;

    KRESOLVE(vkCreateShaderModule, "vkCreateShaderModule")
    KRESOLVE(vkDestroyShaderModule, "vkDestroyShaderModule")
    KRESOLVE(vkCreateBuffer, "vkCreateBuffer")
    KRESOLVE(vkDestroyBuffer, "vkDestroyBuffer")
    KRESOLVE(vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements")
    KRESOLVE(vkAllocateMemory, "vkAllocateMemory")
    KRESOLVE(vkFreeMemory, "vkFreeMemory")
    KRESOLVE(vkBindBufferMemory, "vkBindBufferMemory")
    KRESOLVE(vkMapMemory, "vkMapMemory")
    KRESOLVE(vkUnmapMemory, "vkUnmapMemory")
    KRESOLVE(vkCreateBufferView, "vkCreateBufferView")
    KRESOLVE(vkDestroyBufferView, "vkDestroyBufferView")
    KRESOLVE(vkCreateImage, "vkCreateImage")
    KRESOLVE(vkDestroyImage, "vkDestroyImage")
    KRESOLVE(vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements")
    KRESOLVE(vkBindImageMemory, "vkBindImageMemory")
    KRESOLVE(vkCreateImageView, "vkCreateImageView")
    KRESOLVE(vkDestroyImageView, "vkDestroyImageView")
    KRESOLVE(vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout")
    KRESOLVE(vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout")
    KRESOLVE(vkCreatePipelineLayout, "vkCreatePipelineLayout")
    KRESOLVE(vkDestroyPipelineLayout, "vkDestroyPipelineLayout")
    KRESOLVE(vkCreateComputePipelines, "vkCreateComputePipelines")
    KRESOLVE(vkDestroyPipeline, "vkDestroyPipeline")
    KRESOLVE(vkCreateDescriptorPool, "vkCreateDescriptorPool")
    KRESOLVE(vkDestroyDescriptorPool, "vkDestroyDescriptorPool")
    KRESOLVE(vkAllocateDescriptorSets, "vkAllocateDescriptorSets")
    KRESOLVE(vkUpdateDescriptorSets, "vkUpdateDescriptorSets")
    KRESOLVE(vkCreateCommandPool, "vkCreateCommandPool")
    KRESOLVE(vkDestroyCommandPool, "vkDestroyCommandPool")
    KRESOLVE(vkAllocateCommandBuffers, "vkAllocateCommandBuffers")
    KRESOLVE(vkBeginCommandBuffer, "vkBeginCommandBuffer")
    KRESOLVE(vkCmdPipelineBarrier, "vkCmdPipelineBarrier")
    KRESOLVE(vkCmdBindPipeline, "vkCmdBindPipeline")
    KRESOLVE(vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets")
    KRESOLVE(vkCmdPushConstants, "vkCmdPushConstants")
    KRESOLVE(vkCmdDispatch, "vkCmdDispatch")
    KRESOLVE(vkEndCommandBuffer, "vkEndCommandBuffer")
    KRESOLVE(vkGetDeviceQueue, "vkGetDeviceQueue")
    KRESOLVE(vkQueueSubmit, "vkQueueSubmit")
    KRESOLVE(vkDeviceWaitIdle, "vkDeviceWaitIdle")
    KRESOLVE(vkCreateFence, "vkCreateFence")
    KRESOLVE(vkDestroyFence, "vkDestroyFence")
    KRESOLVE(vkResetFences, "vkResetFences")
    KRESOLVE(vkWaitForFences, "vkWaitForFences")

    // Memory-type scan: query the ring's OWN physical device (exported by
    // gpu_ring precisely so consumers never re-create instances).
    VkPhysicalDeviceMemoryProperties_ memprops;
    {
        void (*vkGetPhysicalDeviceMemoryProperties)(void*, VkPhysicalDeviceMemoryProperties_*) =
            (void (*)(void*, VkPhysicalDeviceMemoryProperties_*))
                weft_gpu_vk_instance_proc(g, "vkGetPhysicalDeviceMemoryProperties");
        if (vkGetPhysicalDeviceMemoryProperties == NULL ||
            weft_gpu_vk_physical_device(g) == NULL) {
            weft_gpu_stream_destroy(s);
            return WEFT_GPU_STREAM_ERR_NO_VULKAN;
        }
        vkGetPhysicalDeviceMemoryProperties((void*)weft_gpu_vk_physical_device(g), &memprops);
    }

    // --- result buffer: 32 B HOST_VISIBLE|COHERENT -----------------------
    VkBufferCreateInfo_ rbci = {0};
    rbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    rbci.size = STREAM_RESULT_WORDS * 4;
    rbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    rbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (s->vkCreateBuffer(s->device, &rbci, NULL, &s->res_buffer) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_MEMORY;
    }
    VkMemoryRequirements_ req;
    s->vkGetBufferMemoryRequirements(s->device, s->res_buffer, &req);
    uint32_t memtype = 0;
    if (stream_find_host_memory(&memprops, &req, &memtype) < 0) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_MEMORY;
    }
    VkMemoryAllocateInfo_ mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memtype;
    if (s->vkAllocateMemory(s->device, &mai, NULL, &s->res_memory) != VK_SUCCESS ||
        s->vkBindBufferMemory(s->device, s->res_buffer, s->res_memory, 0) != VK_SUCCESS ||
        s->vkMapMemory(s->device, s->res_memory, 0, VK_WHOLE_SIZE, 0,
                       (void**)&s->res_mapped) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_MEMORY;
    }
    memset(s->res_mapped, 0, STREAM_RESULT_WORDS * 4);

    // --- optional image (binding 2) --------------------------------------
    if (img_w > 0 && img_h > 0) {
        if (!s->vkCreateImage || !s->vkCreateImageView) {
            weft_gpu_stream_destroy(s);
            return WEFT_GPU_STREAM_ERR_IMAGE;
        }
        VkImageCreateInfo_ ici = {0};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UINT_;
        ici.extent.width = img_w;
        ici.extent.height = img_h;
        ici.extent.depth = 1;
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (s->vkCreateImage(s->device, &ici, NULL, &s->image) != VK_SUCCESS) {
            // Documented fallback: some ICDs expose storage images on LINEAR
            // tiling only (SwiftShader does) — retry once, never silently.
            ici.tiling = VK_IMAGE_TILING_LINEAR;
            if (s->vkCreateImage(s->device, &ici, NULL, &s->image) != VK_SUCCESS) {
                weft_gpu_stream_destroy(s);
                return WEFT_GPU_STREAM_ERR_IMAGE;
            }
        }
        VkMemoryRequirements_ ireq;
        s->vkGetImageMemoryRequirements(s->device, s->image, &ireq);
        uint32_t imem = 0;
        // Image memory may be DEVICE_LOCAL-only; accept ANY compatible type.
        if (ireq.memoryTypeBits == 0) {
            weft_gpu_stream_destroy(s);
            return WEFT_GPU_STREAM_ERR_IMAGE;
        }
        for (uint32_t m = 0; m < 32; m++) {
            if (ireq.memoryTypeBits & (1u << m)) { imem = m; break; }
        }
        VkMemoryAllocateInfo_ imai = {0};
        imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        imai.allocationSize = ireq.size;
        imai.memoryTypeIndex = imem;
        if (s->vkAllocateMemory(s->device, &imai, NULL, &s->image_memory) != VK_SUCCESS ||
            s->vkBindImageMemory(s->device, s->image, s->image_memory, 0) != VK_SUCCESS) {
            weft_gpu_stream_destroy(s);
            return WEFT_GPU_STREAM_ERR_IMAGE;
        }
        VkImageViewCreateInfo_ ivci = {0};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = s->image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R8G8B8A8_UINT_;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (s->vkCreateImageView(s->device, &ivci, NULL, &s->image_view) != VK_SUCCESS) {
            weft_gpu_stream_destroy(s);
            return WEFT_GPU_STREAM_ERR_IMAGE;
        }
    }

    // --- optional texel view (binding 3) ---------------------------------
    if (want_texel) {
        VkBufferViewCreateInfo_ bvci = {0};
        bvci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        bvci.buffer = weft_gpu_vk_buffer(g);
        bvci.format = VK_FORMAT_R32_UINT;
        bvci.offset = 0;
        bvci.range = VK_WHOLE_SIZE;
        if (s->vkCreateBufferView(s->device, &bvci, NULL, &s->texel_view) != VK_SUCCESS) {
            weft_gpu_stream_destroy(s);
            return WEFT_GPU_STREAM_ERR_TEXEL;
        }
    }

    // --- descriptor set layout (2..4 bindings) ---------------------------
    VkDescriptorSetLayoutBinding_ binds[4] = {0};
    uint32_t nbinds = 2;
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (s->image != NULL) {
        binds[2].binding = 2;
        binds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binds[2].descriptorCount = 1;
        binds[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        nbinds = 3;
    }
    if (s->texel_view != NULL) {
        binds[nbinds].binding = 3;
        binds[nbinds].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        binds[nbinds].descriptorCount = 1;
        binds[nbinds].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        nbinds++;
    }
    VkDescriptorSetLayoutCreateInfo_ dslci = {0};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = nbinds;
    dslci.pBindings = binds;
    if (s->vkCreateDescriptorSetLayout(s->device, &dslci, NULL, &s->set_layout) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_DESCRIPTOR;
    }

    VkPushConstantRange_ pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 16;  // kit-wide max; shaders may consume less
    const void* layouts[1] = { s->set_layout };
    VkPipelineLayoutCreateInfo_ plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (s->vkCreatePipelineLayout(s->device, &plci, NULL, &s->pipeline_layout) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_PIPELINE;
    }

    VkShaderModuleCreateInfo_ smci = {0};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv_bytes;
    smci.pCode = (const uint32_t*)spv;
    if (s->vkCreateShaderModule(s->device, &smci, NULL, &s->module) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_SHADER;
    }

    VkComputePipelineCreateInfo_ cpci = {0};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = s->module;
    cpci.stage.pName = "main";
    cpci.layout = s->pipeline_layout;
    // count=1 + NULL-handle rejection (the audit's defect class #4).
    if (s->vkCreateComputePipelines(s->device, NULL, 1, &cpci, NULL, &s->pipeline) != VK_SUCCESS ||
        s->pipeline == NULL) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_PIPELINE;
    }

    // --- pool + set + writes ---------------------------------------------
    VkDescriptorPoolSize_ pool_sizes[3] = {0};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 2;
    uint32_t npools = 1;
    if (s->image != NULL) {
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pool_sizes[1].descriptorCount = 1;
        npools = 2;
    }
    if (s->texel_view != NULL) {
        pool_sizes[npools].type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        pool_sizes[npools].descriptorCount = 1;
        npools++;
    }
    VkDescriptorPoolCreateInfo_ dpci = {0};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = npools;
    dpci.pPoolSizes = pool_sizes;
    if (s->vkCreateDescriptorPool(s->device, &dpci, NULL, &s->desc_pool) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_DESCRIPTOR;
    }
    const void* alloc_layouts[1] = { s->set_layout };
    VkDescriptorSetAllocateInfo_ dsai = {0};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s->desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = alloc_layouts;
    if (s->vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_DESCRIPTOR;
    }

    VkDescriptorBufferInfo_ ring_info = {0};
    ring_info.buffer = (uint64_t)(uintptr_t)weft_gpu_vk_buffer(g);
    ring_info.range = weft_gpu_vk_buffer_bytes(g);
    VkDescriptorBufferInfo_ res_info = {0};
    res_info.buffer = (uint64_t)(uintptr_t)s->res_buffer;
    res_info.range = STREAM_RESULT_WORDS * 4;
    VkDescriptorImageInfo_ img_info = {0};
    img_info.imageView = s->image_view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const void* texel_handles[1] = { NULL };

    VkWriteDescriptorSet_ writes[4];
    memset(writes, 0, sizeof(writes));
    uint32_t nwrites = 0;
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = s->set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &ring_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = s->set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &res_info;
    nwrites = 2;
    if (s->image != NULL) {
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = s->set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo = &img_info;
        nwrites = 3;
    }
    if (s->texel_view != NULL) {
        texel_handles[0] = s->texel_view;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = s->set;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        writes[3].pTexelBufferView = texel_handles;
        nwrites = 4;
    }
    s->vkUpdateDescriptorSets(s->device, nwrites, writes, 0, NULL);

    // --- command pool/buffer + queue --------------------------------------
    VkCommandPoolCreateInfo_ cpci2 = {0};
    cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci2.queueFamilyIndex = weft_gpu_vk_queue_family(g);
    if (s->vkCreateCommandPool(s->device, &cpci2, NULL, &s->cmd_pool) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_COMMAND;
    }
    VkCommandBufferAllocateInfo_ cbai = {0};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = s->cmd_pool;
    cbai.level = 0;  // PRIMARY
    cbai.commandBufferCount = 1;
    if (s->vkAllocateCommandBuffers(s->device, &cbai, &s->cmd) != VK_SUCCESS) {
        weft_gpu_stream_destroy(s);
        return WEFT_GPU_STREAM_ERR_COMMAND;
    }
    s->vkGetDeviceQueue(s->device, weft_gpu_vk_queue_family(g), 0, &s->queue);

    // issue #17-5: the fence ring, created PRE-SIGNALED so the first
    // flush/wait passes without a prior submit.
    for (unsigned i = 0; i < 4; i++) {
        VkFenceCreateInfo_ fc = {0};
        fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (s->vkCreateFence(s->device, &fc, NULL, &s->fences[i]) != VK_SUCCESS) {
            s->fences[i] = NULL;
        }
    }
    s->fence_next = 0;

    *out = s;
    return WEFT_GPU_STREAM_OK;
}

weft_gpu_stream_err_t weft_gpu_stream_dispatch(weft_gpu_stream_t* s,
                                               const void* push,
                                               unsigned push_bytes,
                                               unsigned gx, unsigned gy,
                                               unsigned gz) {
    // issue #17-5: dispatch is now async-submit + flush — IDENTICAL return
    // semantics (work complete when this returns), but the wait is
    // fence-scoped instead of a whole-device DeviceWaitIdle stall. The
    // recording lives once, in dispatch_async.
    const weft_gpu_stream_err_t a =
        weft_gpu_stream_dispatch_async(s, push, push_bytes, gx, gy, gz);
    if (a != WEFT_GPU_STREAM_OK) return a;
    return weft_gpu_stream_flush(s);

}

weft_gpu_stream_err_t weft_gpu_stream_dispatch_async(weft_gpu_stream_t* s,
                                                     const void* push,
                                                     unsigned push_bytes,
                                                     unsigned gx, unsigned gy,
                                                     unsigned gz) {
    if (s == NULL || push_bytes > 16u) return WEFT_GPU_STREAM_ERR_BAD_ARG;
    if (s->cmd == NULL || s->queue == NULL) return WEFT_GPU_STREAM_ERR_COMMAND;

    static int atrace = -1;
    if (atrace < 0) atrace = (getenv("WEFT_GPU_STREAM_TRACE") != NULL);
#define ATRACE(msg) do { if (atrace) fprintf(stderr, "stream-async: %s\n", msg); } while (0)

    // Reuse is single-command-buffer: the PREVIOUS submit must be complete
    // before this begin() re-records it. Wait on the ring slot's fence
    // (fence-scoped — NOT a whole-device stall; unrelated queues keep
    // running), then reset it for this submission.
    const unsigned fi = s->fence_next;
    if (s->fences[fi] != NULL) {
        if (s->vkWaitForFences(s->device, 1, &s->fences[fi], 1u, UINT64_MAX)
                != VK_SUCCESS) {
            return WEFT_GPU_STREAM_ERR_SUBMIT;
        }
        if (s->vkResetFences(s->device, 1, &s->fences[fi]) != VK_SUCCESS) {
            return WEFT_GPU_STREAM_ERR_SUBMIT;
        }
    }

    ATRACE("begin");
    VkCommandBufferBeginInfo_ bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (s->vkBeginCommandBuffer(s->cmd, &bi) != VK_SUCCESS) {
        return WEFT_GPU_STREAM_ERR_SUBMIT;
    }
    if (s->image != NULL && !s->image_barriered) {
        VkImageMemoryBarrier_ bar = {0};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = s->image;
        bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bar.subresourceRange.levelCount = 1;
        bar.subresourceRange.layerCount = 1;
        s->vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                0, NULL, 0, NULL, 1, &bar);
        s->image_barriered = 1;
    }
    ATRACE("bind");
    s->vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipeline);
    s->vkCmdBindDescriptorSets(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               s->pipeline_layout, 0, 1,
                               (const void* const*)&s->set, 0, NULL);
    if (push_bytes > 0 && push != NULL) {
        s->vkCmdPushConstants(s->cmd, s->pipeline_layout,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
    }
    ATRACE("dispatch-cmd");
    s->vkCmdDispatch(s->cmd, gx, gy, gz);
    if (s->vkEndCommandBuffer(s->cmd) != VK_SUCCESS) {
        return WEFT_GPU_STREAM_ERR_SUBMIT;
    }

    VkSubmitInfo_ si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = (const void* const*)(const void*)&s->cmd;
    ATRACE("submit-fenced");
    if (s->vkQueueSubmit(s->queue, 1, &si, s->fences[fi]) != VK_SUCCESS) {
        return WEFT_GPU_STREAM_ERR_SUBMIT;
    }
    s->fence_next = (fi + 1) % 4;   // NOTE: single cmd buffer serializes the
                                    // GPU work; the ring keeps fence waits
                                    // short and the device UNSTALLED between
                                    // dispatches (the measured win).
    return WEFT_GPU_STREAM_OK;
}

weft_gpu_stream_err_t weft_gpu_stream_flush(weft_gpu_stream_t* s) {
    if (s == NULL) return WEFT_GPU_STREAM_ERR_BAD_ARG;
    if (s->device == NULL) return WEFT_GPU_STREAM_ERR_COMMAND;
    for (unsigned i = 0; i < 4; i++) {
        if (s->fences[i] == NULL) continue;
        if (s->vkWaitForFences(s->device, 1, &s->fences[i], 1u, UINT64_MAX)
                != VK_SUCCESS) {
            return WEFT_GPU_STREAM_ERR_SUBMIT;
        }
    }
    return WEFT_GPU_STREAM_OK;
}

const uint32_t* weft_gpu_stream_result(const weft_gpu_stream_t* s) {
    return s ? s->res_mapped : NULL;
}

void weft_gpu_stream_destroy(weft_gpu_stream_t* s) {
    if (s == NULL) return;
    if (s->device != NULL) {
        // Wait before teardown: pending work must not reference freed objects.
        if (s->vkDeviceWaitIdle) s->vkDeviceWaitIdle(s->device);
        if (s->cmd_pool && s->vkDestroyCommandPool) s->vkDestroyCommandPool(s->device, s->cmd_pool, NULL);
        if (s->desc_pool && s->vkDestroyDescriptorPool) s->vkDestroyDescriptorPool(s->device, s->desc_pool, NULL);
        if (s->pipeline && s->vkDestroyPipeline) s->vkDestroyPipeline(s->device, s->pipeline, NULL);
        if (s->module && s->vkDestroyShaderModule) s->vkDestroyShaderModule(s->device, s->module, NULL);
        if (s->pipeline_layout && s->vkDestroyPipelineLayout) s->vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
        if (s->set_layout && s->vkDestroyDescriptorSetLayout) s->vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
        if (s->image_view && s->vkDestroyImageView) s->vkDestroyImageView(s->device, s->image_view, NULL);
        if (s->image && s->vkDestroyImage) s->vkDestroyImage(s->device, s->image, NULL);
        if (s->image_memory && s->vkFreeMemory) s->vkFreeMemory(s->device, s->image_memory, NULL);
        if (s->texel_view && s->vkDestroyBufferView) s->vkDestroyBufferView(s->device, s->texel_view, NULL);
        for (unsigned i = 0; i < 4; i++) {
            if (s->fences[i] && s->vkDestroyFence) {
                s->vkDestroyFence(s->device, s->fences[i], NULL);
            }
        }
        if (s->res_mapped && s->vkUnmapMemory) s->vkUnmapMemory(s->device, s->res_memory);
        if (s->res_buffer && s->vkDestroyBuffer) s->vkDestroyBuffer(s->device, s->res_buffer, NULL);
        if (s->res_memory && s->vkFreeMemory) s->vkFreeMemory(s->device, s->res_memory, NULL);
    }
    free(s);
}
