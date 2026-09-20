// weft_vk_compute.c — RFC-0017 §3 implementation (part 2: the pooled
// compute kit, the alias canary, and the frozen preprocess dispatch).
//
// LAW 1 discipline in dispatch(): the ONLY Vulkan calls on the hot path
// are vkResetCommandBuffer / Begin / record / End / ResetFences /
// QueueSubmit / WaitForFences — zero creates, zero allocations (the
// command buffer, fence, descriptor set, pipeline, and pool are all
// created once at init; binds update the existing descriptor set).

#include "weft_vk_bridge.h"

#include <stdlib.h>
#include <string.h>

#include "vk_min.h"
#include "weft/weft_accel_common.h"

#include "shaders/weft_preprocess_spv.h"  // committed, byte-frozen

// additional entry-point types (the vk_min discipline)
typedef int   (*vk_create_shader_module_fn)(void*,
                                            const VkShaderModuleCreateInfo_*,
                                            const void*, void**);
typedef void  (*vk_destroy_shader_module_fn)(void*, void*, const void*);
typedef int   (*vk_create_descriptor_set_layout_fn)(
    void*, const VkDescriptorSetLayoutCreateInfo_*, const void*, void**);
typedef void  (*vk_destroy_descriptor_set_layout_fn)(void*, void*, const void*);
typedef int   (*vk_create_pipeline_layout_fn)(void*,
                                              const VkPipelineLayoutCreateInfo_*,
                                              const void*, void**);
typedef void  (*vk_destroy_pipeline_layout_fn)(void*, void*, const void*);
typedef int   (*vk_create_compute_pipelines_fn)(
    void*, void*, uint32_t, const VkComputePipelineCreateInfo_*,
    const void*, void**);  // SIX params — no flags arg (flags live
                           // inside each create-info; a 7-arg call shifts
                           // every argument and drivers dereference a
                           // near-NULL infos pointer)
typedef void  (*vk_destroy_pipeline_fn)(void*, void*, const void*);
typedef int   (*vk_create_descriptor_pool_fn)(void*,
                                              const VkDescriptorPoolCreateInfo_*,
                                              const void*, void**);
typedef void  (*vk_destroy_descriptor_pool_fn)(void*, void*, const void*);
typedef int   (*vk_allocate_descriptor_sets_fn)(void*,
                                                const VkDescriptorSetAllocateInfo_*,
                                                void**);
typedef void  (*vk_update_descriptor_sets_fn)(void*, uint32_t,
                                              const VkWriteDescriptorSet_*,
                                              uint32_t, const void*);
typedef int   (*vk_create_command_pool_fn)(void*,
                                           const VkCommandPoolCreateInfo_*,
                                           const void*, void**);
typedef void  (*vk_destroy_command_pool_fn)(void*, void*, const void*);
typedef int   (*vk_allocate_command_buffers_fn)(void*,
                                                const VkCommandBufferAllocateInfo_*,
                                                void**);
typedef int   (*vk_begin_cmd_fn)(void*, const VkCommandBufferBeginInfo_*);
typedef int   (*vk_end_cmd_fn)(void*);
typedef void  (*vk_cmd_bind_pipeline_fn)(void*, uint32_t, void*);
typedef void  (*vk_cmd_bind_descriptor_sets_fn)(void*, uint32_t, void*,
                                                uint32_t, uint32_t,
                                                const void* const*,
                                                uint32_t, const uint32_t*);
typedef void  (*vk_cmd_push_constants_fn)(void*, void*, uint32_t, uint32_t,
                                          uint32_t, const void*);
typedef void  (*vk_cmd_dispatch_fn)(void*, uint32_t, uint32_t, uint32_t);
typedef int   (*vk_queue_submit_fn)(void*, uint32_t, const VkSubmitInfo_*,
                                    void*);
typedef int   (*vk_create_fence_fn)(void*, const VkFenceCreateInfo_*,
                                    const void*, void**);
typedef void  (*vk_destroy_fence_fn)(void*, void*, const void*);
typedef int   (*vk_wait_fences_fn)(void*, uint32_t, const void* const*,
                                   VkBool32_, uint64_t);
