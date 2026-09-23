/* vk_heddle_probe.c — the NATIVE Vulkan leg of heddle-canvas (Tier 1).
 *
 * WHAT IT PROVES (and how):
 *   1. The WHP1 plane contract (native/whp1_layout.h, the C twin of
 *      src/plane/whp1.ts) is NATIVE-CONSUMABLE: the probe BUILDS a plane
 *      byte-for-byte (header, lane table, data region, the atomic-word
 *      layout) exactly as Engineer 1's engine would.
 *   2. The Tier-1 native road — DIRECT STORAGE-BUFFER COMPUTE over the
 *      plane's own memory — executes on a real ICD: the WHOLE PLANE is
 *      one mapped HOST_VISIBLE VkBuffer bound as SSBO; the decimation
 *      shader (shaders/vk/osc_decimate.comp, the Vulkan twin of the WGSL
 *      compute pass) reduces it in place.
 *   3. The reduction is BIT-EXACT against the C CPU oracle — the same
 *      reference window walk every tier implements (RFC-0022 §4.2), for
 *      BOTH a linear window (writePos < capacity) and a WRAPPED ring
 *      window (writePos > capacity).
 *
 * THE CLAIM'S BOUNDARY (printed in the output): the sandbox ICD is Mesa
 * lavapipe (software Vulkan). The proof is about CORRECTNESS of the
 * layout, the binding road and the shader math — not discrete-GPU
 * bandwidth; hardware numbers stay hardware-deferred (D-42 §6).
 *
 * Usage: vk-heddle-probe [--cols N]
 * Env:   WEFT_HEDDLE_SPV (default ../shaders/vk/osc_decimate.spv)
 *        VK_ICD_FILENAMES (lavapipe) as usual
 * Exit:  0 bit-exact PASS (both windows)
 *        1 mismatch (details printed)
 *        2 environment error (no libvulkan, alloc failure, shader missing)
 *        3 no Vulkan ICD (enumeration empty — named refusal, not a crash)
 *
 * Build: make -C native (glslangValidator rebuilds the .spv when present;
 *        the committed .spv is canonical otherwise — the house pattern).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "whp1_layout.h"
#include "vk_min.h"

// All Vulkan constants, struct layouts and the audited sType numbers come
// from the repo's vk_min.h (the Series-8 ABI-audited minimal Vulkan ABI —
// see its header comment: hand-remembered constants are exactly how this
// probe's first draft crashed, which is the audit's whole raison d'être).
#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0u
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1u
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO 2u
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO 3u
#define VK_STRUCTURE_TYPE_SUBMIT_INFO 4u
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5u
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 12u
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO 16u
#define VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO 18u
#define VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO 29u
#define VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO 30u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO 32u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO 33u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO 34u
#define VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET 35u
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 39u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 40u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 42u
// VK_BUFFER_USAGE_STORAGE_BUFFER_BIT: from vk_min.h
// VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT: from vk_min.h
// VK_MEMORY_PROPERTY_HOST_COHERENT_BIT: from vk_min.h
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER 7u
// VK_SHADER_STAGE_COMPUTE_BIT: from vk_min.h
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 2u
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 0u
// VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT: from vk_min.h
#define VK_PIPELINE_BIND_POINT_COMPUTE 1u
#define VK_API_VERSION_1_0 0x00400000u

static void die_env(const char* msg) { fprintf(stderr, "vk-heddle-probe: %s\n", msg); exit(2); }

// --- the deterministic waveform (f32, stable across compilers) --------------
static float wave_sample(uint64_t i) {
    uint32_t ph = (uint32_t)(i % 2048u);
    float tri = ph < 1024u ? (float)ph : 2047.0f - (float)ph;
    uint32_t noise = (uint32_t)(((i * 2654435761ull) >> 32) & 63u);
    return (tri + (float)noise - 512.0f) / 512.0f;
}

// --- the C CPU oracle: the reference window walk (RFC-0022 §4.2) ------------
static void cpu_decimate(const float* samples, uint32_t capacity, uint32_t vis,
                         uint32_t window_start, uint32_t cols, float* out) {
    uint32_t bucket = (vis + cols - 1) / cols;
    if (bucket == 0) bucket = 1;
    for (uint32_t c = 0; c < cols; c++) {
        uint64_t j0 = (uint64_t)c * bucket;
        if (j0 >= vis) { out[c * 2] = 0.0f; out[c * 2 + 1] = 0.0f; continue; }
        uint64_t j1 = (j0 + bucket < vis) ? j0 + bucket : vis;
        float lo = 3.402823466e38f, hi = -3.402823466e38f;
        for (uint64_t j = j0; j < j1; j++) {
            uint64_t slot = ((uint64_t)window_start + capacity - vis + j) % capacity;
            float v = samples[slot];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        out[c * 2] = lo;
        out[c * 2 + 1] = hi;
    }
}

// --- push constants (must match shaders/vk/osc_decimate.comp) ---------------
typedef struct {
    uint32_t sample_count;
    uint32_t column_count;
    uint32_t column_bucket;
    uint32_t window_start;
    uint32_t capacity;
    uint32_t viewport_w;
    uint32_t viewport_h;
    uint32_t reserved;
} PushParams;

int main(int argc, char** argv) {
    uint32_t cols = 1280;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) cols = (uint32_t)strtoul(argv[++i], 0, 0);
    }
    if (cols < 8 || cols > 8192) die_env("--cols outside 8..8192");

    // ---- 1. dlopen bootstrap (the house no-link discipline) ----------------
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == 0) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == 0) { fprintf(stderr, "vk-heddle-probe: no libvulkan — exit 3\n"); return 3; }

    void* (*vkGetInstanceProcAddr)(void*, const char*) =
        (void* (*)(void*, const char*))dlsym(lib, "vkGetInstanceProcAddr");

    VkApplicationInfo_ app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vk-heddle-probe";
    app.applicationVersion = 1;
    app.pEngineName = "weft-heddle-canvas";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo_ ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    void* instance = 0;
    int (*vkCreateInstance)(const VkInstanceCreateInfo_*, const void*, void**) =
        (int (*)(const VkInstanceCreateInfo_*, const void*, void**))vkGetInstanceProcAddr(0, "vkCreateInstance");
    if (vkCreateInstance == 0 || vkCreateInstance(&ici, 0, &instance) != 0)
        die_env("vkCreateInstance failed");
#define GPA(name) ((void*)vkGetInstanceProcAddr(instance, name))
    int (*vkEnumeratePhysicalDevices)(void*, uint32_t*, void*) = GPA("vkEnumeratePhysicalDevices");
    void (*vkGetPhysicalDeviceQueueFamilyProperties)(void*, uint32_t*, VkQueueFamilyProperties_*) = GPA("vkGetPhysicalDeviceQueueFamilyProperties");
    void (*vkGetPhysicalDeviceMemoryProperties)(void*, VkPhysicalDeviceMemoryProperties_*) = GPA("vkGetPhysicalDeviceMemoryProperties");
    void* (*vkGetDeviceProcAddr)(void*, const char*) = GPA("vkGetDeviceProcAddr");
    void (*vkDestroyInstance)(void*, const void*) = GPA("vkDestroyInstance");

    uint32_t pdCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &pdCount, 0) != 0 || pdCount == 0) {
        fprintf(stderr, "vk-heddle-probe: no Vulkan ICD (physical device enumeration empty) — exit 3\n");
        return 3;
    }
    void** pds = calloc(pdCount, sizeof(void*));
    if (vkEnumeratePhysicalDevices(instance, &pdCount, pds) != 0) die_env("physical device re-enumeration failed");
    void* pd = pds[0];
    free(pds);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, 0);
    VkQueueFamilyProperties_* qfs = calloc(qfCount ? qfCount : 1, sizeof(VkQueueFamilyProperties_));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs);
    uint32_t qf = 0;
    int found = 0;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; found = 1; break; }
    }
    free(qfs);
    if (!found) die_env("no compute queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo_ dqci = {0};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = qf;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &prio;
    VkDeviceCreateInfo_ dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    void* device = 0;
    int (*vkCreateDevice)(void*, const VkDeviceCreateInfo_*, const void*, void**) =
        (int (*)(void*, const VkDeviceCreateInfo_*, const void*, void**))GPA("vkCreateDevice");
    if (vkCreateDevice(pd, &dci, 0, &device) != 0) die_env("vkCreateDevice failed");
#define DPA(name) ((void*)vkGetDeviceProcAddr(device, name))
    void (*vkGetDeviceQueue)(void*, uint32_t, uint32_t, void**) = DPA("vkGetDeviceQueue");
    void (*vkDestroyDevice)(void*, const void*) = DPA("vkDestroyDevice");
    int (*vkCreateBuffer)(void*, const VkBufferCreateInfo_*, const void*, void**) = DPA("vkCreateBuffer");
    void (*vkDestroyBuffer)(void*, void*, const void*) = DPA("vkDestroyBuffer");
    void (*vkGetBufferMemoryRequirements)(void*, void*, VkMemoryRequirements_*) = DPA("vkGetBufferMemoryRequirements");
    int (*vkAllocateMemory)(void*, const VkMemoryAllocateInfo_*, const void*, void**) = DPA("vkAllocateMemory");
    void (*vkFreeMemory)(void*, void*, const void*) = DPA("vkFreeMemory");
    int (*vkMapMemory)(void*, void*, uint64_t, uint64_t, uint32_t, void**) = DPA("vkMapMemory");
    void (*vkUnmapMemory)(void*, void*) = DPA("vkUnmapMemory");
    int (*vkBindBufferMemory)(void*, void*, void*, uint64_t) = DPA("vkBindBufferMemory");
    int (*vkCreateShaderModule)(void*, const VkShaderModuleCreateInfo_*, const void*, void**) = DPA("vkCreateShaderModule");
    void (*vkDestroyShaderModule)(void*, void*, const void*) = DPA("vkDestroyShaderModule");
    int (*vkCreateDescriptorSetLayout)(void*, const VkDescriptorSetLayoutCreateInfo_*, const void*, void**) = DPA("vkCreateDescriptorSetLayout");
    void (*vkDestroyDescriptorSetLayout)(void*, void*, const void*) = DPA("vkDestroyDescriptorSetLayout");
    int (*vkCreatePipelineLayout)(void*, const VkPipelineLayoutCreateInfo_*, const void*, void**) = DPA("vkCreatePipelineLayout");
    void (*vkDestroyPipelineLayout)(void*, void*, const void*) = DPA("vkDestroyPipelineLayout");
    int (*vkCreateComputePipelines)(void*, void*, uint32_t, const VkComputePipelineCreateInfo_*, const void*, void**) = DPA("vkCreateComputePipelines");
    void (*vkDestroyPipeline)(void*, void*, const void*) = DPA("vkDestroyPipeline");
    int (*vkCreateDescriptorPool)(void*, const VkDescriptorPoolCreateInfo_*, const void*, void**) = DPA("vkCreateDescriptorPool");
    void (*vkDestroyDescriptorPool)(void*, void*, const void*) = DPA("vkDestroyDescriptorPool");
    int (*vkAllocateDescriptorSets)(void*, const VkDescriptorSetAllocateInfo_*, void**) = DPA("vkAllocateDescriptorSets");
    void (*vkUpdateDescriptorSets)(void*, uint32_t, const VkWriteDescriptorSet_*, uint32_t, const void*) = DPA("vkUpdateDescriptorSets");
    int (*vkCreateCommandPool)(void*, const VkCommandPoolCreateInfo_*, const void*, void**) = DPA("vkCreateCommandPool");
    void (*vkDestroyCommandPool)(void*, void*, const void*) = DPA("vkDestroyCommandPool");
    int (*vkAllocateCommandBuffers)(void*, const VkCommandBufferAllocateInfo_*, void**) = DPA("vkAllocateCommandBuffers");
    int (*vkBeginCommandBuffer)(void*, const VkCommandBufferBeginInfo_*) = DPA("vkBeginCommandBuffer");
    int (*vkEndCommandBuffer)(void*) = DPA("vkEndCommandBuffer");
    void (*vkCmdBindPipeline)(void*, uint32_t, void*) = DPA("vkCmdBindPipeline");
    void (*vkCmdBindDescriptorSets)(void*, uint32_t, void*, uint32_t, uint32_t, const void* const*, uint32_t, const uint32_t*) = DPA("vkCmdBindDescriptorSets");
    void (*vkCmdPushConstants)(void*, void*, uint32_t, uint32_t, uint32_t, const void*) = DPA("vkCmdPushConstants");
    void (*vkCmdDispatch)(void*, uint32_t, uint32_t, uint32_t) = DPA("vkCmdDispatch");
    int (*vkQueueSubmit)(void*, uint32_t, const VkSubmitInfo_*, void*) = DPA("vkQueueSubmit");
    int (*vkQueueWaitIdle)(void*) = DPA("vkQueueWaitIdle");
    // Instrumented load guard: any NULL device-level entry point is an
    // env error with the NAME (never a silent segfault).
    {
        const char* names[] = {
            "vkGetDeviceQueue", "vkDestroyDevice", "vkCreateBuffer", "vkDestroyBuffer",
            "vkGetBufferMemoryRequirements", "vkAllocateMemory", "vkFreeMemory", "vkMapMemory",
            "vkUnmapMemory", "vkBindBufferMemory", "vkCreateShaderModule", "vkDestroyShaderModule",
            "vkCreateDescriptorSetLayout", "vkDestroyDescriptorSetLayout", "vkCreatePipelineLayout",
            "vkDestroyPipelineLayout", "vkCreateComputePipelines", "vkDestroyPipeline",
            "vkCreateDescriptorPool", "vkDestroyDescriptorPool", "vkAllocateDescriptorSets",
            "vkUpdateDescriptorSets", "vkCreateCommandPool", "vkDestroyCommandPool",
            "vkAllocateCommandBuffers", "vkBeginCommandBuffer", "vkEndCommandBuffer",
            "vkCmdBindPipeline", "vkCmdBindDescriptorSets", "vkCmdPushConstants", "vkCmdDispatch",
            "vkQueueSubmit", "vkQueueWaitIdle", 0,
        };
        for (int i = 0; names[i]; i++) {
            if (DPA(names[i]) == 0) {
                fprintf(stderr, "vk-heddle-probe: device proc %s is NULL — exit 2\n", names[i]);
                return 2;
            }
        }
    }
    void* queue = 0;
    vkGetDeviceQueue(device, qf, 0, &queue);

    // ---- 2. build the WHP1 plane (producer role, byte-identical contract) --
    const uint32_t capacity = 1u << 20; // the mandate's 1,048,576 points
    const uint32_t gran = capacity / 64; // 16,384 elements per dirty bit
    const uint32_t strides[1] = { 4 };
    const uint32_t caps[1] = { capacity };
    const uint64_t planeBytes = whp1_plane_bytes(strides, caps, 1);
    const uint64_t dataStart = WHP1_DATA_START;

    VkMemoryRequirements_ planeReq = {0}, mmReq = {0};
    VkBufferCreateInfo_ bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = planeBytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    void* planeBuf = 0;
    if (vkCreateBuffer(device, &bci, 0, &planeBuf) != 0) die_env("plane VkBuffer create failed");
    vkGetBufferMemoryRequirements(device, planeBuf, &planeReq);

    VkBufferCreateInfo_ mci = bci;
    mci.size = (uint64_t)cols * 8;
    void* mmBuf = 0;
    if (vkCreateBuffer(device, &mci, 0, &mmBuf) != 0) die_env("minmax VkBuffer create failed");
    vkGetBufferMemoryRequirements(device, mmBuf, &mmReq);

    VkPhysicalDeviceMemoryProperties_ memProps;
    vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
    uint32_t memType = 0xffffffffu;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((memProps.memoryTypes[i].propertyFlags & want) == want) { memType = i; break; }
    }
    if (memType == 0xffffffffu) die_env("no HOST_VISIBLE|HOST_COHERENT memory type");

    // One device memory per buffer (simplicity; the probe's allocation
    // count is not the claim under test).
    VkMemoryAllocateInfo_ pmai = {0};
    pmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    pmai.allocationSize = planeReq.size;
    pmai.memoryTypeIndex = memType;
    void* planeMem = 0;
    if (vkAllocateMemory(device, &pmai, 0, &planeMem) != 0) die_env("plane memory alloc failed");
    if (vkBindBufferMemory(device, planeBuf, planeMem, 0) != 0) die_env("plane bind failed");
    VkMemoryAllocateInfo_ mmai = {0};
    mmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mmai.allocationSize = mmReq.size;
    mmai.memoryTypeIndex = memType;
    void* mmMem = 0;
    if (vkAllocateMemory(device, &mmai, 0, &mmMem) != 0) die_env("minmax memory alloc failed");
    if (vkBindBufferMemory(device, mmBuf, mmMem, 0) != 0) die_env("minmax bind failed");

    uint8_t* plane = 0;
    if (vkMapMemory(device, planeMem, 0, planeBytes, 0, (void**)&plane) != 0) die_env("plane map failed");
    float* mm = 0;
    if (vkMapMemory(device, mmMem, 0, mmReq.size, 0, (void**)&mm) != 0) die_env("minmax map failed");
    memset(plane, 0, planeBytes);

    // Header.
    uint32_t* hdr = (uint32_t*)plane;
    hdr[WHP1_OFF_MAGIC / 4] = WHP1_MAGIC;
    hdr[WHP1_OFF_VERSION / 4] = WHP1_VERSION;
    hdr[WHP1_OFF_HEADER_BYTES / 4] = WHP1_HEADER_BYTES;
    hdr[WHP1_OFF_LANE_COUNT / 4] = 1;
    hdr[WHP1_OFF_EPOCH / 4] = 1;
    hdr[WHP1_OFF_DATA_START / 4] = (uint32_t)dataStart;
    hdr[WHP1_OFF_DATA_START / 4 + 1] = (uint32_t)(dataStart >> 32);
    hdr[WHP1_OFF_PLANE_BYTES / 4] = (uint32_t)planeBytes;
    hdr[WHP1_OFF_PLANE_BYTES / 4 + 1] = (uint32_t)(planeBytes >> 32);
    // Lane 0 descriptor: WAVEFORM, gran, offset, capacity, stride.
    uint32_t* lane = (uint32_t*)(plane + WHP1_LANE_TABLE);
    lane[WHP1_LANE_OFF_MAGIC / 4] = WHP1_LANE_MAGIC;
    lane[WHP1_LANE_OFF_KIND / 4] = HP_KIND_WAVEFORM_F32;
    lane[WHP1_LANE_OFF_DTYPE / 4] = HP_DTYPE_F32;
    lane[WHP1_LANE_OFF_GRANULARITY / 4] = gran;
    *(uint64_t*)(plane + WHP1_LANE_TABLE + WHP1_LANE_OFF_OFFSET) = dataStart;
    *(uint64_t*)(plane + WHP1_LANE_TABLE + WHP1_LANE_OFF_CAPACITY) = capacity;
    *(uint64_t*)(plane + WHP1_LANE_TABLE + WHP1_LANE_OFF_STRIDE) = 4;
    lane[WHP1_LANE_OFF_FLAGS / 4] = WHP1_LANE_FLAG_ACTIVE;

    // Waveform data (all slots — the ring may wrap anywhere).
    float* samples = (float*)(plane + dataStart);
    for (uint64_t i = 0; i < capacity; i++) samples[i] = wave_sample(i);

    // ---- 3. compute pipeline from the .spv ------------------------------
    const char* spvPath = getenv("WEFT_HEDDLE_SPV");
    if (spvPath == 0 || spvPath[0] == 0) spvPath = "../shaders/vk/osc_decimate.spv";
    FILE* f = fopen(spvPath, "rb");
    if (f == 0) die_env("shader .spv not found (build with glslangValidator or commit it)");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t* code = malloc(sz);
    if (fread(code, 1, sz, f) != (size_t)sz) die_env("shader .spv read failed");
    fclose(f);
    VkShaderModuleCreateInfo_ smci = {0};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (uint64_t)sz;
    smci.pCode = code;
    void* module = 0;
    if (vkCreateShaderModule(device, &smci, 0, &module) != 0) die_env("shader module create failed");

    VkDescriptorSetLayoutBinding_ bindings[2] = {0};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo_ dslci = {0};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    void* setLayout = 0;
    if (vkCreateDescriptorSetLayout(device, &dslci, 0, &setLayout) != 0) die_env("descriptor set layout failed");

    VkPushConstantRange_ pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PushParams);
    VkPipelineLayoutCreateInfo_ plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    const void* const plLayoutPtrs[1] = { setLayout };
    plci.pSetLayouts = plLayoutPtrs;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    void* pipeLayout = 0;
    if (vkCreatePipelineLayout(device, &plci, 0, &pipeLayout) != 0) die_env("pipeline layout failed");

    VkPipelineShaderStageCreateInfo_ stage = {0};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo_ cpci = {0};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage = stage;
    cpci.layout = pipeLayout;
    void* pipeline = 0;
    if (vkCreateComputePipelines(device, 0, 1, &cpci, 0, &pipeline) != 0)
        die_env("compute pipeline create failed (shader compile refused?)");

    VkDescriptorPoolSize_ poolSize = {0};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo_ dpci = {0};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    void* pool = 0;
    if (vkCreateDescriptorPool(device, &dpci, 0, &pool) != 0) die_env("descriptor pool failed");
    VkDescriptorSetAllocateInfo_ dsai = {0};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    const void* const layoutPtrs[1] = { setLayout };
    dsai.pSetLayouts = layoutPtrs;
    void* set = 0;
    if (vkAllocateDescriptorSets(device, &dsai, &set) != 0) die_env("descriptor set alloc failed");

    // THE BINDING ROAD: the plane buffer bound with offset = the lane's
    // data start — the shader's samples[0] IS plane[dataStart]. Producer
    // bytes consumed in place, no staging copy anywhere.
    VkDescriptorBufferInfo_ bufInfos[2];
    memset(bufInfos, 0, sizeof(bufInfos));
    bufInfos[0].buffer = (uint64_t)planeBuf;
    bufInfos[0].offset = dataStart;
    bufInfos[0].range = (uint64_t)capacity * 4;
    bufInfos[1].buffer = (uint64_t)mmBuf;
    bufInfos[1].offset = 0;
    bufInfos[1].range = (uint64_t)cols * 8;
    VkWriteDescriptorSet_ writes[2];
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &bufInfos[0];
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufInfos[1];
    vkUpdateDescriptorSets(device, 2, writes, 0, 0);

    VkCommandPoolCreateInfo_ cpi = {0};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = qf;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    void* cmdPool = 0;
    if (vkCreateCommandPool(device, &cpi, 0, &cmdPool) != 0) die_env("command pool failed");
    VkCommandBufferAllocateInfo_ cbai = {0};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    void* cmd = 0;
    if (vkAllocateCommandBuffers(device, &cbai, &cmd) != 0) die_env("command buffer alloc failed");

    // ---- 4. two windows: linear then wrapped -----------------------------
    // Regime A: writePos = 600,000 (linear window, vis < capacity).
    // Regime B: writePos = capacity + 12345 (WRAPPED ring window).
    const uint32_t writePosA = 600000;
    const uint32_t writePosB = capacity + 12345;
    lane[WHP1_LANE_OFF_WRITE_POS / 4] = writePosA;
    lane[WHP1_LANE_OFF_SEQ / 4] = 2;
    // dirty word: whole-range mark (bits 0..63), the 64-bit producer road.
    *(uint64_t*)(plane + WHP1_LANE_TABLE + WHP1_LANE_OFF_DIRTY_LO) = 0xffffffffffffffffull;
    hdr[WHP1_OFF_EPOCH / 4] = 2;

    float* oracle = malloc((size_t)cols * 2 * sizeof(float));
    int failed = 0;

    for (int regime = 0; regime < 2 && !failed; regime++) {
        uint32_t writePos = regime == 0 ? writePosA : writePosB;
        uint32_t vis = writePos < capacity ? writePos : capacity;
        uint32_t windowStart = writePos % capacity;
        uint32_t bucket = (vis + cols - 1) / cols;
        if (bucket == 0) bucket = 1;

        PushParams p;
        memset(&p, 0, sizeof(p));
        p.sample_count = writePos;
        p.column_count = cols;
        p.column_bucket = bucket;
        p.window_start = windowStart;
        p.capacity = capacity;
        p.viewport_w = cols;
        p.viewport_h = 720;

        VkCommandBufferBeginInfo_ bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != 0) die_env("begin cmd failed");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const void* const setPtrs[1] = { set };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, setPtrs, 0, 0);
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushParams), &p);
        vkCmdDispatch(cmd, cols, 1, 1);
        if (vkEndCommandBuffer(cmd) != 0) die_env("end cmd failed");
        VkSubmitInfo_ si = {0};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        const void* const cmdPtrs[1] = { cmd };
        si.pCommandBuffers = cmdPtrs;
        if (vkQueueSubmit(queue, 1, &si, 0) != 0) die_env("submit failed");
        if (vkQueueWaitIdle(queue) != 0) die_env("wait idle failed");

        cpu_decimate(samples, capacity, vis, windowStart, cols, oracle);
        uint32_t mismatches = 0;
        uint32_t firstCol = 0;
        for (uint32_t c = 0; c < cols; c++) {
            if (memcmp(&mm[c * 2], &oracle[c * 2], 8) != 0) {
                if (mismatches == 0) firstCol = c;
                mismatches++;
            }
        }
        printf("regime-%s: writePos=%u vis=%u windowStart=%u bucket=%u -> %u/%u column mismatches %s\n",
               regime == 0 ? "A-linear" : "B-wrapped", writePos, vis, windowStart, bucket,
               mismatches, cols, mismatches == 0 ? "[BIT-EXACT]" : "");
        if (mismatches != 0) {
            printf("  first mismatch at column %u: gpu=[%f,%f] cpu=[%f,%f]\n",
                   firstCol, mm[firstCol * 2], mm[firstCol * 2 + 1],
                   oracle[firstCol * 2], oracle[firstCol * 2 + 1]);
            failed = 1;
        }
    }

    printf("heddle-vk-probe: %s (plane=%llu B bound at lane offset %llu, storage-buffer compute over producer memory; ICD per VK_ICD_FILENAMES)\n",
           failed ? "FAILED" : "ALL BIT-EXACT",
           (unsigned long long)planeBytes, (unsigned long long)dataStart);
    printf("{\"probe\":\"heddle-vk\",\"capacity\":%u,\"cols\":%u,\"plane_bytes\":%llu,\"verdict\":\"%s\"}\n",
           capacity, cols, (unsigned long long)planeBytes, failed ? "fail" : "pass");

    // teardown (the probe is short-lived; the OS reclaims, but the house
    // style tears down what it created when it can)
    vkDestroyCommandPool(device, cmdPool, 0);
    vkDestroyDescriptorPool(device, pool, 0);
    vkDestroyPipeline(device, pipeline, 0);
    vkDestroyPipelineLayout(device, pipeLayout, 0);
    vkDestroyDescriptorSetLayout(device, setLayout, 0);
    vkDestroyShaderModule(device, module, 0);
    vkUnmapMemory(device, planeMem);
    vkUnmapMemory(device, mmMem);
    vkFreeMemory(device, planeMem, 0);
    vkFreeMemory(device, mmMem, 0);
    vkDestroyBuffer(device, planeBuf, 0);
    vkDestroyBuffer(device, mmBuf, 0);
    vkDestroyDevice(device, 0);
    vkDestroyInstance(instance, 0);
    free(code);
    free(oracle);
    return failed ? 1 : 0;
}
