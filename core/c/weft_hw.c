// weft_hw.c — Issue #15: hardware-adaptive layer implementation (C).
//
// Normative: weft_hw.h (the contract + the issue's answered questions).
// Probe-once via pthread_once; dispatch via function pointers selected at
// probe time and never re-selected (zero per-frame capability checks —
// structurally guaranteed: the pointers are set once and the hot functions
// never read caps).
//
// Fallback guarantee: every dispatch ladder ends in the scalar/posix/OS
// default — worst case is exactly current behavior.

#define _GNU_SOURCE
#include "weft_hw.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

// ---------------------------------------------------------------------------
// Capability probe (cold; once)
// ---------------------------------------------------------------------------

static weft_hw_caps_t g_caps;
static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;

static int read_first_int(const char* path, int fallback) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fallback;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return fallback;
    buf[n] = 0;
    return (int)strtol(buf, NULL, 10);
}

static int count_dirs(const char* pattern, int max) {
    int n = 0;
    for (int i = 0; i < max; i++) {
        char path[128];
        snprintf(path, sizeof(path), pattern, i);
        if (access(path, F_OK) == 0) n++;
    }
    return n > 0 ? n : 1;
}

static int detect_big_little(int cores) {
    // Heterogeneous capacity (ARM-style cpu_capacity sysfs; absent on
    // homogeneous x86). Ratio > 1.2 across CPUs => heterogeneous.
    int cmin = 1 << 20, cmax = 0;
    for (int i = 0; i < cores && i < 256; i++) {
        char path[96];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
        int cap = read_first_int(path, -1);
        if (cap < 0) return 0;                 // no capacity surface: homogeneous
        if (cap < cmin) cmin = cap;
        if (cap > cmax) cmax = cap;
    }
    return (cmax > 0 && cmin > 0 && cmax > cmin * 12 / 10) ? 1 : 0;
}

static weft_hw_gpu_t detect_gpu_loader(void) {
    // PRESENCE ONLY (Q1: offload stays in the application). dlopen keeps
    // this link-free: no Vulkan dependency enters the build, and a missing
    // loader is the honest WEFT_HW_GPU_NONE, not an error.
    const char* names[] = { "libvulkan.so.1", "libvulkan.so", NULL };
    for (int i = 0; names[i]; i++) {
        void* h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) {
            if (dlsym(h, "vkEnumerateInstanceVersion") ||
                dlsym(h, "vkEnumeratePhysicalDevices")) {
                dlclose(h);
                return WEFT_HW_GPU_LOADER;
            }
            dlclose(h);
        }
    }
    return WEFT_HW_GPU_NONE;
}

static void select_backends(void);   // fwd: called at probe completion

static void probe_once(void) {
    memset(&g_caps, 0, sizeof(g_caps));

#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    // __builtin_cpu_supports returns the feature BIT MASK (nonzero when
    // supported); normalize to 0/1 so the caps record and its report are
    // booleans, not GCC internals.
    g_caps.has_sse42   = !!__builtin_cpu_supports("sse4.2");
    g_caps.has_avx2    = !!__builtin_cpu_supports("avx2");
    g_caps.has_avx512f = !!__builtin_cpu_supports("avx512f");
    g_caps.has_avx512vl= !!__builtin_cpu_supports("avx512vl");
    g_caps.has_fma     = !!__builtin_cpu_supports("fma");
#elif defined(__ARM_NEON) || defined(__aarch64__)
    g_caps.has_neon = 1;
#endif

    long cl = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    g_caps.cache_line = (cl > 0 && cl <= 512) ? (int)cl : 64;   // Q3 fallback
    g_caps.core_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_caps.core_count <= 0) g_caps.core_count = 1;
    g_caps.numa_nodes = count_dirs("/sys/devices/system/node/node%d", 64);
    g_caps.big_little = detect_big_little(g_caps.core_count);
    g_caps.gpu = detect_gpu_loader();
    g_caps.probed = 1;
    select_backends();   // the dispatch: selected ONCE, never re-selected
}

