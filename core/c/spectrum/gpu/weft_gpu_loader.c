// weft_gpu_loader.c — Vulkan 1.3 compute discovery (Pillar 5, D-52).
//
// WHY EXISTS: the spectrum pipeline's GPU rows (Adreno, Mali Immortalis,
// PC discrete) speak Vulkan 1.3 with TIMELINE SEMAPHORE sync — the
// cross-vendor zero-copy compute path that needs no vendor SDK. This
// loader answers ONE question honestly, with zero dependency footprint:
//
//     Is there a Vulkan 1.3 physical device on THIS host whose queue
//     families support compute, and does it expose timelineSemaphore?
//
// It dlopens libvulkan.so.1, resolves vkGetInstanceProcAddr, enumerates
// physical devices, checks apiVersion >= VK_API_VERSION_1_3 and the
// device's timelineSemaphore feature bit, then destroys everything it
// created — no instance leaks, no allocations outside the probe (which
// runs at DRIVER INIT time, never on the hot path).
//
// HONESTY BOUNDARY: the sandbox CI has no Vulkan ICD — the found-path is
// compile-verified and exercised on GPU-equipped runners; on headless
// hosts the loader reports 0 and drivers route to the next engine
// (deterministic degradation, Law 3). The hand-declared VK structs below
// mirror the frozen public Vulkan 1.3 layouts EXACTLY (full sizes; the
// properties struct uses an opaque 1024B buffer because only its first
// fields are read and the true size is 812B).
//
// LAWS: probe-time only (init); zero heap on any hot path; fail-closed
// (any Vulkan error -> "not available", never a crash).

#include "weft_gpu_loader.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Minimal hand-declared Vulkan 1.3 ABI subset (stable public constants)
// ---------------------------------------------------------------------------

#define WEFT_VK_SUCCESS                          0
#define WEFT_VK_API_VERSION_1_3                 0x00403000u   // variant0|major1|minor3|patch0
#define WEFT_VK_MAX_PHYSICAL_DEVICES 8
#define WEFT_VK_MAX_QUEUE_FAMILIES 16

typedef enum { WEFT_VK_QUEUE_COMPUTE_BIT = 0x00000002u } weft_vk_queue_flag_t;

// VkApplicationInfo (48 bytes, natural alignment matches the ABI exactly)
typedef struct {
    uint32_t        sType;              // 1000000000
    const void*     pNext;
    const char*     pApplicationName;
    uint32_t        applicationVersion;
    const char*     pEngineName;
    uint32_t        engineVersion;
    uint32_t        apiVersion;
} weft_vk_app_info_t;

// VkInstanceCreateInfo (64 bytes — field-for-field the ABI layout)
typedef struct {
    uint32_t        sType;              // 1
    const void*     pNext;
    uint32_t        flags;
    const void*     pApplicationInfo;
    uint32_t        enabledLayerCount;
    const void* const* ppEnabledLayerNames;
    uint32_t        enabledExtensionCount;
    const void* const* ppEnabledExtensionNames;
} weft_vk_instance_ci_t;

// VkPhysicalDeviceProperties is 812 bytes; we only READ apiVersion (offset
// 0) and deviceName (offset 20). Opaque buffer sized to cover the largest
// known struct (812) with margin; the loader never writes past sizeof.
typedef struct {
    uint8_t         raw[1024];
} weft_vk_phys_props_t;

static inline uint32_t weft_vk_props_api_version(const weft_vk_phys_props_t* p) {
    uint32_t v;
    memcpy(&v, p->raw + 0, sizeof(v));
    return v;
}
static inline const char* weft_vk_props_device_name(const weft_vk_phys_props_t* p) {
    return (const char*)(p->raw + 20);
}

// VkQueueFamilyProperties (24 bytes, no padding)
typedef struct {
    uint32_t        queueFlags;
    uint32_t        queueCount;
    uint32_t        timestampValidBits;
    uint32_t        minImageTransferGranularity[3];
} weft_vk_queue_family_props_t;

