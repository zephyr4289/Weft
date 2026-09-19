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
#include <sys/stat.h>
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
    // RFC-0016 §2 wrap state
    int owns_cpu_map;         // 1 = base is OUR mmap (wrap_dmabuf) — destroy unmaps
    size_t map_bytes;         // the owned mapping's extent (owns_cpu_map only)
    char import_kind[20];     // "native" | "host-pointer" | "dmabuf-fd" | ""
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
        // RFC-0016 wrap: the CPU mapping is ours ONLY on the dmabuf road —
        // wrap_host sessions borrow the caller's pointer (never unmapped)
        if (g->owns_cpu_map && g->base) munmap(g->base, g->map_bytes);
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
    snprintf(g->import_kind, sizeof(g->import_kind), "native");
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
    snprintf(g->import_kind, sizeof(g->import_kind), "opaque-fd");
    g->vkDestroyBuffer = vkDestroyBuffer;
    g->vkFreeMemory = vkFreeMemory;
    g->vkDestroyDevice = vkDestroyDevice;
    *out = g;
    return 0;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// RFC-0016 §2: wrap constructors — EXISTING memory becomes GPU-consumable
// ---------------------------------------------------------------------------
// The Series-8 bridge shares sessions Vulkan allocated. This inverts it:
// memory the application already owns — a WFSH shm mapping, a DMA-BUF heap
// allocation — is IMPORTED (VK_EXT_external_memory_host host-pointer road /
// VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf fd road), a
// session-span VkBuffer is bound over it, and the ordinary gpu_stream kit
// consumes the ring through the SAME physical pages the CPU publishes to.
// No staging copy exists anywhere in the path because there is only one
// allocation. Refusals are the honest ladder: extension absent, pointer
// below minImportedHostPointerAlignment (malloc'd rings are NOT importable
// — shm/dmabuf/mmap backing is the documented road), non-dma-buf fd,
// allocation smaller than device requirements, session/geometry mismatch.