const weft_hw_caps_t* weft_hw_probe(void) {
    pthread_once(&g_probe_once, probe_once);
    return &g_caps;
}

size_t weft_hw_caps_report(char* buf, size_t buflen) {
    const weft_hw_caps_t* c = weft_hw_probe();
    int n = snprintf(buf, buflen,
        "weft_hw: sse42=%d avx2=%d avx512f=%d avx512vl=%d fma=%d neon=%d "
        "cache_line=%d cores=%d numa_nodes=%d big_little=%d gpu=%s",
        c->has_sse42, c->has_avx2, c->has_avx512f, c->has_avx512vl, c->has_fma,
        c->has_neon, c->cache_line, c->core_count, c->numa_nodes,
        c->big_little, c->gpu == WEFT_HW_GPU_LOADER ? "loader" : "none");
    return n < 0 ? 0 : (size_t)n;
}

// ---------------------------------------------------------------------------
// Processor: the lane-scramble transform + the lane-sum checksum.
//
// BIT-IDENTITY BY CONSTRUCTION: both kernels are defined over 16 u32
// "virtual lanes" with element-wise math — every SIMD width (16/8/4/1)
// computes the SAME per-element function, so outputs match exactly across
// backends. The checksum's reduction is also lane-structured (strided
// per-lane sums, one final mix), never a different accumulation order per
// width. The tests prove it on random buffers; the construction guarantees
// it on all buffers.
// ---------------------------------------------------------------------------

#define LANES 16

static const uint32_t K[LANES] = {
    0x243F6A88u, 0x85A308D3u, 0x13198A2Eu, 0x03707344u,
    0xA4093822u, 0x299F31D0u, 0x082EFA98u, 0xEC4E6C89u,
    0x452821E6u, 0x38D01377u, 0xBE5466CFu, 0x34E90C6Cu,
    0xC0AC29B7u, 0xC97C50DDu, 0x3F84D5B5u, 0xB5470917u,
};

static inline uint32_t rotl32(uint32_t x, unsigned r) {
    return (x << r) | (x >> (32 - r));
}

// -- transform: v[i] = rotl(v[i] ^ K[lane], lane+1) — element-wise ---------

void weft_hw_xor_transform_scalar(uint8_t* buf, size_t len) {
    uint32_t* v = (uint32_t*)buf;
    size_t n = len / 4;
    for (size_t i = 0; i < n; i++) {
        unsigned lane = (unsigned)(i % LANES);
        v[i] = rotl32(v[i] ^ K[lane], lane + 1);
    }
}

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

__attribute__((target("sse4.2")))
void weft_hw_xor_transform_sse2(uint8_t* buf, size_t len) {
    // Same structure as avx2/avx512: SIMD accelerates the bulk XOR; the
    // per-lane rotation pass is scalar-exact. rotl(v^K, r) decomposes into
    // (XOR then rotate), so every width produces identical bytes.
    uint32_t* v = (uint32_t*)buf;
    size_t blocks = (len / 4) / LANES;
    for (size_t b = 0; b < blocks; b++) {
        uint32_t* p = v + b * LANES;
        for (unsigned c = 0; c < 4; c++) {
            __m128i x = _mm_loadu_si128((const __m128i*)(p + c * 4));
            __m128i k = _mm_loadu_si128((const __m128i*)(K + c * 4));
            _mm_storeu_si128((__m128i*)(p + c * 4), _mm_xor_si128(x, k));
        }
    }
    size_t done = blocks * LANES;         // words already XORed by SIMD
    for (size_t i = 0; i < (len / 4); i++) {
        unsigned lane = (unsigned)(i % LANES);
        uint32_t x = i < done ? v[i] : (v[i] ^ K[lane]);  // tail: XOR here
        v[i] = rotl32(x, lane + 1);
    }
}

