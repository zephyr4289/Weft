// weft_vk_bridge.c — RFC-0017 §3 implementation (part 1: ctx + imports).
//
// Layer discipline: tools layer (backends/vulkan), core/c untouched, no
// link-time Vulkan dependency (dlopen'd loader — the gpu_ring rule). The
// bootstrap mirrors the audited gpu_ring.c wrap pattern (Series 8/10);
// the compute kit (weft_vk_compute.c, same directory) mirrors
// gpu_stream.c's pooled-dispatch discipline with generalized bindings.
//
// Split note: implementation lives in two TUs — this one (context, memory
// imports, Road A session wraps) and weft_vk_compute.c (the pooled
// dispatch kit + alias canary + frozen preprocess). The split is purely
// editorial; both are compiled into every consumer.

#include "weft_vk_bridge.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vk_min.h"       // the core's size-asserted ABI decls
#include "gpu_ring.h"     // Road A: the substrate's wrap constructors
#include "weft/weft_accel_common.h"

// forward (defined with the Road-B memory block below)
weft_vk_err_t weft_vk_mem_map_ctx(weft_vk_ctx_t* c, weft_vk_mem_t* m);

// ---------------------------------------------------------------------------
// Error names (every refusal named — Law 4)
// ---------------------------------------------------------------------------