typedef int   (*vk_reset_fences_fn)(void*, uint32_t, const void* const*);
typedef int   (*vk_reset_cmd_fn)(void*, VkFlags_);

#define WEFT_VK_MAX_BINDINGS 8u
#define WEFT_VK_MAX_PUSH 128u

struct weft_vk_compute {
    weft_vk_dev_t dev;                       // copied device view
    void*    module;                         // VkShaderModule
    void*    set_layout;                     // VkDescriptorSetLayout
    void*    pipe_layout;                    // VkPipelineLayout
    void*    pipeline;                       // VkComputePipeline
    void*    desc_pool;                      // VkDescriptorPool
    void*    desc_set;                       // VkDescriptorSet
    void*    cmd_pool;                       // VkCommandPool
    void*    cmd;                            // VkCommandBuffer (pooled)
    void*    fence;                          // VkFence (pooled)
    uint32_t n_bindings;
    uint32_t push_bytes;
    // the bind table (consumed by dispatch; updated by weft_vk_compute_bind)
    struct {
        void*    buffer;      // VkBuffer (device-local handle)
        uint64_t offset;
        uint64_t range;
        int      bound;
    } binds[WEFT_VK_MAX_BINDINGS];
};

weft_vk_err_t weft_vk_compute_init(weft_vk_compute_t** out,
                                   const weft_vk_dev_t* dev,
                                   const void* spv, uint32_t spv_words,
                                   uint64_t frozen_id_expect,
                                   uint32_t n_bindings,
                                   uint32_t push_bytes) {
    if (!out || !dev || !dev->device || !dev->queue || !dev->proc ||
        !spv || spv_words == 0 ||
        n_bindings == 0 || n_bindings > WEFT_VK_MAX_BINDINGS ||
        push_bytes == 0 || push_bytes > WEFT_VK_MAX_PUSH) {
        return WEFT_VK_ERR_BAD_ARG;
    }
    *out = NULL;

    // Law 3: the frozen-ID check runs BEFORE the module exists — the
    // wrong/old kernel never reaches the device.
    if (frozen_id_expect != 0) {
        uint64_t id = weft_accel_frozen_id(spv, (size_t)spv_words * 4u);
        if (id != frozen_id_expect) return WEFT_VK_ERR_FROZEN_ID;
    }

    vk_create_shader_module_fn vkCreateShaderModule =
        (vk_create_shader_module_fn)(void*)dev->proc(dev->proc_user,
                                                     "vkCreateShaderModule");
    vk_destroy_shader_module_fn vkDestroyShaderModule =
        (vk_destroy_shader_module_fn)(void*)dev->proc(dev->proc_user,
                                                      "vkDestroyShaderModule");
    vk_create_descriptor_set_layout_fn vkCreateDescriptorSetLayout =
        (vk_create_descriptor_set_layout_fn)(void*)dev->proc(
            dev->proc_user, "vkCreateDescriptorSetLayout");
    vk_destroy_descriptor_set_layout_fn vkDestroyDescriptorSetLayout =
        (vk_destroy_descriptor_set_layout_fn)(void*)dev->proc(
            dev->proc_user, "vkDestroyDescriptorSetLayout");
    vk_create_pipeline_layout_fn vkCreatePipelineLayout =
        (vk_create_pipeline_layout_fn)(void*)dev->proc(dev->proc_user,
                                                       "vkCreatePipelineLayout");
    vk_destroy_pipeline_layout_fn vkDestroyPipelineLayout =
        (vk_destroy_pipeline_layout_fn)(void*)dev->proc(
            dev->proc_user, "vkDestroyPipelineLayout");
    vk_create_compute_pipelines_fn vkCreateComputePipelines =
        (vk_create_compute_pipelines_fn)(void*)dev->proc(
            dev->proc_user, "vkCreateComputePipelines");
    vk_destroy_pipeline_fn vkDestroyPipeline =
        (vk_destroy_pipeline_fn)(void*)dev->proc(dev->proc_user,
                                                 "vkDestroyPipeline");
    vk_create_descriptor_pool_fn vkCreateDescriptorPool =
        (vk_create_descriptor_pool_fn)(void*)dev->proc(dev->proc_user,
                                                       "vkCreateDescriptorPool");
    vk_destroy_descriptor_pool_fn vkDestroyDescriptorPool =
        (vk_destroy_descriptor_pool_fn)(void*)dev->proc(
            dev->proc_user, "vkDestroyDescriptorPool");
    vk_allocate_descriptor_sets_fn vkAllocateDescriptorSets =
        (vk_allocate_descriptor_sets_fn)(void*)dev->proc(
            dev->proc_user, "vkAllocateDescriptorSets");
    vk_update_descriptor_sets_fn vkUpdateDescriptorSets =
        (vk_update_descriptor_sets_fn)(void*)dev->proc(
            dev->proc_user, "vkUpdateDescriptorSets");
    vk_create_command_pool_fn vkCreateCommandPool =
        (vk_create_command_pool_fn)(void*)dev->proc(dev->proc_user,
                                                    "vkCreateCommandPool");
    vk_destroy_command_pool_fn vkDestroyCommandPool =
        (vk_destroy_command_pool_fn)(void*)dev->proc(dev->proc_user,
                                                     "vkDestroyCommandPool");
    vk_allocate_command_buffers_fn vkAllocateCommandBuffers =
        (vk_allocate_command_buffers_fn)(void*)dev->proc(
            dev->proc_user, "vkAllocateCommandBuffers");
    vk_create_fence_fn vkCreateFence =
        (vk_create_fence_fn)(void*)dev->proc(dev->proc_user, "vkCreateFence");
    vk_destroy_fence_fn vkDestroyFence =
        (vk_destroy_fence_fn)(void*)dev->proc(dev->proc_user,
                                              "vkDestroyFence");
    if (!vkCreateShaderModule || !vkDestroyShaderModule ||
        !vkCreateDescriptorSetLayout || !vkDestroyDescriptorSetLayout ||
        !vkCreatePipelineLayout || !vkDestroyPipelineLayout ||
        !vkCreateComputePipelines || !vkDestroyPipeline ||
        !vkCreateDescriptorPool || !vkDestroyDescriptorPool ||
        !vkAllocateDescriptorSets || !vkUpdateDescriptorSets ||
        !vkCreateCommandPool || !vkDestroyCommandPool ||
        !vkAllocateCommandBuffers || !vkCreateFence || !vkDestroyFence) {
        return WEFT_VK_ERR_NO_DEVICE;
    }

    weft_vk_compute_t* k = (weft_vk_compute_t*)calloc(1, sizeof(*k));
    if (!k) return WEFT_VK_ERR_NO_LOADER;  // setup OOM: honest refusal
    k->dev = *dev;
    k->n_bindings = n_bindings;
    k->push_bytes = push_bytes;

    // shader module
    VkShaderModuleCreateInfo_ smci = {0};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (uint64_t)spv_words * 4u;
    smci.pCode = (const uint32_t*)spv;
    if (vkCreateShaderModule(dev->device, &smci, NULL, &k->module) !=
        VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_SHADER;
    }

    // descriptor set layout: n_bindings storage buffers
    VkDescriptorSetLayoutBinding_ lbs[WEFT_VK_MAX_BINDINGS];
    for (uint32_t i = 0; i < n_bindings; i++) {
        lbs[i].binding = i;
        lbs[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lbs[i].descriptorCount = 1;
        lbs[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lbs[i].pImmutableSamplers = NULL;
    }
    VkDescriptorSetLayoutCreateInfo_ dslci = {0};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = n_bindings;
    dslci.pBindings = lbs;
    if (vkCreateDescriptorSetLayout(dev->device, &dslci, NULL,
                                    &k->set_layout) != VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_PIPELINE;
    }

    // pipeline layout + push constants
    VkPushConstantRange_ pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = push_bytes;
    VkPipelineLayoutCreateInfo_ plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const void* layouts[1] = { k->set_layout };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev->device, &plci, NULL, &k->pipe_layout) !=
        VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_PIPELINE;
    }

    // compute pipeline
    VkComputePipelineCreateInfo_ cpci = {0};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = k->module;
    cpci.stage.pName = "main";
    cpci.layout = k->pipe_layout;
    if (vkCreateComputePipelines(dev->device, NULL, 1, &cpci, NULL,
                                 &k->pipeline) != VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_PIPELINE;
    }

    // descriptor pool + one set
    VkDescriptorPoolSize_ pool_size = {0};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = n_bindings;
    VkDescriptorPoolCreateInfo_ dpci = {0};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(dev->device, &dpci, NULL, &k->desc_pool) !=
        VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_PIPELINE;
    }
    VkDescriptorSetAllocateInfo_ dsai = {0};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = k->desc_pool;
    dsai.descriptorSetCount = 1;
    const void* set_layouts[1] = { k->set_layout };
    dsai.pSetLayouts = set_layouts;
    if (vkAllocateDescriptorSets(dev->device, &dsai, &k->desc_set) !=
        VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_PIPELINE;
    }

    // command pool + one primary command buffer (pooled for every
    // dispatch). The pool carries the DEV's queue family — command
    // buffers are submitted to dev->queue and the spec binds pools to
    // their family (cross-family submit is undefined, not portable).
    // RESET_COMMAND_BUFFER_BIT: dispatch() re-records the pooled buffer
    // per submit and resets it explicitly — without this flag the spec
    // makes vkResetCommandBuffer on the pool's buffers UNDEFINED
    // (the substrate's kit instead relies on begin's implicit reset;
    // this kit prefers the explicit reset + the flag).
    VkCommandPoolCreateInfo_ cpci2 = {0};
    cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci2.queueFamilyIndex = dev->queue_family;
    cpci2.flags = 0x2u;  // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    if (vkCreateCommandPool(dev->device, &cpci2, NULL, &k->cmd_pool) !=
        VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_COMMAND;
    }
    VkCommandBufferAllocateInfo_ cbai = {0};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = k->cmd_pool;
    cbai.level = 0;    // VK_COMMAND_BUFFER_LEVEL_PRIMARY
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev->device, &cbai, &k->cmd) != VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_COMMAND;
    }

    // pooled fence (created UNSIGNALED; reset before every submit)
    VkFenceCreateInfo_ fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(dev->device, &fci, NULL, &k->fence) != VK_SUCCESS) {
        weft_vk_compute_destroy(k);
        return WEFT_VK_ERR_COMMAND;
    }

    *out = k;
    return WEFT_VK_OK;
}