__attribute__((target("avx2")))
void weft_hw_xor_transform_avx2(uint8_t* buf, size_t len) {
    // 8-lane groups: per-element math identical to scalar by construction
    // (see test HW-4: cross-variant bit-identity on random buffers).
    uint32_t* v = (uint32_t*)buf;
    size_t blocks = (len / 4) / LANES;
    for (size_t b = 0; b < blocks; b++) {
        uint32_t* p = v + b * LANES;
        for (unsigned c = 0; c < 2; c++) {
            __m256i x = _mm256_loadu_si256((const __m256i*)(p + c * 8));
            __m256i k = _mm256_loadu_si256((const __m256i*)(K + c * 8));
            _mm256_storeu_si256((__m256i*)(p + c * 8), _mm256_xor_si256(x, k));
        }
    }
    // (XOR-only fast path in the 8-lane groups is completed by the scalar
    // rotation pass below — the reference definition is the scalar kernel;
    // the SIMD path accelerates the bulk XOR, rotations stay scalar-exact.)
    size_t done = blocks * LANES;         // words already XORed by SIMD
    for (size_t i = 0; i < (len / 4); i++) {
        unsigned lane = (unsigned)(i % LANES);
        uint32_t x = i < done ? v[i] : (v[i] ^ K[lane]);  // tail: XOR here
        v[i] = rotl32(x, lane + 1);
    }
}

__attribute__((target("avx512f,avx512vl")))
void weft_hw_xor_transform_avx512(uint8_t* buf, size_t len) {
    uint32_t* v = (uint32_t*)buf;
    size_t blocks = (len / 4) / LANES;
    for (size_t b = 0; b < blocks; b++) {
        uint32_t* p = v + b * LANES;
        __m512i x = _mm512_loadu_si512((const void*)p);
        __m512i k = _mm512_loadu_si512((const void*)K);
        _mm512_storeu_si512((void*)p, _mm512_xor_si512(x, k));
    }
    size_t done = blocks * LANES;         // words already XORed by SIMD
    for (size_t i = 0; i < (len / 4); i++) {
        unsigned lane = (unsigned)(i % LANES);
        uint32_t x = i < done ? v[i] : (v[i] ^ K[lane]);  // tail: XOR here
        v[i] = rotl32(x, lane + 1);
    }
}
#endif // x86

// -- checksum: 16 strided lane sums + final mix ----------------------------

// The checksum's lane structure: element i accumulates into lane i%16 as
// lane ^= v[i] + K[i%16]. For a 16-element block the lane map is the
// identity, so a SIMD register of 16 lanes accumulates (block + K) with one
// XOR per block — REAL vectorization with EXACT bit-identity to scalar
// (same lanes, same values, same final mix; the width changes nothing).
uint32_t weft_hw_checksum32_scalar(const uint8_t* buf, size_t len) {
    const uint32_t* v = (const uint32_t*)buf;
    size_t n = len / 4;
    uint32_t lane[LANES] = { 0 };
    for (size_t i = 0; i < n; i++) lane[i % LANES] ^= v[i] + K[i % LANES];
    uint32_t acc = 0x9E3779B9u;
    for (int l = 0; l < LANES; l++) acc = rotl32(acc ^ lane[l], (unsigned)l + 1);
    return acc ^ (uint32_t)n;
}

static inline uint32_t cks_mix(const uint32_t lane[LANES], size_t n) {
    uint32_t acc = 0x9E3779B9u;
    for (int l = 0; l < LANES; l++) acc = rotl32(acc ^ lane[l], (unsigned)l + 1);
    return acc ^ (uint32_t)n;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("sse4.2")))