// VkPhysicalDeviceTimelineSemaphoreFeatures (24 bytes)
typedef struct {
    uint32_t        sType;              // 1000207002
    const void*     pNext;
    uint32_t        timelineSemaphore;
} weft_vk_timeline_sem_feats_t;

// VkPhysicalDeviceFeatures2 (sType 1000059000; 56 VkBool32 features)
typedef struct {
    uint32_t        sType;
    const void*     pNext;
    uint32_t        features56[56];
} weft_vk_phys_dev_feats2_t;

// Function pointer typedefs (the subset the probe touches). Handles are
// pointer-sized opaque values; we never dereference them.
typedef void* (*weft_vkGetInstanceProcAddr_t)(void*, const char*);
typedef uint32_t (*weft_vkCreateInstance_t)(const weft_vk_instance_ci_t*,
                                            const void*, void**);
typedef void (*weft_vkDestroyInstance_t)(void*, const void*);
typedef uint32_t (*weft_vkEnumeratePhysicalDevices_t)(void*, uint32_t*, void**);
typedef void (*weft_vkGetPhysicalDeviceProperties_t)(void*,
                                                     weft_vk_phys_props_t*);
typedef void (*weft_vkGetPhysicalDeviceQueueFamilyProperties_t)(
    void*, uint32_t*, weft_vk_queue_family_props_t*);
typedef void (*weft_vkGetPhysicalDeviceFeatures2_t)(void*,
                                                    weft_vk_phys_dev_feats2_t*);

// ---------------------------------------------------------------------------
// Loader state (resolved once at driver init; immutable afterwards)
// ---------------------------------------------------------------------------

static int g_probed = 0;
static int g_available = 0;
static char g_dev_name[WEFT_GPU_MAX_NAME];