weft_vk_err_t weft_vk_compute_bind(weft_vk_compute_t* k, uint32_t binding,
                                   void* vk_buffer, uint64_t offset,
                                   uint64_t range) {
    if (!k || !vk_buffer || binding >= k->n_bindings) {
        return WEFT_VK_ERR_BAD_ARG;
    }
    k->binds[binding].buffer = vk_buffer;
    k->binds[binding].offset = offset;
    k->binds[binding].range = range;
    k->binds[binding].bound = 1;
    return WEFT_VK_OK;
}

weft_vk_err_t weft_vk_compute_dispatch(weft_vk_compute_t* k,
                                       const void* push,
                                       uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!k || !push) return WEFT_VK_ERR_BAD_ARG;
    for (uint32_t i = 0; i < k->n_bindings; i++) {
        if (!k->binds[i].bound) return WEFT_VK_ERR_BAD_ARG;  // unbound slot
    }
    const weft_vk_dev_t* dev = &k->dev;
    vk_update_descriptor_sets_fn vkUpdateDescriptorSets =
        (vk_update_descriptor_sets_fn)(void*)dev->proc(dev->proc_user,
                                                       "vkUpdateDescriptorSets");
    vk_begin_cmd_fn vkBeginCommandBuffer =
        (vk_begin_cmd_fn)(void*)dev->proc(dev->proc_user,
                                          "vkBeginCommandBuffer");
    vk_end_cmd_fn vkEndCommandBuffer =
        (vk_end_cmd_fn)(void*)dev->proc(dev->proc_user, "vkEndCommandBuffer");
    vk_cmd_bind_pipeline_fn vkCmdBindPipeline =
        (vk_cmd_bind_pipeline_fn)(void*)dev->proc(dev->proc_user,
                                                  "vkCmdBindPipeline");
    vk_cmd_bind_descriptor_sets_fn vkCmdBindDescriptorSets =
        (vk_cmd_bind_descriptor_sets_fn)(void*)dev->proc(
            dev->proc_user, "vkCmdBindDescriptorSets");
    vk_cmd_push_constants_fn vkCmdPushConstants =
        (vk_cmd_push_constants_fn)(void*)dev->proc(dev->proc_user,
                                                   "vkCmdPushConstants");
    vk_cmd_dispatch_fn vkCmdDispatch =
        (vk_cmd_dispatch_fn)(void*)dev->proc(dev->proc_user, "vkCmdDispatch");
    vk_queue_submit_fn vkQueueSubmit =
        (vk_queue_submit_fn)(void*)dev->proc(dev->proc_user, "vkQueueSubmit");
    vk_wait_fences_fn vkWaitForFences =
        (vk_wait_fences_fn)(void*)dev->proc(dev->proc_user, "vkWaitForFences");
    vk_reset_fences_fn vkResetFences =
        (vk_reset_fences_fn)(void*)dev->proc(dev->proc_user,
                                             "vkResetFences");
    vk_reset_cmd_fn vkResetCommandBuffer =
        (vk_reset_cmd_fn)(void*)dev->proc(dev->proc_user,
                                          "vkResetCommandBuffer");
    if (!vkUpdateDescriptorSets || !vkBeginCommandBuffer ||
        !vkEndCommandBuffer || !vkCmdBindPipeline ||
        !vkCmdBindDescriptorSets || !vkCmdPushConstants || !vkCmdDispatch ||
        !vkQueueSubmit || !vkWaitForFences || !vkResetFences ||
        !vkResetCommandBuffer) {
        return WEFT_VK_ERR_NO_DEVICE;
    }

    // descriptor writes from the bind table (update, not create — Law 1)
    VkDescriptorBufferInfo_ binfos[WEFT_VK_MAX_BINDINGS];
    VkWriteDescriptorSet_ writes[WEFT_VK_MAX_BINDINGS];
    for (uint32_t i = 0; i < k->n_bindings; i++) {
        binfos[i].buffer = (uint64_t)(uintptr_t)k->binds[i].buffer;
        binfos[i].offset = k->binds[i].offset;
        binfos[i].range = k->binds[i].range;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = k->desc_set;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pImageInfo = NULL;
        writes[i].pBufferInfo = &binfos[i];
        writes[i].pTexelBufferView = NULL;
    }
    vkUpdateDescriptorSets(dev->device, k->n_bindings, writes, 0, NULL);

    // record (pooled command buffer — reset, begin, record, end)
    if (vkResetCommandBuffer(k->cmd, 0) != VK_SUCCESS) {
        return WEFT_VK_ERR_COMMAND;
    }
    VkCommandBufferBeginInfo_ bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(k->cmd, &bi) != VK_SUCCESS) {
        return WEFT_VK_ERR_COMMAND;
    }
    vkCmdBindPipeline(k->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k->pipeline);
    const void* sets[1] = { k->desc_set };
    vkCmdBindDescriptorSets(k->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            k->pipe_layout, 0, 1, sets, 0, NULL);
    vkCmdPushConstants(k->cmd, k->pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, k->push_bytes, push);
    vkCmdDispatch(k->cmd, gx, gy, gz);
    if (vkEndCommandBuffer(k->cmd) != VK_SUCCESS) {
        return WEFT_VK_ERR_COMMAND;
    }

    // submit + wait (pooled fence: reset before submit, wait after)
    if (vkResetFences(dev->device, 1,
                      (const void* const*)&k->fence) != VK_SUCCESS) {
        return WEFT_VK_ERR_COMMAND;
    }
    VkSubmitInfo_ si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    const void* const cmds[1] = { k->cmd };
    si.pCommandBuffers = cmds;
    if (vkQueueSubmit(dev->queue, 1, &si, k->fence) != VK_SUCCESS) {
        return WEFT_VK_ERR_COMMAND;
    }
    if (vkWaitForFences(dev->device, 1, (const void* const*)&k->fence, 1,
                        ~0ull) != VK_SUCCESS) {
        return WEFT_VK_ERR_COMMAND;
    }
    return WEFT_VK_OK;
}