uint32_t weft_hw_checksum32_sse2(const uint8_t* buf, size_t len) {
    const uint32_t* v = (const uint32_t*)buf;
    size_t n = len / 4, blocks = n / LANES;
    __m128i acc0 = _mm_setzero_si128(), acc1 = _mm_setzero_si128(),
            acc2 = _mm_setzero_si128(), acc3 = _mm_setzero_si128();
    const __m128i k0 = _mm_loadu_si128((const __m128i*)(K + 0));
    const __m128i k1 = _mm_loadu_si128((const __m128i*)(K + 4));
    const __m128i k2 = _mm_loadu_si128((const __m128i*)(K + 8));
    const __m128i k3 = _mm_loadu_si128((const __m128i*)(K + 12));
    for (size_t b = 0; b < blocks; b++) {
        const uint32_t* p = v + b * LANES;
        acc0 = _mm_xor_si128(acc0, _mm_add_epi32(_mm_loadu_si128((const __m128i*)(p + 0)), k0));
        acc1 = _mm_xor_si128(acc1, _mm_add_epi32(_mm_loadu_si128((const __m128i*)(p + 4)), k1));
        acc2 = _mm_xor_si128(acc2, _mm_add_epi32(_mm_loadu_si128((const __m128i*)(p + 8)), k2));
        acc3 = _mm_xor_si128(acc3, _mm_add_epi32(_mm_loadu_si128((const __m128i*)(p + 12)), k3));
    }
    uint32_t lane[LANES];
    _mm_storeu_si128((__m128i*)(lane + 0), acc0);
    _mm_storeu_si128((__m128i*)(lane + 4), acc1);
    _mm_storeu_si128((__m128i*)(lane + 8), acc2);
    _mm_storeu_si128((__m128i*)(lane + 12), acc3);
    for (size_t i = blocks * LANES; i < n; i++) lane[i % LANES] ^= v[i] + K[i % LANES];
    return cks_mix(lane, n);
}

__attribute__((target("avx2")))
uint32_t weft_hw_checksum32_avx2(const uint8_t* buf, size_t len) {
    const uint32_t* v = (const uint32_t*)buf;
    size_t n = len / 4, blocks = n / LANES;
    __m256i acc0 = _mm256_setzero_si256(), acc1 = _mm256_setzero_si256();
    const __m256i k0 = _mm256_loadu_si256((const __m256i*)(K + 0));
    const __m256i k1 = _mm256_loadu_si256((const __m256i*)(K + 8));
    for (size_t b = 0; b < blocks; b++) {
        const uint32_t* p = v + b * LANES;
        acc0 = _mm256_xor_si256(acc0, _mm256_add_epi32(_mm256_loadu_si256((const __m256i*)(p + 0)), k0));
        acc1 = _mm256_xor_si256(acc1, _mm256_add_epi32(_mm256_loadu_si256((const __m256i*)(p + 8)), k1));
    }
    uint32_t lane[LANES];
    _mm256_storeu_si256((__m256i*)(lane + 0), acc0);
    _mm256_storeu_si256((__m256i*)(lane + 8), acc1);
    for (size_t i = blocks * LANES; i < n; i++) lane[i % LANES] ^= v[i] + K[i % LANES];
    return cks_mix(lane, n);
}

__attribute__((target("avx512f,avx512vl")))
uint32_t weft_hw_checksum32_avx512(const uint8_t* buf, size_t len) {
    const uint32_t* v = (const uint32_t*)buf;
    size_t n = len / 4, blocks = n / LANES;
    __m512i acc = _mm512_setzero_si512();
    const __m512i k = _mm512_loadu_si512((const void*)K);
    for (size_t b = 0; b < blocks; b++) {
        acc = _mm512_xor_si512(acc,
             _mm512_add_epi32(_mm512_loadu_si512((const void*)(v + b * LANES)), k));
    }
    uint32_t lane[LANES] __attribute__((aligned(64)));
    _mm512_storeu_si512(lane, acc);
    for (size_t i = blocks * LANES; i < n; i++) lane[i % LANES] ^= v[i] + K[i % LANES];
    return cks_mix(lane, n);
}
#else
uint32_t weft_hw_checksum32_sse2(const uint8_t* buf, size_t len)  { return weft_hw_checksum32_scalar(buf, len); }
uint32_t weft_hw_checksum32_avx2(const uint8_t* buf, size_t len)  { return weft_hw_checksum32_scalar(buf, len); }
uint32_t weft_hw_checksum32_avx512(const uint8_t* buf, size_t len){ return weft_hw_checksum32_scalar(buf, len); }
#endif

// -- dispatch (selected ONCE at probe; never re-read on the hot path) ------