const char* weft_vk_err_name(weft_vk_err_t e) {
    switch (e) {
    case WEFT_VK_OK:              return "ok";
    case WEFT_VK_ERR_NO_LOADER:   return "no-vulkan-loader";
    case WEFT_VK_ERR_NO_DEVICE:   return "no-compute-device-with-extensions";
    case WEFT_VK_ERR_BAD_ARG:     return "bad-argument";
    case WEFT_VK_ERR_ALIGN:       return "pointer-below-min-import-alignment";
    case WEFT_VK_ERR_IMPORT:      return "memory-import-refused";
    case WEFT_VK_ERR_MEMORY_TYPE: return "no-host-visible-coherent-memory-type";
    case WEFT_VK_ERR_BUFFER:      return "buffer-create-or-bind-refused";
    case WEFT_VK_ERR_SHADER:      return "shader-module-refused";
    case WEFT_VK_ERR_FROZEN_ID:   return "kernel-frozen-id-mismatch";
    case WEFT_VK_ERR_PIPELINE:    return "pipeline-or-descriptor-refused";
    case WEFT_VK_ERR_COMMAND:     return "command-record-or-submit-refused";
    case WEFT_VK_ERR_ALIAS:       return "alias-canary-failed-fresh-memory-backing";
    case WEFT_VK_ERR_VIEW:        return "tensor-view-refused";
    case WEFT_VK_ERR_SESSION:     return "wfsh-session-invalid";
    default:                      return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Local entry-point types (the vk_min discipline: struct ABI shared, fn
// pointer types declared per-consumer; handles are void*)
// ---------------------------------------------------------------------------

typedef void* (*vkGetInstanceProcAddr_fn)(void*, const char*);
typedef int   (*vkEnumerateDeviceExtensionProperties_fn)(
    void*, const char*, uint32_t*, VkExtensionProperties_*);
typedef int   (*vkCreateInstance_fn)(const VkInstanceCreateInfo_*,
                                     const void*, void**);
typedef void  (*vkDestroyInstance_fn)(void*, const void*);
typedef int   (*vkEnumeratePhysicalDevices_fn)(void*, uint32_t*, void*);
typedef int   (*vkCreateDevice_fn)(void*, const VkDeviceCreateInfo_*,
                                   const void*, void**);
typedef void* (*vkGetDeviceProcAddr_fn)(void*, const char*);
typedef void  (*vkGetPhysicalDeviceQueueFamilyProperties_fn)(
    void*, uint32_t*, VkQueueFamilyProperties_*);
typedef void  (*vkGetPhysicalDeviceMemoryProperties_fn)(
    void*, VkPhysicalDeviceMemoryProperties_*);
typedef void  (*vkGetPhysicalDeviceProperties_fn)(
    void*, VkPhysicalDeviceProperties_*);
typedef void  (*vkGetPhysicalDeviceProperties2_fn)(
    void*, VkPhysicalDeviceProperties2_*);
typedef void  (*vkGetDeviceQueue_fn)(void*, uint32_t, uint32_t, void**);
typedef int   (*vkAllocateMemory_fn)(void*, const VkMemoryAllocateInfo_*,
                                     const void*, void**);
typedef void  (*vkFreeMemory_fn)(void*, void*, const void*);
typedef int   (*vkCreateBuffer_fn)(void*, const VkBufferCreateInfo_*,
                                   const void*, void**);
typedef void  (*vkDestroyBuffer_fn)(void*, void*, const void*);
typedef void  (*vkGetBufferMemoryRequirements_fn)(void*, void*,
                                                  VkMemoryRequirements_*);
typedef int   (*vkBindBufferMemory_fn)(void*, void*, void*, uint64_t);
typedef int   (*vkMapMemory_fn)(void*, void*, uint64_t, uint64_t, VkFlags_,
                                void**);
typedef void  (*vkUnmapMemory_fn)(void*, void*);

// ---------------------------------------------------------------------------
// ROAD B context — the standalone device bootstrap
// ---------------------------------------------------------------------------

struct weft_vk_ctx {
    void*       lib;
    void*       instance;
    void*       physical;
    void*       device;
    void*       queue;
    uint32_t    queue_family;
    weft_vk_dev_t dev;
    char        device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    int         has_fd_ext;
    int         has_dmabuf_ext;
    int         has_host_ext;
    uint64_t    min_host_align;
    VkPhysicalDeviceMemoryProperties_ memprops;
};

/// The ctx's resolver — resolves device-level entry points on ctx->device.
static const void* weft_vk_ctx_resolve(void* user, const char* name) {
    weft_vk_ctx_t* c = (weft_vk_ctx_t*)user;
    if (!c || !c->lib || !c->instance || !c->device) return NULL;
    vkGetInstanceProcAddr_fn gpa =
        (vkGetInstanceProcAddr_fn)dlsym(c->lib, "vkGetInstanceProcAddr");
    if (!gpa) return NULL;
    vkGetDeviceProcAddr_fn gdpa =
        (vkGetDeviceProcAddr_fn)(void*)gpa(c->instance, "vkGetDeviceProcAddr");
    return gdpa ? gdpa(c->device, name) : NULL;
}

/// vkEnumerateDeviceExtensionProperties returns VK_INCOMPLETE (5) when
/// the caller's property buffer is smaller than the device's full list
/// (lavapipe advertises 150+): a SUCCESS-class result for enumeration
/// purposes. Anything else is a real refusal.
#define WEFT_VK_INCOMPLETE 5

static int vk_has_extension(
    vkEnumerateDeviceExtensionProperties_fn enumerate, void* phys,
    const char* name) {
    uint32_t n = 0;
    if (enumerate(phys, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    // Full-size buffer: real devices advertise 150+ extensions and the
    // list order is the ICD's, not alphabetical — a fixed window can
    // straddle the wanted name. Setup path: one allocation, freed here
    // (Law 1 governs the hot path, not this).
    VkExtensionProperties_* props =
        (VkExtensionProperties_*)malloc((size_t)n * sizeof(*props));
    if (!props) return 0;
    int r = enumerate(phys, NULL, &n, props);
    int found = 0;
    if (r == VK_SUCCESS || r == WEFT_VK_INCOMPLETE) {
        for (uint32_t i = 0; i < n; i++) {
            if (strncmp(props[i].extensionName, name,
                        VK_MAX_EXTENSION_NAME_SIZE) == 0) {
                found = 1;
                break;
            }
        }
    }
    free(props);
    return found;
}

weft_vk_err_t weft_vk_ctx_create(weft_vk_ctx_t** out, const char* app_tag) {
    if (!out) return WEFT_VK_ERR_BAD_ARG;
    *out = NULL;

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return WEFT_VK_ERR_NO_LOADER;

    vkGetInstanceProcAddr_fn gpa =
        (vkGetInstanceProcAddr_fn)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gpa) { dlclose(lib); return WEFT_VK_ERR_NO_LOADER; }
    vkCreateInstance_fn vkCreateInstance =
        (vkCreateInstance_fn)(void*)gpa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) {
        dlclose(lib);
        return WEFT_VK_ERR_NO_LOADER;
    }

    VkApplicationInfo_ app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = app_tag ? app_tag : "weft-tensor";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo_ ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    void* instance = NULL;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) {
        dlclose(lib);
        return WEFT_VK_ERR_NO_LOADER;  // loader present, ICD absent — the
                                       // no-ICD refusal leg is a CI gate
    }

    // vkDestroyInstance is an INSTANCE-level command — the spec allows
    // vkGetInstanceProcAddr(NULL, ...) to resolve ONLY the four global
    // commands, so this resolves AFTER the instance exists (the loader
    // discipline gpu_ring follows; resolving it pre-instance returns NULL
    // on conforming loaders).
    vkDestroyInstance_fn vkDestroyInstance =
        (vkDestroyInstance_fn)(void*)gpa(instance, "vkDestroyInstance");
    if (!vkDestroyInstance) {
        dlclose(lib);
        return WEFT_VK_ERR_NO_LOADER;
    }

    vkEnumeratePhysicalDevices_fn vkEnumeratePhysicalDevices =
        (vkEnumeratePhysicalDevices_fn)(void*)gpa(instance,
                                                  "vkEnumeratePhysicalDevices");
    vkEnumerateDeviceExtensionProperties_fn vkEnumerateDeviceExtensionProperties =
        (vkEnumerateDeviceExtensionProperties_fn)(void*)gpa(
            instance, "vkEnumerateDeviceExtensionProperties");
    vkCreateDevice_fn vkCreateDevice =
        (vkCreateDevice_fn)(void*)gpa(instance, "vkCreateDevice");
    vkGetDeviceProcAddr_fn vkGetDeviceProcAddr =
        (vkGetDeviceProcAddr_fn)(void*)gpa(instance, "vkGetDeviceProcAddr");
    vkGetPhysicalDeviceQueueFamilyProperties_fn
        vkGetPhysicalDeviceQueueFamilyProperties =
            (vkGetPhysicalDeviceQueueFamilyProperties_fn)(void*)gpa(
                instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    vkGetPhysicalDeviceMemoryProperties_fn vkGetPhysicalDeviceMemoryProperties =
        (vkGetPhysicalDeviceMemoryProperties_fn)(void*)gpa(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    vkGetPhysicalDeviceProperties_fn vkGetPhysicalDeviceProperties =
        (vkGetPhysicalDeviceProperties_fn)(void*)gpa(
            instance, "vkGetPhysicalDeviceProperties");
    vkGetPhysicalDeviceProperties2_fn vkGetPhysicalDeviceProperties2 =
        (vkGetPhysicalDeviceProperties2_fn)(void*)gpa(
            instance, "vkGetPhysicalDeviceProperties2");
    if (!vkEnumeratePhysicalDevices || !vkEnumerateDeviceExtensionProperties ||
        !vkCreateDevice || !vkGetDeviceProcAddr ||
        !vkGetPhysicalDeviceQueueFamilyProperties ||
        !vkGetPhysicalDeviceMemoryProperties || !vkGetPhysicalDeviceProperties) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return WEFT_VK_ERR_NO_LOADER;
    }

    uint32_t nphys = 0;
    if (vkEnumeratePhysicalDevices(instance, &nphys, NULL) != VK_SUCCESS ||
        nphys == 0) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return WEFT_VK_ERR_NO_DEVICE;
    }
    void* phys[4] = {0};
    uint32_t ngot = nphys > 4 ? 4 : nphys;
    if (vkEnumeratePhysicalDevices(instance, &ngot, phys) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return WEFT_VK_ERR_NO_DEVICE;
    }

    // First physical device with a compute queue family AND fd+dmabuf
    // import extensions (the camera road's hard requirement; host-import
    // presence is recorded — the shm road).
    uint32_t queue_family = 0xFFFFFFFFu;
    void* chosen = NULL;
    VkQueueFamilyProperties_ fam[8];
    VkPhysicalDeviceProperties_ props;
    VkPhysicalDeviceMemoryProperties_ memprops;
    int has_fd = 0, has_dmabuf = 0, has_host = 0;
    for (uint32_t d = 0; d < ngot && !chosen; d++) {
        if (!vk_has_extension(vkEnumerateDeviceExtensionProperties, phys[d],
                              VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
            continue;
        }
        if (!vk_has_extension(vkEnumerateDeviceExtensionProperties, phys[d],
                              VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
            continue;  // fd road without dma-buf handles cannot import heaps
        }
        has_fd = 1;
        has_dmabuf = 1;
        has_host = vk_has_extension(vkEnumerateDeviceExtensionProperties,
                                    phys[d],
                                    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nfam, NULL);
        if (nfam == 0 || nfam > 8) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nfam, fam);
        for (uint32_t f = 0; f < nfam; f++) {
            if (fam[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queue_family = f;
                chosen = phys[d];
                vkGetPhysicalDeviceProperties(chosen, &props);
                vkGetPhysicalDeviceMemoryProperties(chosen, &memprops);
                break;
            }
        }
    }
    if (!chosen) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return WEFT_VK_ERR_NO_DEVICE;
    }

    const char* exts[3];
    uint32_t nexts = 0;
    exts[nexts++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    exts[nexts++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    if (has_host) exts[nexts++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo_ qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo_ dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = nexts;
    dci.ppEnabledExtensionNames = exts;
    void* device = NULL;
    if (vkCreateDevice(chosen, &dci, NULL, &device) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return WEFT_VK_ERR_NO_DEVICE;
    }

    weft_vk_ctx_t* c = (weft_vk_ctx_t*)calloc(1, sizeof(*c));
    if (!c) {
        vkGetDeviceProcAddr_fn gdpa2 =
            (vkGetDeviceProcAddr_fn)(void*)gpa(instance, "vkGetDeviceProcAddr");
        void (*vkDestroyDevice)(void*, const void*) =
            (void (*)(void*, const void*))gdpa2(device, "vkDestroyDevice");
        if (vkDestroyDevice) vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return WEFT_VK_ERR_NO_LOADER;  // setup OOM: honest refusal
    }
    c->lib = lib;
    c->instance = instance;
    c->physical = chosen;
    c->device = device;
    c->queue_family = queue_family;
    c->has_fd_ext = has_fd;
    c->has_dmabuf_ext = has_dmabuf;
    c->has_host_ext = has_host;
    c->memprops = memprops;
    memcpy(c->device_name, props.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);

    // Host-import alignment contract (mirrors the substrate's query).
    c->min_host_align = 4096;  // spec floor until the device says more
    if (vkGetPhysicalDeviceProperties2 && has_host) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT_ hp = {0};
        hp.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2_ p2 = {0};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &hp;
        vkGetPhysicalDeviceProperties2(chosen, &p2);
        if (hp.minImportedHostPointerAlignment != 0) {
            c->min_host_align = hp.minImportedHostPointerAlignment;
        }
    }

    vkGetDeviceQueue_fn vkGetDeviceQueue =
        (vkGetDeviceQueue_fn)(void*)vkGetDeviceProcAddr(device,
                                                        "vkGetDeviceQueue");
    if (!vkGetDeviceQueue) {
        free(c);
        vkGetDeviceProcAddr_fn gdpa2 =
            (vkGetDeviceProcAddr_fn)(void*)gpa(instance, "vkGetDeviceProcAddr");
        void (*vkDestroyDevice)(void*, const void*) =
            (void (*)(void*, const void*))gdpa2(device, "vkDestroyDevice");
        if (vkDestroyDevice) vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return WEFT_VK_ERR_NO_DEVICE;
    }
    vkGetDeviceQueue(device, queue_family, 0, &c->queue);

    c->dev.device = device;
    c->dev.queue = c->queue;
    c->dev.queue_family = queue_family;
    c->dev.proc = weft_vk_ctx_resolve;
    c->dev.proc_user = c;
    *out = c;
    return WEFT_VK_OK;
}

const char* weft_vk_ctx_device_name(const weft_vk_ctx_t* c) {
    return c ? c->device_name : "(null)";
}
int weft_vk_ctx_has_dmabuf_ext(const weft_vk_ctx_t* c) {
    return c ? c->has_dmabuf_ext : 0;
}
int weft_vk_ctx_has_host_ext(const weft_vk_ctx_t* c) {
    return c ? c->has_host_ext : 0;
}
uint64_t weft_vk_ctx_min_host_align(const weft_vk_ctx_t* c) {
    return c ? c->min_host_align : 0;
}
const weft_vk_dev_t* weft_vk_ctx_dev(const weft_vk_ctx_t* c) {
    return c ? &c->dev : NULL;
}

void weft_vk_ctx_destroy(weft_vk_ctx_t* c) {
    if (!c) return;
    vkGetInstanceProcAddr_fn gpa =
        (vkGetInstanceProcAddr_fn)dlsym(c->lib, "vkGetInstanceProcAddr");
    if (c->device && gpa) {
        vkGetDeviceProcAddr_fn gdpa =
            (vkGetDeviceProcAddr_fn)(void*)gpa(c->instance,
                                                "vkGetDeviceProcAddr");
        if (gdpa) {
            void (*vkDestroyDevice)(void*, const void*) =
                (void (*)(void*, const void*))gdpa(c->device,
                                                    "vkDestroyDevice");
            if (vkDestroyDevice) vkDestroyDevice(c->device, NULL);
        }
    }
    if (c->instance && gpa) {
        vkDestroyInstance_fn vkDestroyInstance =
            (vkDestroyInstance_fn)(void*)gpa(c->instance,
                                              "vkDestroyInstance");
        if (vkDestroyInstance) vkDestroyInstance(c->instance, NULL);
    }
    if (c->lib) dlclose(c->lib);
    free(c);
}

// ---------------------------------------------------------------------------
// ROAD B memory — imports + output allocations
// ---------------------------------------------------------------------------

static weft_vk_err_t vk_pick_memory_type(const weft_vk_ctx_t* c,
                                         const VkMemoryRequirements_* req,
                                         VkFlags_ want, int* out_type) {
    for (uint32_t m = 0; m < c->memprops.memoryTypeCount; m++) {
        if (!(req->memoryTypeBits & (1u << m))) continue;
        if ((c->memprops.memoryTypes[m].propertyFlags & want) == want) {
            *out_type = (int)m;
            return WEFT_VK_OK;
        }
    }
    return WEFT_VK_ERR_MEMORY_TYPE;
}

/// Shared buffer-over-import plumbing: create the external-handle buffer,
/// pick the memory type, allocate with the import pNext chain, bind.
static weft_vk_err_t vk_import_common(weft_vk_ctx_t* c, uint64_t bytes,
                                      VkFlags_ buffer_handle_type,
                                      const void* alloc_pNext,
                                      weft_vk_mem_t* out) {
    if (!c || !out || bytes == 0) return WEFT_VK_ERR_BAD_ARG;
    const weft_vk_dev_t* dev = &c->dev;

    vkCreateBuffer_fn vkCreateBuffer =
        (vkCreateBuffer_fn)(void*)dev->proc(dev->proc_user, "vkCreateBuffer");
    vkDestroyBuffer_fn vkDestroyBuffer =
        (vkDestroyBuffer_fn)(void*)dev->proc(dev->proc_user, "vkDestroyBuffer");
    vkGetBufferMemoryRequirements_fn vkGetBufferMemoryRequirements =
        (vkGetBufferMemoryRequirements_fn)(void*)dev->proc(
            dev->proc_user, "vkGetBufferMemoryRequirements");
    vkAllocateMemory_fn vkAllocateMemory =
        (vkAllocateMemory_fn)(void*)dev->proc(dev->proc_user,
                                              "vkAllocateMemory");
    vkFreeMemory_fn vkFreeMemory =
        (vkFreeMemory_fn)(void*)dev->proc(dev->proc_user, "vkFreeMemory");
    vkBindBufferMemory_fn vkBindBufferMemory =
        (vkBindBufferMemory_fn)(void*)dev->proc(dev->proc_user,
                                                "vkBindBufferMemory");
    if (!vkCreateBuffer || !vkDestroyBuffer ||
        !vkGetBufferMemoryRequirements || !vkAllocateMemory ||
        !vkFreeMemory || !vkBindBufferMemory) {
        return WEFT_VK_ERR_NO_DEVICE;
    }

    VkExternalMemoryBufferInfo_ ext_buf = {0};
    ext_buf.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_INFO;
    ext_buf.handleTypes = buffer_handle_type;
    VkBufferCreateInfo_ bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &ext_buf;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    void* buffer = NULL;
    if (vkCreateBuffer(c->device, &bci, NULL, &buffer) != VK_SUCCESS) {
        return WEFT_VK_ERR_BUFFER;
    }

    VkMemoryRequirements_ req;
    vkGetBufferMemoryRequirements(c->device, buffer, &req);
    int memtype = -1;
    weft_vk_err_t e = vk_pick_memory_type(
        c, &req,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &memtype);
    if (e != WEFT_VK_OK) {
        vkDestroyBuffer(c->device, buffer, NULL);
        return e;
    }

    VkMemoryAllocateInfo_ mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = alloc_pNext;
    mai.allocationSize = req.size;  // device requirement (>= bytes)
    mai.memoryTypeIndex = (uint32_t)memtype;
    void* memory = NULL;
    if (vkAllocateMemory(c->device, &mai, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(c->device, buffer, NULL);
        return WEFT_VK_ERR_IMPORT;
    }
    if (vkBindBufferMemory(c->device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(c->device, memory, NULL);
        vkDestroyBuffer(c->device, buffer, NULL);
        return WEFT_VK_ERR_BUFFER;
    }

    out->buffer = buffer;
    out->memory = memory;
    out->map = NULL;
    out->bytes = bytes;
    out->alias_verified = 0;
    return WEFT_VK_OK;
}

weft_vk_err_t weft_vk_import_fd(weft_vk_ctx_t* c, int fd, uint64_t bytes,
                                weft_vk_mem_t* out) {
    if (!c || !out || fd < 0) return WEFT_VK_ERR_BAD_ARG;
    if (!c->has_fd_ext || !c->has_dmabuf_ext) return WEFT_VK_ERR_NO_DEVICE;

    // The fd is dup'd INTO the import: Vulkan consumes the fd on success
    // (ownership transfer per spec) — the caller's fd stays open because
    // we dup first (the substrate's borrowed-fd discipline).
    int dup_fd = dup(fd);
    if (dup_fd < 0) return WEFT_VK_ERR_BAD_ARG;
    VkImportMemoryFdInfoKHR_ imp = {0};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd = dup_fd;
    weft_vk_err_t e = vk_import_common(
        c, bytes, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &imp, out);
    if (e != WEFT_VK_OK) {
        close(dup_fd);  // not consumed on failure — release the dup
        return e;
    }
    e = weft_vk_mem_map_ctx(c, out);  // host-visible by construction
    if (e != WEFT_VK_OK) {
        weft_vk_mem_free(c, out);
        return e;
    }
    return WEFT_VK_OK;
}

weft_vk_err_t weft_vk_import_host(weft_vk_ctx_t* c, void* ptr,
                                  uint64_t bytes, weft_vk_mem_t* out) {
    if (!c || !out || !ptr) return WEFT_VK_ERR_BAD_ARG;
    if (!c->has_host_ext) return WEFT_VK_ERR_NO_DEVICE;
    if (((uintptr_t)ptr % (uintptr_t)c->min_host_align) != 0) {
        return WEFT_VK_ERR_ALIGN;  // malloc'd memory — the documented
                                   // shm/memfd/dmabuf road
    }
    VkImportMemoryHostPointerInfoEXT_ imp = {0};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = ptr;
    weft_vk_err_t e = vk_import_common(
        c, bytes, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        &imp, out);
    if (e != WEFT_VK_OK) return e;
    e = weft_vk_mem_map_ctx(c, out);
    if (e != WEFT_VK_OK) {
        weft_vk_mem_free(c, out);
        return e;
    }
    // The host import's mapping aliases the caller's own pointer — keep
    // the CPU view as the caller's pointer (the map is the import).
    out->map = ptr;
    return WEFT_VK_OK;
}

weft_vk_err_t weft_vk_alloc_output(weft_vk_ctx_t* c, uint64_t bytes,
                                   int host_visible, weft_vk_mem_t* out) {
    if (!c || !out || bytes == 0) return WEFT_VK_ERR_BAD_ARG;
    const weft_vk_dev_t* dev = &c->dev;
    vkCreateBuffer_fn vkCreateBuffer =
        (vkCreateBuffer_fn)(void*)dev->proc(dev->proc_user, "vkCreateBuffer");
    vkDestroyBuffer_fn vkDestroyBuffer =
        (vkDestroyBuffer_fn)(void*)dev->proc(dev->proc_user, "vkDestroyBuffer");
    vkGetBufferMemoryRequirements_fn vkGetBufferMemoryRequirements =
        (vkGetBufferMemoryRequirements_fn)(void*)dev->proc(
            dev->proc_user, "vkGetBufferMemoryRequirements");
    vkAllocateMemory_fn vkAllocateMemory =
        (vkAllocateMemory_fn)(void*)dev->proc(dev->proc_user,
                                              "vkAllocateMemory");
    vkFreeMemory_fn vkFreeMemory =
        (vkFreeMemory_fn)(void*)dev->proc(dev->proc_user, "vkFreeMemory");
    vkBindBufferMemory_fn vkBindBufferMemory =
        (vkBindBufferMemory_fn)(void*)dev->proc(dev->proc_user,
                                                "vkBindBufferMemory");
    if (!vkCreateBuffer || !vkDestroyBuffer ||
        !vkGetBufferMemoryRequirements || !vkAllocateMemory ||
        !vkFreeMemory || !vkBindBufferMemory) {
        return WEFT_VK_ERR_NO_DEVICE;
    }

    VkBufferCreateInfo_ bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    void* buffer = NULL;
    if (vkCreateBuffer(c->device, &bci, NULL, &buffer) != VK_SUCCESS) {
        return WEFT_VK_ERR_BUFFER;
    }
    VkMemoryRequirements_ req;
    vkGetBufferMemoryRequirements(c->device, buffer, &req);
    int memtype = -1;
    VkFlags_ want = host_visible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    weft_vk_err_t e = vk_pick_memory_type(c, &req, want, &memtype);
    if (e != WEFT_VK_OK) {
        vkDestroyBuffer(c->device, buffer, NULL);
        return e;
    }
    VkMemoryAllocateInfo_ mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)memtype;
    void* memory = NULL;
    if (vkAllocateMemory(c->device, &mai, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(c->device, buffer, NULL);
        return WEFT_VK_ERR_IMPORT;
    }
    if (vkBindBufferMemory(c->device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(c->device, memory, NULL);
        vkDestroyBuffer(c->device, buffer, NULL);
        return WEFT_VK_ERR_BUFFER;
    }
    out->buffer = buffer;
    out->memory = memory;
    out->map = NULL;
    out->bytes = bytes;
    out->alias_verified = 0;
    if (host_visible) {
        weft_vk_err_t e = weft_vk_mem_map_ctx(c, out);
        if (e != WEFT_VK_OK) {
            weft_vk_mem_free(c, out);
            return e;
        }
    }
    return WEFT_VK_OK;
}

void* weft_vk_mem_map(weft_vk_mem_t* m) {
    return m ? m->map : NULL;
}

/// Map a Road-B memory on its ctx (setup path; imports are HOST_VISIBLE by
/// construction, so the map always succeeds or the import refuses).
weft_vk_err_t weft_vk_mem_map_ctx(weft_vk_ctx_t* c, weft_vk_mem_t* m) {
    if (!c || !m || m->map) {
        return (c && m) ? WEFT_VK_OK : WEFT_VK_ERR_BAD_ARG;
    }
    vkGetInstanceProcAddr_fn gpa =
        (vkGetInstanceProcAddr_fn)dlsym(c->lib, "vkGetInstanceProcAddr");
    if (!gpa) return WEFT_VK_ERR_NO_LOADER;
    vkGetDeviceProcAddr_fn gdpa =
        (vkGetDeviceProcAddr_fn)(void*)gpa(c->instance, "vkGetDeviceProcAddr");
    if (!gdpa) return WEFT_VK_ERR_NO_LOADER;
    vkMapMemory_fn vkMapMemory =
        (vkMapMemory_fn)(void*)gdpa(c->device, "vkMapMemory");
    if (!vkMapMemory) return WEFT_VK_ERR_NO_DEVICE;
    void* p = NULL;
    if (vkMapMemory(c->device, m->memory, 0, VK_WHOLE_SIZE, 0, &p) !=
        VK_SUCCESS) {
        return WEFT_VK_ERR_IMPORT;
    }
    m->map = p;
    return WEFT_VK_OK;
}

void weft_vk_mem_unmap(weft_vk_ctx_t* c, weft_vk_mem_t* m) {
    if (!c || !m || !m->map) return;
    vkGetInstanceProcAddr_fn gpa =
        (vkGetInstanceProcAddr_fn)dlsym(c->lib, "vkGetInstanceProcAddr");
    if (!gpa) return;
    vkGetDeviceProcAddr_fn gdpa =
        (vkGetDeviceProcAddr_fn)(void*)gpa(c->instance, "vkGetDeviceProcAddr");
    if (!gdpa) return;
    vkUnmapMemory_fn vkUnmapMemory =
        (vkUnmapMemory_fn)(void*)gdpa(c->device, "vkUnmapMemory");
    if (vkUnmapMemory) vkUnmapMemory(c->device, m->memory);
    m->map = NULL;
}

void weft_vk_mem_free(weft_vk_ctx_t* c, weft_vk_mem_t* m) {
    if (!c || !m) return;
    weft_vk_mem_unmap(c, m);
    vkGetInstanceProcAddr_fn gpa =
        (vkGetInstanceProcAddr_fn)dlsym(c->lib, "vkGetInstanceProcAddr");
    if (!gpa) return;
    vkGetDeviceProcAddr_fn gdpa =
        (vkGetDeviceProcAddr_fn)(void*)gpa(c->instance, "vkGetDeviceProcAddr");
    if (!gdpa) return;
    vkDestroyBuffer_fn vkDestroyBuffer =
        (vkDestroyBuffer_fn)(void*)gdpa(c->device, "vkDestroyBuffer");
    vkFreeMemory_fn vkFreeMemory =
        (vkFreeMemory_fn)(void*)gdpa(c->device, "vkFreeMemory");
    if (m->buffer && vkDestroyBuffer) vkDestroyBuffer(c->device, m->buffer, NULL);
    if (m->memory && vkFreeMemory) vkFreeMemory(c->device, m->memory, NULL);
    memset(m, 0, sizeof(*m));
}

// ---------------------------------------------------------------------------
// ROAD A — wrap a WFSH ring session through the Series-10 substrate
// ---------------------------------------------------------------------------

struct weft_vk_ring {
    weft_gpu_ring_t* g;      // the substrate session (wrap constructors)
    weft_vk_dev_t dev;       // its device, exposed for the compute kit
    uint32_t queue_family;
    int borrowed;            // 1 = native session (destroy frees only the
                             // wrapper); 0 = wrap (destroy frees the wrap)
};

static const void* weft_vk_ring_resolve(void* user, const char* name) {
    // user points at the weft_vk_ring_t's owner — see wrap below: the
    // resolver forwards to weft_gpu_vk_proc on the SUBSTRATE session.
    weft_gpu_ring_t* g = (weft_gpu_ring_t*)user;
    return weft_gpu_vk_proc(g, name);
}

static weft_vk_err_t ring_wrap_common(weft_vk_ring_t** out,
                                      weft_gpu_ring_t* g, int borrowed) {
    weft_vk_ring_t* r = (weft_vk_ring_t*)calloc(1, sizeof(*r));
    if (!r) {
        if (!borrowed) weft_gpu_destroy(g);
        return WEFT_VK_ERR_NO_LOADER;  // setup OOM: honest refusal
    }
    r->g = g;
    r->borrowed = borrowed;
    r->dev.device = (void*)weft_gpu_vk_device(g);
    r->queue_family = weft_gpu_vk_queue_family(g);
    r->dev.queue_family = r->queue_family;
    r->dev.proc = weft_vk_ring_resolve;
    r->dev.proc_user = g;
    // The queue handle: resolve vkGetDeviceQueue on the wrapped device.
    vkGetDeviceQueue_fn vkGetDeviceQueue =
        (vkGetDeviceQueue_fn)(void*)weft_gpu_vk_proc(g, "vkGetDeviceQueue");
    if (!vkGetDeviceQueue) {
        free(r);
        if (!borrowed) weft_gpu_destroy(g);
        return WEFT_VK_ERR_NO_DEVICE;
    }
    vkGetDeviceQueue(r->dev.device, r->queue_family, 0, &r->dev.queue);
    *out = r;
    return WEFT_VK_OK;
}

weft_vk_err_t weft_vk_ring_of_gpu_session(weft_vk_ring_t** out,
                                          weft_gpu_ring_t* g) {
    if (!out || !g) return WEFT_VK_ERR_BAD_ARG;
    if (weft_gpu_backend(g) != WEFT_GPU_BACKEND_VULKAN) {
        return WEFT_VK_ERR_SESSION;  // CPU/Metal sessions carry no device
    }
    return ring_wrap_common(out, g, 1);
}

weft_vk_err_t weft_vk_ring_wrap_host(weft_vk_ring_t** out,
                                     size_t payload_bytes,
                                     unsigned slot_count, void* session_mem,
                                     size_t map_bytes) {
    if (!out || !session_mem) return WEFT_VK_ERR_BAD_ARG;
    weft_gpu_ring_t* g = NULL;
    // The substrate validates the WFSH session, imports, and runs the
    // alias canary itself (ALIAS-VERIFIED by construction).
    if (weft_gpu_wrap_host(&g, payload_bytes, slot_count, session_mem,
                           map_bytes) != 0) {
        return WEFT_VK_ERR_SESSION;
    }
    return ring_wrap_common(out, g, 0);
}

weft_vk_err_t weft_vk_ring_wrap_fd(weft_vk_ring_t** out,
                                   size_t payload_bytes,
                                   unsigned slot_count, int fd) {
    if (!out || fd < 0) return WEFT_VK_ERR_BAD_ARG;
    weft_gpu_ring_t* g = NULL;
    if (weft_gpu_wrap_dmabuf(&g, payload_bytes, slot_count, fd) != 0) {
        return WEFT_VK_ERR_SESSION;
    }
    return ring_wrap_common(out, g, 0);
}

const weft_vk_dev_t* weft_vk_ring_dev(const weft_vk_ring_t* r) {
    return r ? &r->dev : NULL;
}

void* weft_vk_ring_buffer(const weft_vk_ring_t* r) {
    return r ? (void*)weft_gpu_vk_buffer(r->g) : NULL;
}

uint64_t weft_vk_ring_buffer_bytes(const weft_vk_ring_t* r) {
    return r ? weft_gpu_vk_buffer_bytes(r->g) : 0;
}

void weft_vk_ring_slot_range(const weft_vk_ring_t* r, unsigned slot,
                             uint64_t* off, uint64_t* len) {
    if (!r) {
        if (off) *off = 0;
        if (len) *len = 0;
        return;
    }
    // The session-span buffer: 64 B WFSH header + RFC-0004 ring
    // (16 B ctrl + 8*M slotSeq + M*payload). Slot k's payload:
    unsigned m = weft_gpu_slot_count(r->g);
    size_t payload = weft_gpu_payload_bytes(r->g);
    if (slot >= m) {
        if (off) *off = 0;
        if (len) *len = 0;
        return;
    }
    if (off) *off = 64u + 16u + (uint64_t)(8u * m) + (uint64_t)slot * payload;
    if (len) *len = payload;
}

void weft_vk_ring_destroy(weft_vk_ring_t* r) {
    if (!r) return;
    if (!r->borrowed) {
        weft_gpu_destroy(r->g);  // frees only the Vulkan objects (substrate)
    }
    free(r);
}