void weft_vk_compute_destroy(weft_vk_compute_t* k) {
    if (!k) return;
    const weft_vk_dev_t* dev = &k->dev;
    if (dev->device && dev->proc) {
        vk_destroy_shader_module_fn dsm =
            (vk_destroy_shader_module_fn)(void*)dev->proc(dev->proc_user,
                                              "vkDestroyShaderModule");
        vk_destroy_descriptor_set_layout_fn ddsl =
            (vk_destroy_descriptor_set_layout_fn)(void*)dev->proc(
                dev->proc_user, "vkDestroyDescriptorSetLayout");
        vk_destroy_pipeline_layout_fn dpl =
            (vk_destroy_pipeline_layout_fn)(void*)dev->proc(
                dev->proc_user, "vkDestroyPipelineLayout");
        vk_destroy_pipeline_fn dp =
            (vk_destroy_pipeline_fn)(void*)dev->proc(dev->proc_user,
                                                     "vkDestroyPipeline");
        vk_destroy_descriptor_pool_fn ddp =
            (vk_destroy_descriptor_pool_fn)(void*)dev->proc(
                dev->proc_user, "vkDestroyDescriptorPool");
        vk_destroy_command_pool_fn dcp =
            (vk_destroy_command_pool_fn)(void*)dev->proc(
                dev->proc_user, "vkDestroyCommandPool");
        vk_destroy_fence_fn df =
            (vk_destroy_fence_fn)(void*)dev->proc(dev->proc_user,
                                                  "vkDestroyFence");
        if (k->fence && df) df(dev->device, k->fence, NULL);
        if (k->cmd_pool && dcp) dcp(dev->device, k->cmd_pool, NULL);
        if (k->desc_pool && ddp) ddp(dev->device, k->desc_pool, NULL);
        if (k->pipeline && dp) dp(dev->device, k->pipeline, NULL);
        if (k->pipe_layout && dpl) dpl(dev->device, k->pipe_layout, NULL);
        if (k->set_layout && ddsl) ddsl(dev->device, k->set_layout, NULL);
        if (k->module && dsm) dsm(dev->device, k->module, NULL);
    }
    free(k);
}