int weft_gpu_vulkan13_probe(void) {
    g_probed = 1;
    g_available = 0;
    g_dev_name[0] = '\0';

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        return 0;   // honest absence: headless CI, no ICD (Law 3 degrades)
    }

    weft_vkGetInstanceProcAddr_t getProc = NULL;
    {
        void* sym = dlsym(lib, "vkGetInstanceProcAddr");
        memcpy(&getProc, &sym, sizeof(getProc));
    }
    if (getProc == NULL) {
        dlclose(lib);
        return 0;
    }

    // vkCreateInstance is a GLOBAL-level entry point (NULL instance).
    weft_vkCreateInstance_t createInstance = NULL;
    {
        void* sym = getProc(NULL, "vkCreateInstance");
        memcpy(&createInstance, &sym, sizeof(createInstance));
    }
    if (createInstance == NULL) {
        dlclose(lib);
        return 0;
    }

    weft_vk_app_info_t app;
    memset(&app, 0, sizeof(app));
    app.sType = 1000000000u;   // VK_STRUCTURE_TYPE_APPLICATION_INFO
    app.apiVersion = WEFT_VK_API_VERSION_1_3;

    weft_vk_instance_ci_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = 1u;             // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    ci.pApplicationInfo = &app;

    void* instance = NULL;
    if (createInstance(&ci, NULL, &instance) != WEFT_VK_SUCCESS ||
        instance == NULL) {
        dlclose(lib);
        return 0;   // no compatible ICD / driver — honest absence
    }

    weft_vkEnumeratePhysicalDevices_t enumDevs = NULL;
    weft_vkDestroyInstance_t destroyInstance = NULL;
    weft_vkGetPhysicalDeviceProperties_t getProps = NULL;
    weft_vkGetPhysicalDeviceQueueFamilyProperties_t getQFam = NULL;
    weft_vkGetPhysicalDeviceFeatures2_t getFeats2 = NULL;
    {
        void* sym;
        sym = getProc(instance, "vkEnumeratePhysicalDevices");
        memcpy(&enumDevs, &sym, sizeof(enumDevs));
        sym = getProc(instance, "vkDestroyInstance");
        memcpy(&destroyInstance, &sym, sizeof(destroyInstance));
        sym = getProc(instance, "vkGetPhysicalDeviceProperties");
        memcpy(&getProps, &sym, sizeof(getProps));
        sym = getProc(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        memcpy(&getQFam, &sym, sizeof(getQFam));
        sym = getProc(instance, "vkGetPhysicalDeviceFeatures2");
        memcpy(&getFeats2, &sym, sizeof(getFeats2));
    }

    int found = 0;
    if (enumDevs != NULL && getProps != NULL && getQFam != NULL &&
        getFeats2 != NULL && destroyInstance != NULL) {
        void* devs[WEFT_VK_MAX_PHYSICAL_DEVICES];
        uint32_t n = 0;
        if (enumDevs(instance, &n, NULL) == WEFT_VK_SUCCESS && n > 0) {
            uint32_t cap = (n > WEFT_VK_MAX_PHYSICAL_DEVICES)
                               ? WEFT_VK_MAX_PHYSICAL_DEVICES
                               : n;
            if (enumDevs(instance, &cap, devs) == WEFT_VK_SUCCESS) {
                for (uint32_t d = 0; d < cap && !found; d++) {
                    weft_vk_phys_props_t props;
                    memset(&props, 0, sizeof(props));
                    getProps(devs[d], &props);
                    if (weft_vk_props_api_version(&props) < WEFT_VK_API_VERSION_1_3) {
                        continue;   // Vulkan 1.3 timeline sync is REQUIRED
                    }
                    uint32_t total_q = 0;
                    getQFam(devs[d], &total_q, NULL);
                    uint32_t nq = (total_q > WEFT_VK_MAX_QUEUE_FAMILIES)
                                      ? WEFT_VK_MAX_QUEUE_FAMILIES
                                      : total_q;
                    weft_vk_queue_family_props_t qfam[WEFT_VK_MAX_QUEUE_FAMILIES];
                    memset(qfam, 0, sizeof(qfam));
                    getQFam(devs[d], &nq, qfam);
                    int has_compute = 0;
                    for (uint32_t q = 0; q < nq; q++) {
                        if ((qfam[q].queueFlags & WEFT_VK_QUEUE_COMPUTE_BIT) != 0u) {
                            has_compute = 1;
                            break;
                        }
                    }
                    if (!has_compute) {
                        continue;
                    }
                    weft_vk_timeline_sem_feats_t tl;
                    memset(&tl, 0, sizeof(tl));
                    tl.sType = 1000207002u;   // TIMELINE_SEMAPHORE_FEATURES
                    weft_vk_phys_dev_feats2_t f2;
                    memset(&f2, 0, sizeof(f2));
                    f2.sType = 1000059000u;   // PHYSICAL_DEVICE_FEATURES_2
                    f2.pNext = &tl;
                    getFeats2(devs[d], &f2);
                    if (tl.timelineSemaphore == 0u) {
                        continue;   // no timeline sync: refuse honestly
                    }
                    // WINNER: record the honest device identity
                    strncpy(g_dev_name, weft_vk_props_device_name(&props),
                            WEFT_GPU_MAX_NAME - 1);
                    g_dev_name[WEFT_GPU_MAX_NAME - 1] = '\0';
                    found = 1;
                }
            }
        }
    }

    if (destroyInstance != NULL) {
        destroyInstance(instance, NULL);
    }
    dlclose(lib);
    g_available = found;
    return found;
}

int weft_gpu_vulkan13_available(const char** out_dev_name) {
    if (!g_probed) {
        weft_gpu_vulkan13_probe();
    }
    if (out_dev_name != NULL) {
        *out_dev_name = g_dev_name;
    }
    return g_available;
}

const char* weft_gpu_vulkan13_device_name(void) {
    if (!g_probed) {
        weft_gpu_vulkan13_probe();
    }
    return g_dev_name;
}

void weft_gpu_vulkan13_reset_probe(void) {
    g_probed = 0;
    g_available = 0;
    g_dev_name[0] = '\0';
}
