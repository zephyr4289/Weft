// gpu_ring.c — GPU-resident zero-copy rings: Vulkan (dlopen) / Metal
// (compile-gated) / CPU fallback. See gpu_ring.h for the contract.
//
// VULKAN ROAD (the zero-copy claim):
//   dlopen("libvulkan.so.1") → vkCreateInstance → first compute-capable
//   physical device → logical device → VkBuffer(STORAGE|TRANSFER_SRC/DST)
//   → memory requirements → HOST_VISIBLE|HOST_COHERENT allocation →
//   vkMapMemory (persistent) → session header + ring zero-init.
//   The CPU publishes through the fan-out API on the mapped pointer; the
//   GPU binds the SAME VkBuffer as a storage buffer and dereferences the
//   live words (the probe's shader does exactly this). No staging buffer,
//   no copy command, one allocation.
//
//   Loader discipline: every device-level entry point is resolved through
//   vkGetDeviceProcAddr off the ONE instance-level pair kept in the ring
//   struct. dlopen'd with RTLD_NOW|RTLD_LOCAL; dlclosed on destroy. A
//   missing ICD/library degrades to the CPU fallback (reported), never
//   crashes — the probe's gate decides what "no GPU" means for evidence.
//
// METAL ROAD (compile-gated __APPLE__): unified memory — the CPU mapping
//   and the GPU view are the same physical RAM by construction
//   (MTLResourceStorageModeShared is the default on Apple silicon). This
//   build allocates the session via mmap and tags it METAL; the shading
//   language binding story (MSL buffer argument) is the RFC's open
//   question and is NOT claimed here — compiled by the apple CI leg, NOT
//   executable-tested on the x86_64 sandbox, declared.
//
// CPU ROAD: anonymous MAP_SHARED, byte-identical session. Full API parity
//   so tests and GPU-less CI still exercise the protocol.

#include "gpu_ring.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Session header helpers (same protocol as shm_ring.c — kept local so the
// modules stay independent; the layout is the contract, verified by tests)
// ---------------------------------------------------------------------------

#define GPU_HEADER_BYTES 64u
#define GPU_MAGIC 0x48534657u /* "WFSH" LE */