#if !defined(_WIN32)
static int gpu_wrap_vulkan(weft_gpu_ring_t** out, size_t payload_bytes,
                           unsigned slot_count, int kind,
                           void* host_ptr, size_t host_map_bytes, int fd) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) return -1;
    const size_t span = GPU_HEADER_BYTES + rb;

    // ---- CPU-side pre-validation: cheap refusals BEFORE any Vulkan ----
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    if (kind == 0) {  // host-pointer road
        if (host_ptr == NULL || host_map_bytes < span) return -1;
        if (session_header_validate((const uint8_t*)host_ptr, span) != 0) return -1;
    } else {          // dmabuf road
        if (fd < 0) return -1;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
            (size_t)st.st_size < span) return -1;
    }

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
    void (*vkGetPhysicalDeviceProperties)(void*, VkPhysicalDeviceProperties_*) =
        (void (*)(void*, VkPhysicalDeviceProperties_*))dlsym(lib, "vkGetPhysicalDeviceProperties");
    void (*vkGetPhysicalDeviceProperties2)(void*, VkPhysicalDeviceProperties2_*) =
        (void (*)(void*, VkPhysicalDeviceProperties2_*))dlsym(lib, "vkGetPhysicalDeviceProperties2");
    vkEnumerateDeviceExtensionProperties_fn vkEnumerateDeviceExtensionProperties =
        (vkEnumerateDeviceExtensionProperties_fn)dlsym(
            lib, "vkEnumerateDeviceExtensionProperties");
    if (!vkCreateInstance || !vkDestroyInstance || !vkEnumeratePhysicalDevices ||
        !vkGetDeviceProcAddr || !vkGetPhysicalDeviceQueueFamilyProperties ||
        !vkGetPhysicalDeviceMemoryProperties || !vkGetPhysicalDeviceProperties ||
        !vkEnumerateDeviceExtensionProperties ||
        (kind == 0 && !vkGetPhysicalDeviceProperties2)) {
        dlclose(lib);
        return -1;
    }

    VkApplicationInfo_ app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "weft-gpu-wrap";
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

    // First physical device with a compute queue family AND the import
    // extension this wrap kind needs (the honest extension gate).
    uint32_t queue_family = 0xFFFFFFFFu;
    void* chosen_phys = NULL;
    VkQueueFamilyProperties_ fam[8];
    VkPhysicalDeviceProperties_ props;
    VkPhysicalDeviceMemoryProperties_ memprops;
    uint64_t min_host_align = 0;
    const char* need_ext = (kind == 0)
        ? VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME
        : VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    int has_host_ext = 0, has_fd_ext = 0, has_dmabuf_ext = 0;
    bool found = false;
    for (uint32_t d = 0; d < ngot && !found; d++) {
        if (!vulkan_has_extension(vkEnumerateDeviceExtensionProperties,
                                  phys[d], need_ext)) continue;
        if (kind == 0) {
            has_host_ext = 1;
        } else {
            has_fd_ext = 1;
            if (!vulkan_has_extension(vkEnumerateDeviceExtensionProperties,
                                      phys[d],
                                      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME))
                continue;  // fd road without dma-buf handles cannot import heaps
            has_dmabuf_ext = 1;
        }
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
        return -1;  // extension or compute queue absent — honest refusal
    }

    // Host road: the alignment contract (malloc'd rings live below it).
    if (kind == 0) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT_ host_props = {0};
        host_props.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2_ props2 = {0};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &host_props;
        vkGetPhysicalDeviceProperties2(chosen_phys, &props2);
        min_host_align = host_props.minImportedHostPointerAlignment;
        if (min_host_align == 0) min_host_align = (uint64_t)page;  // spec floor
        if (((uintptr_t)host_ptr % (uintptr_t)min_host_align) != 0) {
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;  // below minImportedHostPointerAlignment — refused
        }
    }

    // Logical device with the import extensions enabled.
    const char* device_exts[2];
    uint32_t n_device_exts = 0;
    if (kind == 0) {
        device_exts[n_device_exts++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    } else {
        device_exts[n_device_exts++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        device_exts[n_device_exts++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
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
    dci.ppEnabledExtensionNames = device_exts;
    void* device = NULL;
    {
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
        !vkAllocateMemory || !vkFreeMemory || !vkBindBufferMemory || !vkMapMemory ||
        !vkUnmapMemory || !vkDestroyDevice) {
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // The session-span buffer, created with the matching external handle
    // types (the spec-clean import road), same usage ladder as create.
    VkExternalMemoryBufferInfo_ ext_buf = {0};
    ext_buf.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_INFO;
    ext_buf.handleTypes = (kind == 0)
        ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
        : VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo_ bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &ext_buf;
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

    // HOST_VISIBLE | HOST_COHERENT memory type among the requirements bits.
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

    // The backing allocation must COVER the device's requirements. Host
    // road: the caller's mapping extent rounds up to whole pages (what
    // mmap itself guarantees); dmabuf road: the fstat size is exact.
    const size_t host_effective =
        ((host_map_bytes + (size_t)page - 1) / (size_t)page) * (size_t)page;
    if (kind == 0) {
        if (host_effective < (size_t)req.size) {
            vkDestroyBuffer(device, buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;  // mapping smaller than device requirements
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < (size_t)req.size) {
            vkDestroyBuffer(device, buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
    }

    // The import itself. fd road: a DUP is imported (the caller's fd is
    // never consumed; the driver takes ownership of the dup on success).
    VkImportMemoryHostPointerInfoEXT_ host_import = {0};
    host_import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    host_import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    host_import.pHostPointer = host_ptr;

    int import_fd = -1;
    VkImportMemoryFdInfoKHR_ fd_import = {0};
    fd_import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    fd_import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (kind != 0) {
        import_fd = dup(fd);
        if (import_fd < 0) {
            vkDestroyBuffer(device, buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
        fd_import.fd = import_fd;
    }

    VkMemoryAllocateInfo_ mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = (uint64_t)req.size;
    mai.memoryTypeIndex = (uint32_t)memtype;
    mai.pNext = (kind == 0) ? (const void*)&host_import : (const void*)&fd_import;
    void* memory = NULL;
    if (vkAllocateMemory(device, &mai, NULL, &memory) != VK_SUCCESS) {
        if (import_fd >= 0) close(import_fd);  // not consumed on failure
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;  // the ICD refused THIS import (non-dma-buf fd, etc.)
    }
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        // Imported memory is freed like any other; the underlying pages
        // stay alive (they were never ours on the host road).
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // ---- ALIAS VERIFICATION (Law 4: silent degradation is forbidden) ----
    // The imported allocation must carry the CPU's OWN bytes. llvmpipe
    // (Mesa 25.0.7, the CI software ICD) ACCEPTS host imports but backs
    // them with FRESH memory — vkMapMemory returns a different pointer
    // whose contents are zeros, so a wrap that trusted the return code
    // would silently consume an empty ring (the diag evidence in
    // litmus/evidence/heterogeneous/ records this). The canary round-trip:
    // write through the CPU view, read through the imported allocation's
    // mapped view, restore. A device that does not alias is REFUSED — the
    // caller falls back to weft_gpu_create's HOST_VISIBLE road.
    // Scratch word: header offset 28 (creator_pid) — advisory diagnostics
    // in BOTH WFSH dialects, validated by neither (AXIOM T), restored before
    // any exit: the session is left byte-identical.

    // CPU view: the host road borrows the caller's pointer (the GPU and
    // the CPU alias one physical allocation — THE zero-copy claim); the
    // dmabuf road mmaps its own window over the same pages.
    uint8_t* cpu_base = NULL;
    size_t cpu_map_bytes = 0;
    int owns_cpu_map = 0;
    if (kind == 0) {
        cpu_base = (uint8_t*)host_ptr;
        cpu_map_bytes = host_map_bytes;
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            vkFreeMemory(device, memory, NULL);
            vkDestroyBuffer(device, buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
        cpu_map_bytes = (size_t)st.st_size;
        void* p = mmap(NULL, cpu_map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            vkFreeMemory(device, memory, NULL);
            vkDestroyBuffer(device, buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
        cpu_base = (uint8_t*)p;
        owns_cpu_map = 1;
        if (session_header_validate(cpu_base, span) != 0) {
            // not a WFSH session carrying the caller's geometry — refuse
            munmap(cpu_base, cpu_map_bytes);
            vkFreeMemory(device, memory, NULL);
            vkDestroyBuffer(device, buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
    }

    weft_gpu_ring_t* g = calloc(1, sizeof(weft_gpu_ring_t));
    if (!g) {
        if (owns_cpu_map) munmap(cpu_base, cpu_map_bytes);
        vkFreeMemory(device, memory, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return -1;
    }

    // The alias verification itself (see the block comment above). Runs
    // after the CPU view exists (kind 0: the caller's pointer; kind 1: our
    // mmap) and before the session is handed back.
    {
        const uint32_t canary = 0x46435457u;  // "WTCF" LE
        volatile uint32_t* scratch = (volatile uint32_t*)(cpu_base + 28);
        const uint32_t saved = *scratch;
        *scratch = canary;
        void* mapped = NULL;
        int verified = 0;
        if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
            const uint32_t seen = *(volatile const uint32_t*)((uint8_t*)mapped + 28);
            vkUnmapMemory(device, memory);
            verified = (seen == canary);
        }
        *scratch = saved;  // byte-identical session, whatever happened
        if (!verified) {
            // The device did not alias the import (llvmpipe's fresh-memory
            // behavior) or refused the mapping — refuse the wrap, honestly.
            free(g);
            if (owns_cpu_map) munmap(cpu_base, cpu_map_bytes);
            vkFreeMemory(device, memory, NULL);
            vkDestroyBuffer(device, buffer, NULL);
            vkDestroyDevice(device, NULL);
            vkDestroyInstance(instance, NULL);
            dlclose(lib);
            return -1;
        }
    }

    g->backend = WEFT_GPU_BACKEND_VULKAN;
    g->base = cpu_base;
    g->span = span;
    g->payload_bytes = payload_bytes;
    g->slot_count = slot_count;
    snprintf(g->device_name, sizeof(g->device_name), "%s", props.deviceName);
    snprintf(g->import_kind, sizeof(g->import_kind), "%s",
             kind == 0 ? "host-pointer" : "dmabuf-fd");
    snprintf(g->external_info, sizeof(g->external_info), "imported:%s",
             kind == 0 ? "host-pointer" : "dmabuf-fd");
    g->lib = lib;
    g->instance = instance;
    g->physical = chosen_phys;
    g->device = device;
    g->buffer = buffer;
    g->memory = memory;
    g->mapped = NULL;  // the CPU view is the caller's pointer / our mmap
    g->queue_family = queue_family;
    g->exportable = 0;
    g->owns_cpu_map = owns_cpu_map;
    g->map_bytes = cpu_map_bytes;
    g->vkDestroyBuffer = vkDestroyBuffer;
    g->vkFreeMemory = vkFreeMemory;
    g->vkDestroyDevice = vkDestroyDevice;
    (void)has_host_ext; (void)has_fd_ext; (void)has_dmabuf_ext; (void)min_host_align;
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

int weft_gpu_export_dmabuf_fd(weft_gpu_ring_t* g, int* out_fd) {
    if (out_fd == NULL) return -1;
    *out_fd = -1;
    if (g == NULL || g->backend != WEFT_GPU_BACKEND_VULKAN || !g->exportable) {
        return -1;  // not created exportable, or not a Vulkan session
    }
    vkGetMemoryFdKHR_fn vkGetMemoryFdKHR =
        (vkGetMemoryFdKHR_fn)weft_gpu_vk_proc(g, "vkGetMemoryFdKHR");
    if (vkGetMemoryFdKHR == NULL) return -1;
    // The dma-buf handle type (VK_EXT_external_memory_dma_buf): the fd IS a
    // dma-buf — importable by weft_gpu_wrap_dmabuf, weft_dmabuf consumers,
    // V4L2/camera pipes, other processes. Refused when the ICD cannot mint
    // dma-buf handles for this allocation (every refusal honest, Law 4).
    VkMemoryGetFdInfoKHR_ gi = {0};
    gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gi.memory = g->memory;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
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
    if (g == NULL) return "";
    if (g->backend != WEFT_GPU_BACKEND_VULKAN) return "";
    // RFC-0016: wrapped sessions report their import provenance the same
    // way exportable sessions report their handle types (advisory, AXIOM T)
    if (g->import_kind[0] != '\0' && strcmp(g->import_kind, "native") != 0)
        return g->external_info;
    return g->exportable ? g->external_info : "";
}

// ---------------------------------------------------------------------------
// RFC-0016 §2 public surface: wrap constructors + import provenance
// ---------------------------------------------------------------------------

const char* weft_gpu_import_kind(const weft_gpu_ring_t* g) {
    return g ? g->import_kind : "";
}

int weft_gpu_wrap_host(weft_gpu_ring_t** out, size_t payload_bytes,
                       unsigned slot_count, void* session_mem, size_t map_bytes) {
    if (out == NULL || session_mem == NULL) return -1;
    *out = NULL;
#if !defined(_WIN32)
    return gpu_wrap_vulkan(out, payload_bytes, slot_count, 0,
                           session_mem, map_bytes, -1);
#else
    (void)map_bytes;
    return -1;  // Windows Vulkan loader road is the follow-up (declared)
#endif
}

int weft_gpu_wrap_dmabuf(weft_gpu_ring_t** out, size_t payload_bytes,
                         unsigned slot_count, int fd) {
    if (out == NULL || fd < 0) return -1;
    *out = NULL;
#if !defined(_WIN32)
    return gpu_wrap_vulkan(out, payload_bytes, slot_count, 1,
                           NULL, 0, fd);
#else
    return -1;
#endif
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