// ---------------------------------------------------------------------------
// The alias canary (Law 4's load-bearing gate)
// ---------------------------------------------------------------------------

weft_vk_err_t weft_vk_alias_verify(weft_vk_ctx_t* c, weft_vk_mem_t* mem,
                                   weft_vk_compute_t* k) {
    if (!c || !mem || !k || !mem->map) return WEFT_VK_ERR_BAD_ARG;

    // A setup-time gate, not a hot-path call: it writes 16 bytes of
    // canary at the START of the imported span — run it before the
    // producer publishes real data (or on a scratch span), exactly how
    // the Series-10 canary discipline is applied on ring storage.
    static const uint32_t canary[4] = {
        0xC040FF80u, 0x33CC11AAu, 0x5A5A0F0Fu, 0xF00DBABEu
    };
    uint32_t* words = (uint32_t*)mem->map;
    for (int i = 0; i < 4; i++) words[i] = canary[i];

    // The oracle: scalar normalize over the same canary bytes.
    float oracle[16];
    {
        uint8_t bytes[16];
        memcpy(bytes, canary, 16);
        weft_ref_normalize_u8_to_f32(oracle, bytes, 16, 1.0f / 255.0f);
    }

    // A 256-byte mappable output, bound at binding 1.
    weft_vk_mem_t out_mem;
    weft_vk_err_t e = weft_vk_alloc_output(c, 256, 1, &out_mem);
    if (e != WEFT_VK_OK) return e;

    e = weft_vk_compute_bind(k, 0, mem->buffer, 0, 64);
    if (e == WEFT_VK_OK) {
        e = weft_vk_compute_bind(k, 1, out_mem.buffer, 0, 256);
    }
    if (e == WEFT_VK_OK) {
        weft_vk_preprocess_push_t push = {0};
        push.src_word_off = 0;
        push.dst_elem_off = 0;
        push.n_pixels = 4;
        push.scale = 1.0f / 255.0f;
        e = weft_vk_compute_dispatch(k, &push, 1, 1, 1);
    }
    int proven = 0;
    if (e == WEFT_VK_OK) {
        const float* gpu = (const float*)out_mem.map;
        proven = (memcmp(gpu, oracle, sizeof(oracle)) == 0);
    }
    weft_vk_mem_free(c, &out_mem);
    if (e != WEFT_VK_OK) return e;
    if (!proven) {
        // The device accepted the import but does NOT carry the CPU's
        // bytes (fresh-memory backing — the llvmpipe host-road discovery
        // of Series 10). REFUSED, never silently consumed.
        return WEFT_VK_ERR_ALIAS;
    }
    mem->alias_verified = 1;
    return WEFT_VK_OK;
}