static void gput_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t gget_u16le(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static void gput_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t gget_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void gput_u64le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t gget_u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int session_header_write(uint8_t* base, size_t payload_bytes,
                                unsigned slot_count) {
    memset(base, 0, GPU_HEADER_BYTES);
    gput_u32le(base + 0, GPU_MAGIC);
    gput_u16le(base + 4, 1);                 // version
    gput_u16le(base + 6, (uint16_t)GPU_HEADER_BYTES);
    gput_u32le(base + 8, 0);                 // flags
    gput_u32le(base + 12, (uint32_t)payload_bytes);
    gput_u32le(base + 16, (uint32_t)slot_count);
    gput_u64le(base + 20, (uint64_t)weft_fanout_ring_bytes(payload_bytes, slot_count));
    gput_u32le(base + 28, (uint32_t)getpid());
    // created_unix_ns left 0 — advisory; GPU rings need no wall clock
    return 0;
}

/// Validate a session header span (attach-by-bytes contract).
static int session_header_validate(const uint8_t* base, size_t span) {
    if (span < GPU_HEADER_BYTES) return -1;
    if (gget_u32le(base + 0) != GPU_MAGIC) return -1;
    if (gget_u16le(base + 4) != 1) return -1;
    if (gget_u16le(base + 6) != GPU_HEADER_BYTES) return -1;
    if (gget_u32le(base + 8) != 0) return -1;
    const uint32_t pb = gget_u32le(base + 12);
    const uint32_t sc = gget_u32le(base + 16);
    const uint64_t rb = gget_u64le(base + 20);
    if (rb != (uint64_t)weft_fanout_ring_bytes(pb, sc)) return -1;
    if (span != (size_t)GPU_HEADER_BYTES + (size_t)rb) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// The ring struct — public side
// ---------------------------------------------------------------------------

struct weft_gpu_ring {
    weft_gpu_backend_t backend;
    uint8_t* base;            // session start (mapped)
    size_t span;              // header + ring bytes
    size_t payload_bytes;
    unsigned slot_count;
    char device_name[256];

    // Vulkan state (null on other backends)
    void* lib;                // dlopen handle
    void* instance;           // VkInstance
    void* physical;           // VkPhysicalDevice the device was built from (Series 8)
    void* device;             // VkDevice
    void* buffer;             // VkBuffer (the session span)
    void* memory;             // VkDeviceMemory (HOST_VISIBLE|HOST_COHERENT)
    void* mapped;             // vkMapMemory result == base
    uint32_t queue_family;
    int exportable;           // allocation created with VkExportMemoryAllocateInfo
    char external_info[64];   // advisory handle-type report (AXIOM T)
    // Resolved device-level entry points (kept as void*; typed at call)
    void (*vkDestroyBuffer)(void*, void*, const void*);
    void (*vkFreeMemory)(void*, void*, const void*);
    void (*vkDestroyDevice)(void*, const void*);
};

weft_gpu_backend_t weft_gpu_backend(const weft_gpu_ring_t* g) {
    return g ? g->backend : WEFT_GPU_BACKEND_CPU;
}

const char* weft_gpu_backend_name(const weft_gpu_ring_t* g) {
    switch (weft_gpu_backend(g)) {
        case WEFT_GPU_BACKEND_VULKAN: return "vulkan";
        case WEFT_GPU_BACKEND_METAL: return "metal";
        default: return "cpu";
    }
}

const char* weft_gpu_device_name(const weft_gpu_ring_t* g) {
    return g ? g->device_name : "";
}

uint8_t* weft_gpu_ring_bytes(weft_gpu_ring_t* g) {
    return g ? g->base + GPU_HEADER_BYTES : NULL;
}

size_t weft_gpu_ring_span(weft_gpu_ring_t* g) {
    return g ? g->span : 0;
}

size_t weft_gpu_payload_bytes(const weft_gpu_ring_t* g) {
    return g ? g->payload_bytes : 0;
}

unsigned weft_gpu_slot_count(const weft_gpu_ring_t* g) {
    return g ? g->slot_count : 0;
}

void weft_gpu_destroy(weft_gpu_ring_t* g) {
    if (g == NULL) return;
#if !defined(_WIN32)
    if (g->backend == WEFT_GPU_BACKEND_VULKAN && g->device) {
        if (g->buffer && g->vkDestroyBuffer) g->vkDestroyBuffer(g->device, g->buffer, NULL);
        if (g->memory && g->vkFreeMemory) g->vkFreeMemory(g->device, g->memory, NULL);
        if (g->vkDestroyDevice) g->vkDestroyDevice(g->device, NULL);
        // instance destroyed via the loader's instance-level fn
        if (g->instance) {
            void (*vkDestroyInstance)(void*, const void*) =
                (void (*)(void*, const void*))dlsym(g->lib, "vkDestroyInstance");
            if (vkDestroyInstance) vkDestroyInstance(g->instance, NULL);
        }
        if (g->lib) dlclose(g->lib);
        free(g);
        return;
    }
    munmap(g->base, g->span);
    free(g);
#endif
}

// ---------------------------------------------------------------------------
// CPU / Metal fallback: anonymous MAP_SHARED session
// ---------------------------------------------------------------------------

#if !defined(_WIN32)
static int gpu_create_cpu(weft_gpu_ring_t** out, size_t payload_bytes,
                          unsigned slot_count, weft_gpu_backend_t tag) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) return -1;
    const size_t span = GPU_HEADER_BYTES + rb;
    void* p = mmap(NULL, span, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    weft_gpu_ring_t* g = calloc(1, sizeof(weft_gpu_ring_t));
    if (!g) {
        munmap(p, span);
        return -1;
    }
    memset(p, 0, span);
    session_header_write((uint8_t*)p, payload_bytes, slot_count);
    g->backend = tag;
    g->base = (uint8_t*)p;
    g->span = span;
    g->payload_bytes = payload_bytes;
    g->slot_count = slot_count;
    snprintf(g->device_name, sizeof(g->device_name),
             tag == WEFT_GPU_BACKEND_METAL
                 ? "apple unified memory (compile-gated; MSL binding is RFC-0003 open question)"
                 : "anonymous mapping (no GPU backend available)");
    *out = g;
    return 0;
}
#endif

// ---------------------------------------------------------------------------
// Vulkan road — dlopen'd loader, shared minimal ABI (vk_min.h)
// ---------------------------------------------------------------------------

#include "vk_min.h"

// Instance-level entry points the Series-8 additions need, resolved once
// per ring off the loader DSO (the gpu_ring loader discipline).
typedef void* (*vkGetInstanceProcAddr_fn)(void*, const char*);
typedef int (*vkEnumerateDeviceExtensionProperties_fn)(void*, const char*,
                                                        uint32_t*, VkExtensionProperties_*);
typedef int (*vkGetMemoryFdKHR_fn)(void*, const VkMemoryGetFdInfoKHR_*, int*);

/// Does the ring's physical device offer `ext_name`? (create-path only)
static int vulkan_has_extension(vkEnumerateDeviceExtensionProperties_fn enumerate,
                                void* phys, const char* ext_name) {
    if (enumerate == NULL || phys == NULL) return 0;
    uint32_t count = 0;
    if (enumerate(phys, NULL, &count, NULL) != VK_SUCCESS || count == 0) return 0;
    if (count > 256) count = 256;
    VkExtensionProperties_ props[256];
    if (enumerate(phys, NULL, &count, props) != VK_SUCCESS) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (strncmp(props[i].extensionName, ext_name,
                    VK_MAX_EXTENSION_NAME_SIZE) == 0) {
            return 1;
        }
    }
    return 0;
}