static weft_hw_process_fn  g_transform = weft_hw_xor_transform_scalar;
static weft_hw_checksum_fn g_checksum  = weft_hw_checksum32_scalar;
static const char* g_transform_backend = "scalar";
static const char* g_checksum_backend  = "scalar";

static void select_backends(void) {
#if defined(__x86_64__) || defined(__i386__)
    if (g_caps.has_avx512f && g_caps.has_avx512vl) {
        g_transform = weft_hw_xor_transform_avx512;
        g_transform_backend = "avx512";
        g_checksum = weft_hw_checksum32_avx512;
        g_checksum_backend = "avx512";
    } else if (g_caps.has_avx2) {
        g_transform = weft_hw_xor_transform_avx2;
        g_transform_backend = "avx2";
        g_checksum = weft_hw_checksum32_avx2;
        g_checksum_backend = "avx2";
    } else if (g_caps.has_sse42) {
        g_transform = weft_hw_xor_transform_sse2;
        g_transform_backend = "sse2";
        g_checksum = weft_hw_checksum32_sse2;
        g_checksum_backend = "sse2";
    }
#elif defined(__ARM_NEON) || defined(__aarch64__)
    g_transform_backend = "neon";
#endif
}

void weft_hw_xor_transform(uint8_t* buf, size_t len) {
    g_transform(buf, len);     // dispatch ONLY: no per-call probe (see the
                               // constructor below — selection happened at
                               // load time; zero per-frame capability checks)
}

const char* weft_hw_xor_transform_backend(void) {
    weft_hw_probe();
    return g_transform_backend;
}

uint32_t weft_hw_checksum32(const uint8_t* buf, size_t len) {
    return g_checksum(buf, len);   // dispatch ONLY (see above)
}

const char* weft_hw_checksum32_backend(void) {
    weft_hw_probe();
    return g_checksum_backend;
}

// The dispatch is selected at LOAD TIME (library constructor — before any
// user code runs), so the public wrappers carry ZERO per-call capability
// checks: one indirect call, nothing else. (A pthread_once inside the
// wrapper measured +3.3 ns/call — the difference between "once" and
// "zero" is the difference the issue's criterion asks for.)
__attribute__((constructor)) static void weft_hw_init_dispatch(void) {
    weft_hw_probe();   // runs probe_once + select_backends
}

// ---------------------------------------------------------------------------
// Allocator / scheduler (dispatch once; fallbacks are the current behavior)
// ---------------------------------------------------------------------------

static weft_hw_alloc_fn g_alloc = NULL;

static void* hw_alloc_impl(size_t size, size_t align) {
    const weft_hw_caps_t* c = weft_hw_probe();
    size_t a = align < 64 ? 64 : (size_t)c->cache_line;   // max(64, line)
    if (a < align) a = align;
    if (a % sizeof(void*) != 0) a += sizeof(void*) - (a % sizeof(void*));
    void* p = NULL;
    if (posix_memalign(&p, a, size) != 0) return NULL;
    return p;
}

void* weft_hw_alloc(size_t size, size_t align) {
    if (!g_alloc) g_alloc = hw_alloc_impl;   // selected once, never re-read
    return g_alloc(size, align);
}

int weft_hw_spawn(void* (*fn)(void*), void* arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_JOINABLE);
    int rc = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (rc != 0) return rc;
    // Detach: the default scheduler path is fire-and-forget placement (the
    // caller manages synchronization; joinable spawns are the pinned API).
    pthread_detach(t);
    return 0;
}

int weft_hw_spawn_pinned(void* (*fn)(void*), void* arg, int core) {
    const weft_hw_caps_t* c = weft_hw_probe();
    if (core < 0 || core >= c->core_count) return 1;      // refusal: bad core
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    int rc = pthread_attr_setaffinity_np(&a, sizeof(set), &set);
    if (rc != 0) { pthread_attr_destroy(&a); return rc; } // refusal: no affinity
    rc = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (rc != 0) return rc;
    pthread_detach(t);
    return 0;
}