// ---------------------------------------------------------------------------
// The frozen preprocess kernel
// ---------------------------------------------------------------------------

const uint32_t* weft_vk_preprocess_spv(uint32_t* out_words) {
    if (out_words) *out_words = WEFT_PREPROCESS_SPV_WORDS;
    return weft_preprocess_spv;
}

uint64_t weft_vk_preprocess_frozen_id(void) {
    return weft_accel_frozen_id(weft_preprocess_spv,
                                (size_t)WEFT_PREPROCESS_SPV_WORDS * 4u);
}

weft_vk_err_t weft_vk_preprocess_dispatch(weft_vk_compute_t* k,
                                          void* src_buffer,
                                          uint64_t src_byte_off,
                                          void* dst_buffer,
                                          uint64_t dst_byte_off,
                                          const weft_tensor_view_t* src_rgba8,
                                          float scale) {
    if (!k || !src_buffer || !dst_buffer || !src_rgba8) {
        return WEFT_VK_ERR_BAD_ARG;
    }
    // Law 2: the view ladder gates the GPU bind (LE, aligned, span-sane).
    weft_tv_err_t tv = weft_tensor_view_gpu_ready(src_rgba8);
    if (tv != WEFT_TV_OK) return WEFT_VK_ERR_VIEW;
    if ((src_byte_off & 3u) || (dst_byte_off & 3u)) {
        return WEFT_VK_ERR_VIEW;  // word offsets must be 4-byte aligned
    }
    if (src_rgba8->dtype != (uint8_t)WEFT_TENSOR_U8) {
        return WEFT_VK_ERR_VIEW;  // the frozen v1 kernel is the u8 road
    }
    uint32_t n_pixels = src_rgba8->elem_count / 4u;
    if (n_pixels == 0 || (src_rgba8->elem_count & 3u) != 0) {
        return WEFT_VK_ERR_VIEW;  // whole pixels only — never a guess
    }

    weft_vk_err_t e = weft_vk_compute_bind(k, 0, src_buffer, src_byte_off,
                                           src_rgba8->elem_count);
    if (e != WEFT_VK_OK) return e;
    e = weft_vk_compute_bind(k, 1, dst_buffer, dst_byte_off,
                             (uint64_t)n_pixels * 4u * sizeof(float));
    if (e != WEFT_VK_OK) return e;

    // The push offsets are RELATIVE TO THE BOUND RANGES: the descriptor's
    // VkDescriptorBufferInfo.offset already carries the absolute buffer
    // offset (src_byte_off/dst_byte_off), so the shader's array indices
    // start at 0 inside the bound window. (Double-counting the absolute
    // offset here writes past the window — caught by the bench's
    // bit-exact gate, not by a driver.)
    weft_vk_preprocess_push_t push = {0};
    push.src_word_off = 0;
    push.dst_elem_off = 0;
    push.n_pixels = n_pixels;
    push.scale = scale;
    uint32_t gx = (n_pixels + 63u) / 64u;  // local_size_x = 64 (frozen)
    return weft_vk_compute_dispatch(k, &push, gx, 1, 1);
}