// The instance-level entry points resolve from the loader DSO; device-level
// entry points via vkGetDeviceProcAddr (kept in the ring struct for destroy).
typedef int (*vkCreateInstance_fn)(const VkInstanceCreateInfo_*, const void*, void**);
typedef void (*vkDestroyInstance_fn)(void*, const void*);
typedef int (*vkEnumeratePhysicalDevices_fn)(void*, uint32_t*, void*);
typedef int (*vkCreateDevice_fn)(void*, const VkDeviceCreateInfo_*, const void*, void**);
typedef void* (*vkGetDeviceProcAddr_fn)(void*, const char*);
typedef void (*vkGetPhysicalDeviceQueueFamilyProperties_fn)(void*, uint32_t*, VkQueueFamilyProperties_*);
typedef void (*vkGetPhysicalDeviceMemoryProperties_fn)(void*, VkPhysicalDeviceMemoryProperties_*);
typedef void (*vkGetPhysicalDeviceProperties_fn)(void*, VkPhysicalDeviceProperties_);

#if !defined(_WIN32)
static int gpu_create_vulkan(weft_gpu_ring_t** out, size_t payload_bytes,
                             unsigned slot_count, uint32_t flags) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) return -1;
    const size_t span = GPU_HEADER_BYTES + rb;
    int want_export = (flags & 0x1u) != 0u;  // may be cleared (extension absent)

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (lib == NULL) return -1;  // no loader -> caller falls back

    // Instance-level entry points (exported by the loader itself).
    int (*vkCreateInstance)(const VkInstanceCreateInfo_*, const void*, void**) =
        (int (*)(const VkInstanceCreateInfo_*, const void*, void**))dlsym(lib, "vkCreateInstance");
    void (*vkDestroyInstance)(void*, const void*) =
        (void (*)(void*, const void*))dlsym(lib, "vkDestroyInstance");
    int (*vkEnumeratePhysicalDevices)(void*, uint32_t*, void*) =
        (int (*)(void*, uint32_t*, void*))dlsym(lib, "vkEnumeratePhysicalDevices");
    void* (*vkGetDeviceProcAddr)(void*, const char*) =
        (void* (*)(void*, const char*))dlsym(lib, "vkGetDeviceProcAddr");
    void (*vkGetPhysicalDeviceQueueFamilyProperties)(void*, uint32_t*, VkQueueFamilyProperties_*) =
        (void (*)(void*, uint32_t*, VkQueueFamilyProperties_*))dlsym(lib, "vkGetPhysicalDeviceQueueFamilyProperties");
    void (*vkGetPhysicalDeviceMemoryProperties)(void*, VkPhysicalDeviceMemoryProperties_*) =
        (void (*)(void*, VkPhysicalDeviceMemoryProperties_*))dlsym(lib, "vkGetPhysicalDeviceMemoryProperties");
    void (*vkGetPhysicalDeviceProperties)(void*, VkPhysicalDeviceProperties_*) =
        (void (*)(void*, VkPhysicalDeviceProperties_*))dlsym(lib, "vkGetPhysicalDeviceProperties");
    if (!vkCreateInstance || !vkDestroyInstance || !vkEnumeratePhysicalDevices ||
        !vkGetDeviceProcAddr || !vkGetPhysicalDeviceQueueFamilyProperties ||
        !vkGetPhysicalDeviceMemoryProperties || !vkGetPhysicalDeviceProperties) {
        dlclose(lib);
        return -1;
    }

    VkApplicationInfo_ app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "weft-gpu-ring";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo_ ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    void* instance = NULL;
    if (vkCreateInstance(&ci, NULL, &instance) != VK_SUCCESS) {
        dlclose(lib);
        return -1;
    }

    uint32_t nphys = 0;
    if (vkEnumeratePhysicalDevices(instance, &nphys, NULL) != VK_SUCCESS || nphys == 0) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }
    void* phys[4] = {0};
    uint32_t ngot = nphys > 4 ? 4 : nphys;
    if (vkEnumeratePhysicalDevices(instance, &ngot, phys) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // First physical device with a compute-capable queue family.
    uint32_t queue_family = 0xFFFFFFFFu;
    void* chosen_phys = NULL;  // the device the search picked (Series 8)
    VkQueueFamilyProperties_ fam[8];
    VkPhysicalDeviceProperties_ props;
    VkPhysicalDeviceMemoryProperties_ memprops;
    bool found = false;
    for (uint32_t d = 0; d < ngot && !found; d++) {
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nfam, NULL);
        if (nfam == 0 || nfam > 8) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nfam, fam);
        for (uint32_t f = 0; f < nfam; f++) {
            if (fam[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queue_family = f;
                chosen_phys = phys[d];
                vkGetPhysicalDeviceProperties(phys[d], &props);
                vkGetPhysicalDeviceMemoryProperties(phys[d], &memprops);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // Logical device with one compute queue. The Series-8 fd bridge needs
    // VK_KHR_external_memory_fd ENABLED on the device when exporting; the
    // extension probe result also feeds the advisory external_info string.
    vkEnumerateDeviceExtensionProperties_fn vkEnumerateDeviceExtensionProperties =
        (vkEnumerateDeviceExtensionProperties_fn)dlsym(
            lib, "vkEnumerateDeviceExtensionProperties");
    int has_fd_ext = 0, has_dmabuf_ext = 0;
    if (want_export && vkEnumerateDeviceExtensionProperties != NULL) {
        has_fd_ext = vulkan_has_extension(vkEnumerateDeviceExtensionProperties,
                                          chosen_phys,
                                          VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        has_dmabuf_ext = vulkan_has_extension(vkEnumerateDeviceExtensionProperties,
                                              chosen_phys,
                                              VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        if (!has_fd_ext) want_export = 0;  // transparent: plain session, -1 later
    }
    const char* device_exts[2] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    uint32_t n_device_exts = 0;
    if (has_fd_ext) n_device_exts = 1;
    if (has_dmabuf_ext) n_device_exts = 2;

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
    dci.enabledExtensionCount = n_device_exts;
    dci.ppEnabledExtensionNames = (n_device_exts > 0) ? device_exts : NULL;
    void* device = NULL;
    {
        // vkCreateDevice is an instance-level fn the loader exports too.
        // AUDIT NOTE (Series 8): the pre-Series-8 code created the device
        // from phys[0] regardless of which physical device the family search
        // selected — silently wrong on multi-ICD hosts. Use the chosen one.
        int (*vkCreateDevice)(void*, const VkDeviceCreateInfo_*, const void*, void**) =
            (int (*)(void*, const VkDeviceCreateInfo_*, const void*, void**))dlsym(lib, "vkCreateDevice");
        if (vkCreateDevice == NULL ||
            vkCreateDevice(chosen_phys, &dci, NULL, &device) != VK_SUCCESS) {
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
    }

    // Device-level entry points.
    int (*vkCreateBuffer)(void*, const VkBufferCreateInfo_*, const void*, void**) =
        (int (*)(void*, const VkBufferCreateInfo_*, const void*, void**))vkGetDeviceProcAddr(device, "vkCreateBuffer");
    void (*vkDestroyBuffer)(void*, void*, const void*) =
        (void (*)(void*, void*, const void*))vkGetDeviceProcAddr(device, "vkDestroyBuffer");
    void (*vkGetBufferMemoryRequirements)(void*, void*, VkMemoryRequirements_*) =
        (void (*)(void*, void*, VkMemoryRequirements_*))vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements");
    int (*vkAllocateMemory)(void*, const VkMemoryAllocateInfo_*, const void*, void**) =
        (int (*)(void*, const VkMemoryAllocateInfo_*, const void*, void**))vkGetDeviceProcAddr(device, "vkAllocateMemory");
    void (*vkFreeMemory)(void*, void*, const void*) =
        (void (*)(void*, void*, const void*))vkGetDeviceProcAddr(device, "vkFreeMemory");
    int (*vkBindBufferMemory)(void*, void*, void*, uint64_t) =
        (int (*)(void*, void*, void*, uint64_t))vkGetDeviceProcAddr(device, "vkBindBufferMemory");
    int (*vkMapMemory)(void*, void*, uint64_t, uint64_t, VkFlags_, void**) =
        (int (*)(void*, void*, uint64_t, uint64_t, VkFlags_, void**))vkGetDeviceProcAddr(device, "vkMapMemory");
    void (*vkUnmapMemory)(void*, void*) =
        (void (*)(void*, void*))vkGetDeviceProcAddr(device, "vkUnmapMemory");
    void (*vkDestroyDevice)(void*, const void*) =
        (void (*)(void*, const void*))vkGetDeviceProcAddr(device, "vkDestroyDevice");
    if (!vkCreateBuffer || !vkDestroyBuffer || !vkGetBufferMemoryRequirements ||
        !vkAllocateMemory || !vkFreeMemory || !vkBindBufferMemory ||
        !vkMapMemory || !vkUnmapMemory || !vkDestroyDevice) {
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // The session-span storage buffer — the ring the GPU will bind. Series 8
    // adds the texel-buffer usage bits (the R32_UINT "direct texture" road —
    // binding 3 of the gpu_stream kit): STORAGE first, and a plain
    // STORAGE|TRANSFER retry when the ICD refuses texel usage on buffers
    // (declared fallback, never silent).
    VkBufferCreateInfo_ bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = span;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    void* buffer = NULL;
    if (vkCreateBuffer(device, &bci, NULL, &buffer) != VK_SUCCESS) {
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device, &bci, NULL, &buffer) != VK_SUCCESS) {
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
    }

    // HOST_VISIBLE | HOST_COHERENT memory type.
    VkMemoryRequirements_ req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    int32_t memtype = -1;
    for (uint32_t m = 0; m < memprops.memoryTypeCount; m++) {
        if (!(req.memoryTypeBits & (1u << m))) continue;
        const VkFlags_ want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((memprops.memoryTypes[m].propertyFlags & want) == want) {
            memtype = (int32_t)m;
            break;
        }
    }
    if (memtype < 0) {
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // Exportable allocations chain VkExportMemoryAllocateInfo (Series 8): the
    // OPAQUE_FD handle type is the port cross-process contract; dma-buf joins
    // when the EXT is present. The chain lives in Mai's pNext — one struct,
    // created only when exporting.
    VkExportMemoryAllocateInfo_ export_info = {0};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                              (has_dmabuf_ext
                                   ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                                   : 0u);
    VkMemoryAllocateInfo_ mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)memtype;
    if (want_export) {
        mai.pNext = &export_info;
    }
    void* memory = NULL;
    if (vkAllocateMemory(device, &mai, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }
    void* mapped = NULL;
    if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void**)&mapped) != VK_SUCCESS) {
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    memset(mapped, 0, span);
    session_header_write((uint8_t*)mapped, payload_bytes, slot_count);
    if (session_header_validate((const uint8_t*)mapped, span) != 0) {
        vkUnmapMemory(device, memory);
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    weft_gpu_ring_t* g = calloc(1, sizeof(weft_gpu_ring_t));
    if (!g) {
        vkUnmapMemory(device, memory);
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }
    g->backend = WEFT_GPU_BACKEND_VULKAN;
    g->base = (uint8_t*)mapped;
    g->span = span;
    g->payload_bytes = payload_bytes;
    g->slot_count = slot_count;
    snprintf(g->device_name, sizeof(g->device_name), "%s", props.deviceName);
    g->lib = lib;
    g->instance = instance;
    g->physical = chosen_phys;
    g->device = device;
    g->buffer = buffer;
    g->memory = memory;
    g->mapped = mapped;
    g->queue_family = queue_family;
    g->exportable = want_export;  // effective (extension-probed) state
    if (want_export) {
        snprintf(g->external_info, sizeof(g->external_info), "%s",
                 has_dmabuf_ext ? "opaque-fd+dmabuf" : "opaque-fd");
    }
    g->vkDestroyBuffer = vkDestroyBuffer;
    g->vkFreeMemory = vkFreeMemory;
    g->vkDestroyDevice = vkDestroyDevice;
    (void)vkUnmapMemory;
    *out = g;
    return 0;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Series 8: the import ladder — the fd bridge's far side (RFC-0013)
// ---------------------------------------------------------------------------
// Mirrors gpu_create_vulkan's ladder; the ONE difference is the allocation:
// VkImportMemoryFdInfoKHR chained into VkMemoryAllocateInfo imports the
// exporter's pages (the fd is consumed on success). The session header is
// validated against the caller's geometry BEFORE the ring is handed back —
// an fd carrying a different session is a hard error, not a fallback.

#if !defined(_WIN32)
static int gpu_import_vulkan_fd(weft_gpu_ring_t** out, size_t payload_bytes,
                                unsigned slot_count, int fd) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) return -1;
    const size_t span = GPU_HEADER_BYTES + rb;

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) return -1;

    int (*vkCreateInstance)(const VkInstanceCreateInfo_*, const void*, void**) =
        (int (*)(const VkInstanceCreateInfo_*, const void*, void**))dlsym(lib, "vkCreateInstance");
    void (*vkDestroyInstance)(void*, const void*) =
        (void (*)(void*, const void*))dlsym(lib, "vkDestroyInstance");
    int (*vkEnumeratePhysicalDevices)(void*, uint32_t*, void*) =
        (int (*)(void*, uint32_t*, void*))dlsym(lib, "vkEnumeratePhysicalDevices");
    void* (*vkGetDeviceProcAddr)(void*, const char*) =
        (void* (*)(void*, const char*))dlsym(lib, "vkGetDeviceProcAddr");
    void (*vkGetPhysicalDeviceQueueFamilyProperties)(void*, uint32_t*, VkQueueFamilyProperties_*) =
        (void (*)(void*, uint32_t*, VkQueueFamilyProperties_*))dlsym(lib, "vkGetPhysicalDeviceQueueFamilyProperties");
    void (*vkGetPhysicalDeviceMemoryProperties)(void*, VkPhysicalDeviceMemoryProperties_*) =
        (void (*)(void*, VkPhysicalDeviceMemoryProperties_*))dlsym(lib, "vkGetPhysicalDeviceMemoryProperties");
    vkEnumerateDeviceExtensionProperties_fn vkEnumerateDeviceExtensionProperties =
        (vkEnumerateDeviceExtensionProperties_fn)dlsym(
            lib, "vkEnumerateDeviceExtensionProperties");
    if (!vkCreateInstance || !vkDestroyInstance || !vkEnumeratePhysicalDevices ||
        !vkGetDeviceProcAddr || !vkGetPhysicalDeviceQueueFamilyProperties ||
        !vkGetPhysicalDeviceMemoryProperties || !vkEnumerateDeviceExtensionProperties) {
        dlclose(lib);
        return -1;
    }

    VkApplicationInfo_ app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "weft-gpu-import";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo_ ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    void* instance = NULL;
    if (vkCreateInstance(&ci, NULL, &instance) != VK_SUCCESS) {
        dlclose(lib);
        return -1;
    }

    uint32_t nphys = 0;
    if (vkEnumeratePhysicalDevices(instance, &nphys, NULL) != VK_SUCCESS || nphys == 0) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }
    void* phys[4] = {0};
    uint32_t ngot = nphys > 4 ? 4 : nphys;
    if (vkEnumeratePhysicalDevices(instance, &ngot, phys) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    uint32_t queue_family = 0xFFFFFFFFu;
    void* chosen_phys = NULL;
    VkQueueFamilyProperties_ fam[8];
    VkPhysicalDeviceMemoryProperties_ memprops;
    bool found = false;
    for (uint32_t d = 0; d < ngot && !found; d++) {
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nfam, NULL);
        if (nfam == 0 || nfam > 8) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[d], &nfam, fam);
        for (uint32_t f = 0; f < nfam; f++) {
            if (fam[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queue_family = f;
                chosen_phys = phys[d];
                vkGetPhysicalDeviceMemoryProperties(phys[d], &memprops);
                found = true;
                break;
            }
        }
    }
    if (!found || !vulkan_has_extension(vkEnumerateDeviceExtensionProperties,
                                        chosen_phys,
                                        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo_ qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* fd_exts[1] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME };
    VkDeviceCreateInfo_ dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = fd_exts;
    void* device = NULL;
    int (*vkCreateDevice)(void*, const VkDeviceCreateInfo_*, const void*, void**) =
        (int (*)(void*, const VkDeviceCreateInfo_*, const void*, void**))dlsym(lib, "vkCreateDevice");
    if (vkCreateDevice == NULL ||
        vkCreateDevice(chosen_phys, &dci, NULL, &device) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    int (*vkCreateBuffer)(void*, const VkBufferCreateInfo_*, const void*, void**) =
        (int (*)(void*, const VkBufferCreateInfo_*, const void*, void**))vkGetDeviceProcAddr(device, "vkCreateBuffer");
    void (*vkDestroyBuffer)(void*, void*, const void*) =
        (void (*)(void*, void*, const void*))vkGetDeviceProcAddr(device, "vkDestroyBuffer");
    void (*vkGetBufferMemoryRequirements)(void*, void*, VkMemoryRequirements_*) =
        (void (*)(void*, void*, VkMemoryRequirements_*))vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements");
    int (*vkAllocateMemory)(void*, const VkMemoryAllocateInfo_*, const void*, void**) =
        (int (*)(void*, const VkMemoryAllocateInfo_*, const void*, void**))vkGetDeviceProcAddr(device, "vkAllocateMemory");
    void (*vkFreeMemory)(void*, void*, const void*) =
        (void (*)(void*, void*, const void*))vkGetDeviceProcAddr(device, "vkFreeMemory");
    int (*vkBindBufferMemory)(void*, void*, void*, uint64_t) =
        (int (*)(void*, void*, void*, uint64_t))vkGetDeviceProcAddr(device, "vkBindBufferMemory");
    int (*vkMapMemory)(void*, void*, uint64_t, uint64_t, VkFlags_, void**) =
        (int (*)(void*, void*, uint64_t, uint64_t, VkFlags_, void**))vkGetDeviceProcAddr(device, "vkMapMemory");
    void (*vkDestroyDevice)(void*, const void*) =
        (void (*)(void*, const void*))vkGetDeviceProcAddr(device, "vkDestroyDevice");
    if (!vkCreateBuffer || !vkDestroyBuffer || !vkGetBufferMemoryRequirements ||
        !vkAllocateMemory || !vkFreeMemory || !vkBindBufferMemory ||
        !vkMapMemory || !vkDestroyDevice) {
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    VkBufferCreateInfo_ bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = span;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    void* buffer = NULL;
    if (vkCreateBuffer(device, &bci, NULL, &buffer) != VK_SUCCESS) {
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    VkMemoryRequirements_ req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    int32_t memtype = -1;
    for (uint32_t m = 0; m < memprops.memoryTypeCount; m++) {
        if (!(req.memoryTypeBits & (1u << m))) continue;
        const VkFlags_ want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((memprops.memoryTypes[m].propertyFlags & want) == want) {
            memtype = (int32_t)m;
            break;
        }
    }
    if (memtype < 0) {
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // THE import: the fd's pages become this allocation (consumed on success).
    VkImportMemoryFdInfoKHR_ import_info = {0};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = fd;
    VkMemoryAllocateInfo_ mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &import_info;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)memtype;
    void* memory = NULL;
    if (vkAllocateMemory(device, &mai, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;  // fd NOT consumed on failure — caller keeps ownership
    }
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }
    void* mapped = NULL;
    if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void**)&mapped) != VK_SUCCESS) {
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // The imported session MUST carry the caller's exact geometry.
    if (session_header_validate((const uint8_t*)mapped, span) != 0) {
        // vkFreeMemory implicitly unmaps — no unmap call needed (legal Vulkan).
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    weft_gpu_ring_t* g = calloc(1, sizeof(weft_gpu_ring_t));
    if (!g) {
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }
    g->backend = WEFT_GPU_BACKEND_VULKAN;
    g->base = (uint8_t*)mapped;
    g->span = span;
    g->payload_bytes = payload_bytes;
    g->slot_count = slot_count;
    snprintf(g->device_name, sizeof(g->device_name), "imported fd session");
    snprintf(g->external_info, sizeof(g->external_info), "imported");
    g->lib = lib;
    g->instance = instance;
    g->physical = chosen_phys;
    g->device = device;
    g->buffer = buffer;
    g->memory = memory;
    g->mapped = mapped;
    g->queue_family = queue_family;
    g->exportable = 0;
    g->vkDestroyBuffer = vkDestroyBuffer;
    g->vkFreeMemory = vkFreeMemory;
    g->vkDestroyDevice = vkDestroyDevice;
    *out = g;
    return 0;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Public create: probe order vulkan -> metal -> cpu
// ---------------------------------------------------------------------------

int weft_gpu_create(weft_gpu_ring_t** out, size_t payload_bytes, unsigned slot_count) {
    return weft_gpu_create_ex(out, payload_bytes, slot_count, 0u);
}

int weft_gpu_create_ex(weft_gpu_ring_t** out, size_t payload_bytes,
                       unsigned slot_count, uint32_t flags) {
    if (out == NULL || weft_fanout_ring_bytes(payload_bytes, slot_count) == 0) {
        return -1;
    }
    *out = NULL;
#if !defined(_WIN32)
    if (gpu_create_vulkan(out, payload_bytes, slot_count, flags) == 0) {
        return 0;
    }
    #if defined(__APPLE__)
    if (gpu_create_cpu(out, payload_bytes, slot_count, WEFT_GPU_BACKEND_METAL) == 0) {
        return 0;
    }
    #endif
    return gpu_create_cpu(out, payload_bytes, slot_count, WEFT_GPU_BACKEND_CPU);
#else
    // Windows: Vulkan loader via LoadLibrary is the follow-up (windows CI
    // leg); the CPU road keeps the module usable meanwhile — declared.
    (void)flags;
    return -1;
#endif
}

// ---------------------------------------------------------------------------
// Series 8: the fd bridge (RFC-0013) — export / import / advisory
// ---------------------------------------------------------------------------

int weft_gpu_export_fd(weft_gpu_ring_t* g, int* out_fd) {
    if (out_fd == NULL) return -1;
    *out_fd = -1;
    if (g == NULL || g->backend != WEFT_GPU_BACKEND_VULKAN || !g->exportable) {
        return -1;  // WFSH shm (RFC-0011) is the documented fallback
    }
    // vkGetMemoryFdKHR is a device-level entry point of the enabled
    // VK_KHR_external_memory_fd extension.
    vkGetMemoryFdKHR_fn vkGetMemoryFdKHR =
        (vkGetMemoryFdKHR_fn)weft_gpu_vk_proc(g, "vkGetMemoryFdKHR");
    if (vkGetMemoryFdKHR == NULL) return -1;
    VkMemoryGetFdInfoKHR_ gi = {0};
    gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gi.memory = g->memory;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (vkGetMemoryFdKHR(g->device, &gi, &fd) != VK_SUCCESS || fd < 0) {
        return -1;
    }
    *out_fd = fd;  // caller owns it now (close(2) when done)
    return 0;
}

int weft_gpu_import_fd(weft_gpu_ring_t** out, size_t payload_bytes,
                       unsigned slot_count, int fd) {
    *out = NULL;
    if (fd < 0 || weft_fanout_ring_bytes(payload_bytes, slot_count) == 0) {
        return -1;
    }
#if !defined(_WIN32)
    // Mirror gpu_create_vulkan's ladder, but the allocation IMPORTS the fd
    // (VkImportMemoryFdInfoKHR chained into VkMemoryAllocateInfo). The fd is
    // consumed by the driver on success (do not close it again).
    // NOTE (honesty boundary): the full ladder is exercised by the CI
    // gpu-native shard (lavapipe supports VK_KHR_external_memory_fd); the
    // x86_64 sandbox executes the same code through SwiftShader when its
    // ICD offers the extension, and the WFSH fallback covers the rest.
    return gpu_import_vulkan_fd(out, payload_bytes, slot_count, fd);
#else
    (void)out;
    return -1;
#endif
}

const char* weft_gpu_external_info(const weft_gpu_ring_t* g) {
    return (g && g->backend == WEFT_GPU_BACKEND_VULKAN && g->exportable)
               ? g->external_info
               : "";
}

const void* weft_gpu_vk_buffer(const weft_gpu_ring_t* g) {
    return (g && g->backend == WEFT_GPU_BACKEND_VULKAN) ? g->buffer : NULL;
}

size_t weft_gpu_vk_buffer_bytes(const weft_gpu_ring_t* g) {
    return (g && g->backend == WEFT_GPU_BACKEND_VULKAN) ? g->span : 0;
}

const void* weft_gpu_vk_device(const weft_gpu_ring_t* g) {
    return (g && g->backend == WEFT_GPU_BACKEND_VULKAN) ? g->device : NULL;
}

uint32_t weft_gpu_vk_queue_family(const weft_gpu_ring_t* g) {
    return (g && g->backend == WEFT_GPU_BACKEND_VULKAN) ? g->queue_family : 0xFFFFFFFFu;
}

const void* weft_gpu_vk_instance(const weft_gpu_ring_t* g) {
    return (g && g->backend == WEFT_GPU_BACKEND_VULKAN) ? g->instance : NULL;
}

const void* weft_gpu_vk_physical_device(const weft_gpu_ring_t* g) {
    return (g && g->backend == WEFT_GPU_BACKEND_VULKAN) ? g->physical : NULL;
}

const void* weft_gpu_vk_instance_proc(const weft_gpu_ring_t* g, const char* name) {
    if (g == NULL || g->backend != WEFT_GPU_BACKEND_VULKAN || name == NULL) {
        return NULL;
    }
    vkGetInstanceProcAddr_fn vkGetInstanceProcAddr =
        (vkGetInstanceProcAddr_fn)dlsym(g->lib, "vkGetInstanceProcAddr");
    if (vkGetInstanceProcAddr == NULL || g->instance == NULL) {
        return NULL;
    }
    return vkGetInstanceProcAddr(g->instance, name);
}

const void* weft_gpu_vk_proc(const weft_gpu_ring_t* g, const char* name) {
    if (g == NULL || g->backend != WEFT_GPU_BACKEND_VULKAN || name == NULL) {
        return NULL;
    }
    void* (*vkGetDeviceProcAddr)(void*, const char*) =
        (void* (*)(void*, const char*))dlsym(g->lib, "vkGetDeviceProcAddr");
    if (vkGetDeviceProcAddr == NULL) {
        return NULL;
    }
    return vkGetDeviceProcAddr(g->device, name);
}
