// test_probe_golden.c — WSP5 P-series: golden hardware archetypes.
//
// Runs the REAL probe pipeline against all 12 deterministic mock
// archetypes and asserts, per archetype:
//   - exact capability words 0..4 (bit-exact bitmask expectations),
//   - exact scalar fields (cores, cache line, SIMD width, RAM, VRAM,
//     thermal class, boost headroom, NPU channels, NUMA nodes),
//   - the EXACT integer tier score and resulting tier (the normative
//     scoring table of D-51 §5, pinned),
//   - probe determinism (two consecutive probes byte-identical),
//   - golden ABI fixtures: the 256-byte profile image is written to
//     the golden directory (argv[1]) on first run and byte-compared
//     against the committed image afterwards (ABI freeze gate).
// Plus: fail-closed ladders for an empty source and the unmocked
// host probe, and the driver-layer promotion path.

#include "weft_spectrum_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fails++;                                                      \
            printf("FAIL P %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

#define SENT 0xFFFFFFFFu

typedef struct {
    uint32_t id;
    uint32_t tier;
    uint32_t score;
    const uint32_t *w[5];
    uint16_t cores_total, cores_perf, cores_eff;
    uint32_t cacheline, simd_bits;
    uint64_t ram, gpu_mem;
    uint32_t thermal, boost, npu_ch, gpu_eng, numa;
    uint16_t sources_and; /* bits that must be set   */
    uint16_t sources_not; /* bits that must be clear */
} wsp_expect_t;

static uint64_t word_from_ids(const uint32_t *ids)
{
    uint64_t w = 0;
    for (; *ids != SENT; ids++) {
        w |= (1ull << (*ids & 63u));
    }
    return w;
}

/* ---- expected capability sets per archetype ------------------------ */

static const uint32_t W0_M4[] = { WEFT_F_CPU_NEON, WEFT_F_CPU_AMX, SENT };
static const uint32_t W1_M4[] = { WEFT_F_CPU_128B_CACHELINE, SENT };
static const uint32_t W2_M4[] = { WEFT_F_GPU_PRESENT, WEFT_F_GPU_UNIFIED_MEMORY,
                                  WEFT_F_GPU_COMPUTE_SHADER,
                                  WEFT_F_GPU_METAL3_ARG_BUFFERS, SENT };
static const uint32_t W3_M4[] = { WEFT_F_NPU_PRESENT, WEFT_F_NPU_APPLE_ANE,
                                  WEFT_F_NPU_ZERO_COPY_ARENA, SENT };
static const uint32_t W4_M4[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                  WEFT_F_SYS_APPLE_OS, SENT };

static const uint32_t W0_A17[] = { WEFT_F_CPU_NEON, WEFT_F_CPU_AMX, SENT };
static const uint32_t W1_A17[] = { WEFT_F_CPU_HETERO_CORES,
                                   WEFT_F_CPU_128B_CACHELINE, SENT };
static const uint32_t W2_A17[] = { WEFT_F_GPU_PRESENT, WEFT_F_GPU_UNIFIED_MEMORY,
                                   WEFT_F_GPU_COMPUTE_SHADER,
                                   WEFT_F_GPU_METAL3_ARG_BUFFERS, SENT };
static const uint32_t W3_A17[] = { WEFT_F_NPU_PRESENT, WEFT_F_NPU_APPLE_ANE,
                                   WEFT_F_NPU_ZERO_COPY_ARENA, SENT };
static const uint32_t W4_A17[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                   WEFT_F_SYS_THERMAL_CAPPED,
                                   WEFT_F_SYS_APPLE_OS, SENT };

static const uint32_t W0_SD[] = { WEFT_F_CPU_NEON, WEFT_F_CPU_CRC32,
                                  WEFT_F_CPU_AES, WEFT_F_CPU_LSE_ATOMICS,
                                  WEFT_F_CPU_CLMUL, SENT };
static const uint32_t W1_SD[] = { WEFT_F_CPU_HETERO_CORES,
                                  WEFT_F_CPU_FREQ_BOOST, SENT };
static const uint32_t W2_SD[] = { WEFT_F_GPU_PRESENT,
                                  WEFT_F_GPU_UNIFIED_MEMORY,
                                  WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_SD[] = { WEFT_F_NPU_PRESENT,
                                  WEFT_F_NPU_HEXAGON_FASTRPC,
                                  WEFT_F_NPU_ZERO_COPY_ARENA,
                                  WEFT_F_NPU_MULTI_CHANNEL, SENT };
static const uint32_t W4_SD[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                  WEFT_F_SYS_THERMAL_CAPPED,
                                  WEFT_F_SYS_ANDROID, SENT };

static const uint32_t W0_D93[] = { WEFT_F_CPU_NEON, WEFT_F_CPU_CRC32,
                                   WEFT_F_CPU_AES, WEFT_F_CPU_LSE_ATOMICS,
                                   WEFT_F_CPU_CLMUL, SENT };
static const uint32_t W1_D93[] = { WEFT_F_CPU_HETERO_CORES,
                                   WEFT_F_CPU_FREQ_BOOST, SENT };
static const uint32_t W2_D93[] = { WEFT_F_GPU_PRESENT,
                                   WEFT_F_GPU_UNIFIED_MEMORY,
                                   WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_D93[] = { WEFT_F_NPU_PRESENT,
                                   WEFT_F_NPU_MTK_NEUROPILOT,
                                   WEFT_F_NPU_ZERO_COPY_ARENA, SENT };
static const uint32_t W4_D93[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                   WEFT_F_SYS_THERMAL_CAPPED,
                                   WEFT_F_SYS_ANDROID, SENT };

static const uint32_t W0_G88[] = { WEFT_F_CPU_NEON, WEFT_F_CPU_CRC32,
                                   WEFT_F_CPU_AES, WEFT_F_CPU_LSE_ATOMICS,
                                   WEFT_F_CPU_CLMUL, SENT };
static const uint32_t W1_G88[] = { WEFT_F_CPU_HETERO_CORES,
                                   WEFT_F_CPU_FREQ_BOOST, SENT };
static const uint32_t W2_G88[] = { WEFT_F_GPU_PRESENT,
                                   WEFT_F_GPU_UNIFIED_MEMORY,
                                   WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_G88[] = { SENT };
static const uint32_t W4_G88[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                   WEFT_F_SYS_LOW_RAM,
                                   WEFT_F_SYS_THERMAL_CAPPED,
                                   WEFT_F_SYS_ANDROID, SENT };

static const uint32_t W0_PI5[] = { WEFT_F_CPU_NEON, WEFT_F_CPU_CRC32,
                                   WEFT_F_CPU_AES, WEFT_F_CPU_LSE_ATOMICS,
                                   WEFT_F_CPU_CLMUL, SENT };
static const uint32_t W1_PI5[] = { WEFT_F_CPU_FREQ_BOOST, SENT };
static const uint32_t W2_PI5[] = { WEFT_F_GPU_PRESENT,
                                   WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_PI5[] = { SENT };
static const uint32_t W4_PI5[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                   SENT };

static const uint32_t W0_VF2[] = { SENT };
static const uint32_t W1_VF2[] = { SENT };
static const uint32_t W2_VF2[] = { SENT };
static const uint32_t W3_VF2[] = { SENT };
static const uint32_t W4_VF2[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                   SENT };

static const uint32_t W0_EPYC[] = { WEFT_F_CPU_AVX2, WEFT_F_CPU_AVX512F,
                                    WEFT_F_CPU_AVX512_BF16, WEFT_F_CPU_AVX_VNNI,
                                    WEFT_F_CPU_CRC32, WEFT_F_CPU_CLMUL,
                                    WEFT_F_CPU_AES, SENT };
static const uint32_t W1_EPYC[] = { WEFT_F_CPU_SMT, WEFT_F_CPU_MULTI_NUMA,
                                    WEFT_F_CPU_FREQ_BOOST, WEFT_F_CPU_TSO,
                                    SENT };
static const uint32_t W2_EPYC[] = { WEFT_F_GPU_PRESENT,
                                    WEFT_F_GPU_DISCRETE_VRAM,
                                    WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_EPYC[] = { SENT };
static const uint32_t W4_EPYC[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                    SENT };

static const uint32_t W0_SPR[] = { WEFT_F_CPU_AVX2, WEFT_F_CPU_AVX512F,
                                   WEFT_F_CPU_AVX_VNNI, WEFT_F_CPU_AVX512_BF16,
                                   WEFT_F_CPU_AMX, WEFT_F_CPU_CRC32,
                                   WEFT_F_CPU_CLMUL, WEFT_F_CPU_AES, SENT };
static const uint32_t W1_SPR[] = { WEFT_F_CPU_SMT, WEFT_F_CPU_MULTI_NUMA,
                                   WEFT_F_CPU_FREQ_BOOST, WEFT_F_CPU_TSO,
                                   SENT };
static const uint32_t W2_SPR[] = { WEFT_F_GPU_PRESENT,
                                   WEFT_F_GPU_DISCRETE_VRAM,
                                   WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_SPR[] = { SENT };
static const uint32_t W4_SPR[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                   SENT };

static const uint32_t W0_GH[] = { WEFT_F_CPU_NEON, WEFT_F_CPU_CRC32,
                                  WEFT_F_CPU_AES, WEFT_F_CPU_LSE_ATOMICS,
                                  WEFT_F_CPU_CLMUL, WEFT_F_CPU_SVE2,
                                  WEFT_F_CPU_SVE2_BF16, SENT };
static const uint32_t W1_GH[] = { SENT };
static const uint32_t W2_GH[] = { WEFT_F_GPU_PRESENT,
                                  WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_GH[] = { SENT };
static const uint32_t W4_GH[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                  SENT };

static const uint32_t W0_Z4[] = { WEFT_F_CPU_AVX2, WEFT_F_CPU_AVX512F,
                                  WEFT_F_CPU_AVX_VNNI, WEFT_F_CPU_AVX512_BF16,
                                  WEFT_F_CPU_CRC32, WEFT_F_CPU_CLMUL,
                                  WEFT_F_CPU_AES, SENT };
static const uint32_t W1_Z4[] = { WEFT_F_CPU_SMT, WEFT_F_CPU_FREQ_BOOST,
                                  WEFT_F_CPU_TSO, SENT };
static const uint32_t W2_Z4[] = { WEFT_F_GPU_PRESENT,
                                  WEFT_F_GPU_DISCRETE_VRAM,
                                  WEFT_F_GPU_DMABUF_IMPORT, SENT };
static const uint32_t W3_Z4[] = { SENT };
static const uint32_t W4_Z4[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                  SENT };

static const uint32_t W0_CT[] = { SENT };
static const uint32_t W1_CT[] = { WEFT_F_CPU_TSO, SENT };
static const uint32_t W2_CT[] = { SENT };
static const uint32_t W3_CT[] = { SENT };
static const uint32_t W4_CT[] = { WEFT_F_SYS_64BIT, WEFT_F_SYS_LITTLE_ENDIAN,
                                  WEFT_F_SYS_CONTAINERIZED,
                                  WEFT_F_SYS_BARE_FALLBACK, SENT };

static const wsp_expect_t EXPECT[] = {
    { WEFT_ARCHETYPE_APPLE_M4_MAX, 1, 88,
      { W0_M4, W1_M4, W2_M4, W3_M4, W4_M4 },
      16, 16, 0, 128, 128, 68719476736ull, 0, 0, 0, 1, 1, 1,
      (uint16_t)(WEFT_SRC_SYSCONF | WEFT_SRC_SYSCTL | WEFT_SRC_CACHELINE |
                 WEFT_SRC_MEMINFO | WEFT_SRC_TOPOLOGY | WEFT_SRC_KERNEL_REL),
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_DRM |
                 WEFT_SRC_SYSPROP | WEFT_SRC_THERMAL) },
    { WEFT_ARCHETYPE_APPLE_A17_PRO, 2, 53,
      { W0_A17, W1_A17, W2_A17, W3_A17, W4_A17 },
      6, 2, 4, 128, 128, 8589934592ull, 0, 8000, 0, 1, 1, 1,
      (uint16_t)(WEFT_SRC_SYSCONF | WEFT_SRC_SYSCTL | WEFT_SRC_CACHELINE |
                 WEFT_SRC_MEMINFO | WEFT_SRC_TOPOLOGY | WEFT_SRC_THERMAL |
                 WEFT_SRC_KERNEL_REL),
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_DRM) },
    { WEFT_ARCHETYPE_SNAPDRAGON_8GEN3, 2, 49,
      { W0_SD, W1_SD, W2_SD, W3_SD, W4_SD },
      8, 6, 2, 64, 128, 15957032960ull, 0, 8000, 3100, 2, 1, 1,
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF |
                 WEFT_SRC_CACHELINE | WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO |
                 WEFT_SRC_NUMA | WEFT_SRC_DRM | WEFT_SRC_SYSPROP |
                 WEFT_SRC_THERMAL | WEFT_SRC_FASTRPC | WEFT_SRC_KERNEL_REL |
                 WEFT_SRC_TOPOLOGY),
      (uint16_t)(WEFT_SRC_SYSCTL | WEFT_SRC_CGROUP_MEM |
                 WEFT_SRC_NEUROPILOT) },
    { WEFT_ARCHETYPE_DIMENSITY_9300, 2, 45,
      { W0_D93, W1_D93, W2_D93, W3_D93, W4_D93 },
      8, 4, 4, 64, 128, 16185139200ull, 0, 8000, 2925, 1, 1, 1,
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF |
                 WEFT_SRC_CACHELINE | WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO |
                 WEFT_SRC_NUMA | WEFT_SRC_DRM | WEFT_SRC_SYSPROP |
                 WEFT_SRC_THERMAL | WEFT_SRC_NEUROPILOT |
                 WEFT_SRC_KERNEL_REL | WEFT_SRC_TOPOLOGY),
      (uint16_t)(WEFT_SRC_SYSCTL | WEFT_SRC_FASTRPC) },
    { WEFT_ARCHETYPE_HELIO_G88, 3, 19,
      { W0_G88, W1_G88, W2_G88, W3_G88, W4_G88 },
      8, 2, 6, 64, 128, 3858550784ull, 0, 8000, 1050, 0, 1, 1,
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF |
                 WEFT_SRC_CACHELINE | WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO |
                 WEFT_SRC_NUMA | WEFT_SRC_DRM | WEFT_SRC_SYSPROP |
                 WEFT_SRC_THERMAL | WEFT_SRC_KERNEL_REL |
                 WEFT_SRC_TOPOLOGY),
      (uint16_t)(WEFT_SRC_SYSCTL | WEFT_SRC_FASTRPC |
                 WEFT_SRC_NEUROPILOT) },
    { WEFT_ARCHETYPE_RASPBERRY_PI_5, 3, 40,
      { W0_PI5, W1_PI5, W2_PI5, W3_PI5, W4_PI5 },
      4, 4, 0, 64, 128, 7987322880ull, 0, 0, 1800, 0, 1, 1,
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF |
                 WEFT_SRC_CACHELINE | WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO |
                 WEFT_SRC_NUMA | WEFT_SRC_DRM | WEFT_SRC_KERNEL_REL |
                 WEFT_SRC_TOPOLOGY),
      (uint16_t)(WEFT_SRC_SYSPROP | WEFT_SRC_THERMAL | WEFT_SRC_SYSCTL) },
    { WEFT_ARCHETYPE_VISIONFIVE_2, 3, 27,
      { W0_VF2, W1_VF2, W2_VF2, W3_VF2, W4_VF2 },
      4, 4, 0, 64, 0, 8084959232ull, 0, 0, 0, 0, 0, 1,
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF |
                 WEFT_SRC_MEMINFO | WEFT_SRC_NUMA | WEFT_SRC_KERNEL_REL),
      (uint16_t)(WEFT_SRC_DRM | WEFT_SRC_CPUFREQ | WEFT_SRC_SYSPROP |
                 WEFT_SRC_SYSCTL) },
    { WEFT_ARCHETYPE_EPYC_9654, 1, 85,
      { W0_EPYC, W1_EPYC, W2_EPYC, W3_EPYC, W4_EPYC },
      96, 96, 0, 64, 512, 791453007872ull, 17179869184ull, 0, 1550, 0, 1, 8,
      (uint16_t)(WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF | WEFT_SRC_CACHELINE |
                 WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO | WEFT_SRC_NUMA |
                 WEFT_SRC_DRM | WEFT_SRC_KERNEL_REL | WEFT_SRC_TOPOLOGY),
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_SYSPROP | WEFT_SRC_THERMAL) },
    { WEFT_ARCHETYPE_XEON_SPR_AMX, 1, 85,
      { W0_SPR, W1_SPR, W2_SPR, W3_SPR, W4_SPR },
      56, 56, 0, 64, 512, 270758051840ull, 0, 0, 4000, 0, 1, 2,
      (uint16_t)(WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF | WEFT_SRC_CACHELINE |
                 WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO | WEFT_SRC_NUMA |
                 WEFT_SRC_DRM | WEFT_SRC_KERNEL_REL | WEFT_SRC_TOPOLOGY),
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_SYSPROP | WEFT_SRC_THERMAL) },
    { WEFT_ARCHETYPE_GRACE_HOPPER, 1, 76,
      { W0_GH, W1_GH, W2_GH, W3_GH, W4_GH },
      72, 72, 0, 64, 128, 503075307520ull, 0, 0, 0, 0, 1, 1,
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF |
                 WEFT_SRC_CACHELINE | WEFT_SRC_MEMINFO | WEFT_SRC_NUMA |
                 WEFT_SRC_DRM | WEFT_SRC_KERNEL_REL),
      (uint16_t)(WEFT_SRC_CPUFREQ | WEFT_SRC_SYSPROP | WEFT_SRC_THERMAL |
                 WEFT_SRC_SYSCTL) },
    { WEFT_ARCHETYPE_DESKTOP_ZEN4, 1, 76,
      { W0_Z4, W1_Z4, W2_Z4, W3_Z4, W4_Z4 },
      6, 6, 0, 64, 512, 134481944576ull, 25769803776ull, 0, 2300, 0, 1, 1,
      (uint16_t)(WEFT_SRC_CPUINFO | WEFT_SRC_SYSCONF | WEFT_SRC_CACHELINE |
                 WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO | WEFT_SRC_NUMA |
                 WEFT_SRC_DRM | WEFT_SRC_KERNEL_REL | WEFT_SRC_TOPOLOGY),
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_SYSPROP | WEFT_SRC_THERMAL) },
    { WEFT_ARCHETYPE_CONTAINER_FALLBACK, 3, 31,
      { W0_CT, W1_CT, W2_CT, W3_CT, W4_CT },
      4, 4, 0, 64, 0, 8589934592ull, 0, 0, 0, 0, 0, 1,
      (uint16_t)(WEFT_SRC_SYSCONF | WEFT_SRC_CGROUP_MEM),
      (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO | WEFT_SRC_CACHELINE |
                 WEFT_SRC_CPUFREQ | WEFT_SRC_MEMINFO | WEFT_SRC_NUMA |
                 WEFT_SRC_DRM | WEFT_SRC_SYSCTL | WEFT_SRC_SYSPROP |
                 WEFT_SRC_THERMAL | WEFT_SRC_FASTRPC |
                 WEFT_SRC_NEUROPILOT | WEFT_SRC_KERNEL_REL |
                 WEFT_SRC_TOPOLOGY) },
};

#define EXPECT_COUNT (sizeof(EXPECT) / sizeof(EXPECT[0]))

/* ---- golden fixture IO --------------------------------------------- */

static int golden_write_or_compare(const char *dir, uint32_t id,
                                   const weft_hw_profile_t *p)
{
    char path[512];
    FILE *f;
    unsigned char image[256];
    unsigned char committed[256];
    size_t n;

    if (snprintf(path, sizeof(path), "%s/weft_hw_profile_%02u_%s.bin",
                 dir, (unsigned)id, weft_mock_archetype_name(id)) < 0) {
        return -1;
    }
    memcpy(image, p, sizeof(image));

    f = fopen(path, "rb");
    if (f != NULL) {
        n = fread(committed, 1, sizeof(committed), f);
        fclose(f);
        if (n != sizeof(committed) ||
            memcmp(committed, image, sizeof(image)) != 0) {
            printf("  golden ABI FREEZE VIOLATION: %s\n", path);
            return -1;
        }
        printf("  golden freeze OK: %s\n", path);
        return 0;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        printf("  golden write FAILED: %s\n", path);
        return -1;
    }
    n = fwrite(image, 1, sizeof(image), f);
    fclose(f);
    if (n != sizeof(image)) {
        return -1;
    }
    printf("  golden GENERATED: %s\n", path);
    return 0;
}

/* ---- empty source (bare-metal fail-closed ladder) ------------------ */

static uint64_t empty_auxv(uint32_t t) { (void)t; return 0u; }
static int empty_sysctl(const char *n, char *b, size_t c)
{ (void)n; (void)b; (void)c; return -1; }
static int empty_sysprop(const char *k, char *b, size_t c)
{ (void)k; (void)b; (void)c; return -1; }
static int empty_read_file(const char *p, char *b, size_t c)
{ (void)p; (void)b; (void)c; return -1; }
static int empty_file_exists(const char *p) { (void)p; return 0; }
static long empty_sysconf(int n) { (void)n; return -1; }
static uint32_t empty_arch_hint(void) { return WEFT_PROBE_ARCH_UNKNOWN; }
static uint64_t empty_now_ns(void) { return 0u; }

static const weft_probe_source_t EMPTY_SOURCE = {
    empty_auxv, empty_sysctl, empty_sysprop, empty_read_file,
    empty_file_exists, empty_sysconf, empty_arch_hint, empty_now_ns
};

int main(int argc, char **argv)
{
    const char *golden_dir = (argc > 1) ? argv[1] : "golden_out";
    size_t e;

    CHECK(EXPECT_COUNT == (size_t)WEFT_ARCHETYPE_COUNT,
          "expectation table covers all archetypes (%zu vs %d)",
          EXPECT_COUNT, WEFT_ARCHETYPE_COUNT);

    for (e = 0; e < EXPECT_COUNT; e++) {
        const wsp_expect_t *ex = &EXPECT[e];
        weft_hw_profile_t p1, p2;
        uint32_t w;

        CHECK(weft_mock_archetype_select(ex->id) == WEFT_SPECTRUM_OK,
              "select archetype %u", ex->id);
        CHECK(weft_hw_probe(&p1) == WEFT_SPECTRUM_OK, "probe %s",
              weft_mock_archetype_name(ex->id));
        CHECK(weft_hw_profile_validate(&p1) == WEFT_SPECTRUM_OK,
              "validate %s", weft_mock_archetype_name(ex->id));

        /* capability words, bit-exact */
        for (w = 0; w < 5u; w++) {
            uint64_t want = word_from_ids(ex->w[w]);
            CHECK(p1.caps[w] == want,
                  "%s caps[%u] = 0x%016llx want 0x%016llx",
                  weft_mock_archetype_name(ex->id), w,
                  (unsigned long long)p1.caps[w],
                  (unsigned long long)want);
        }

        /* scalar fields */
        CHECK(p1.cores_total == ex->cores_total, "%s cores_total %u",
              weft_mock_archetype_name(ex->id), p1.cores_total);
        CHECK(p1.cores_performance == ex->cores_perf, "%s cores_perf %u",
              weft_mock_archetype_name(ex->id), p1.cores_performance);
        CHECK(p1.cores_efficiency == ex->cores_eff, "%s cores_eff %u",
              weft_mock_archetype_name(ex->id), p1.cores_efficiency);
        CHECK(p1.cache_line_size == ex->cacheline, "%s cacheline %u",
              weft_mock_archetype_name(ex->id), p1.cache_line_size);
        CHECK(p1.simd_max_bits == ex->simd_bits, "%s simd %u",
              weft_mock_archetype_name(ex->id), p1.simd_max_bits);
        CHECK(p1.ram_total_bytes == ex->ram, "%s ram %llu",
              weft_mock_archetype_name(ex->id),
              (unsigned long long)p1.ram_total_bytes);
        CHECK(p1.ram_available_bytes == ex->ram, "%s ram_avail %llu",
              weft_mock_archetype_name(ex->id),
              (unsigned long long)p1.ram_available_bytes);
        CHECK(p1.gpu_memory_bytes == ex->gpu_mem, "%s gpu_mem %llu",
              weft_mock_archetype_name(ex->id),
              (unsigned long long)p1.gpu_memory_bytes);
        CHECK(p1.thermal_limit_mw == ex->thermal, "%s thermal %u",
              weft_mock_archetype_name(ex->id), p1.thermal_limit_mw);
        CHECK(p1.boost_headroom_mhz == ex->boost, "%s boost %u",
              weft_mock_archetype_name(ex->id), p1.boost_headroom_mhz);
        CHECK(p1.npu_channels == ex->npu_ch, "%s npu_ch %u",
              weft_mock_archetype_name(ex->id), p1.npu_channels);
        CHECK(p1.gpu_engines == ex->gpu_eng, "%s gpu_eng %u",
              weft_mock_archetype_name(ex->id), p1.gpu_engines);
        CHECK(p1.numa_node_count == ex->numa, "%s numa %u",
              weft_mock_archetype_name(ex->id), p1.numa_node_count);
        CHECK(p1.display_max_hz == 0u, "%s display unknown",
              weft_mock_archetype_name(ex->id));
        CHECK(p1.probe_cost_ns == 0u, "%s frozen mock clock",
              weft_mock_archetype_name(ex->id));

        /* source acknowledgement mask */
        CHECK((p1.probe_sources_ok & ex->sources_and) == ex->sources_and,
              "%s sources missing bits 0x%04x (have 0x%04x)",
              weft_mock_archetype_name(ex->id), ex->sources_and,
              p1.probe_sources_ok);
        CHECK((p1.probe_sources_ok & ex->sources_not) == 0u,
              "%s sources stray bits (have 0x%04x)",
              weft_mock_archetype_name(ex->id), p1.probe_sources_ok);

        /* normative tier scoring — exact integers */
        CHECK(weft_governor_score(&p1) == ex->score,
              "%s score %u want %u", weft_mock_archetype_name(ex->id),
              weft_governor_score(&p1), ex->score);
        CHECK(weft_governor_tier_for_profile(&p1) == ex->tier,
              "%s tier %u want %u", weft_mock_archetype_name(ex->id),
              weft_governor_tier_for_profile(&p1), ex->tier);

        /* determinism: second probe byte-identical */
        CHECK(weft_hw_probe(&p2) == WEFT_SPECTRUM_OK, "reprobe %s",
              weft_mock_archetype_name(ex->id));
        CHECK(memcmp(&p1, &p2, sizeof(p1)) == 0, "%s deterministic",
              weft_mock_archetype_name(ex->id));

        /* golden ABI fixture */
        CHECK(golden_write_or_compare(golden_dir, ex->id, &p1) == 0,
              "golden %s", weft_mock_archetype_name(ex->id));

        printf("P %-24s tier=%u score=%2u caps=%016llx %016llx %016llx "
               "%016llx %016llx\n",
               weft_mock_archetype_name(ex->id),
               weft_governor_tier_for_profile(&p1),
               weft_governor_score(&p1),
               (unsigned long long)p1.caps[0],
               (unsigned long long)p1.caps[1],
               (unsigned long long)p1.caps[2],
               (unsigned long long)p1.caps[3],
               (unsigned long long)p1.caps[4]);
    }

    /* ---- EPYC NUMA summary mask (low-32 truncation) ------------- */
    {
        weft_hw_profile_t p;
        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_EPYC_9654) ==
              WEFT_SPECTRUM_OK, "select epyc");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe epyc");
        CHECK(p.numa_cpu_mask[0] == 0x00000FFFu, "node0 mask %08x",
              p.numa_cpu_mask[0]);
        CHECK(p.numa_cpu_mask[7] == 0u, "node7 mask empty");
    }

    /* ---- fail-closed: empty source ------------------------------- */
    {
        weft_hw_profile_t p;
        weft_probe_set_source(&EMPTY_SOURCE);
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "empty probe OK");
        CHECK(weft_hw_profile_validate(&p) == WEFT_SPECTRUM_OK,
              "empty probe sealed");
        CHECK(weft_hw_has_feature(&p, WEFT_F_SYS_BARE_FALLBACK),
              "bare fallback asserted");
        CHECK(!weft_hw_has_feature(&p, WEFT_F_SYS_64BIT) == false,
              "64bit host fact");
        CHECK(p.cache_line_size == 64u, "conservative 64B baseline");
        CHECK(p.cores_total == 0u, "honest unknown cores");
        CHECK(p.ram_total_bytes == 0ull, "honest unknown RAM");
        CHECK(p.simd_max_bits == 0u, "scalar fail-closed");
        CHECK(weft_governor_score(&p) == 11u, "empty score %u",
              weft_governor_score(&p));
        CHECK(weft_governor_tier_for_profile(&p) ==
              (uint32_t)WEFT_SPECTRUM_TIER_3, "empty tier 3");
    }

    /* ---- fail-closed: unknown archetype id ----------------------- */
    CHECK(weft_mock_archetype_select(999u) == WEFT_SPECTRUM_ENOARCHETYPE,
          "unknown archetype refused");

    /* ---- driver-layer promotion path ----------------------------- */
    {
        weft_hw_profile_t p;
        CHECK(weft_mock_archetype_select(WEFT_ARCHETYPE_SNAPDRAGON_8GEN3) ==
              WEFT_SPECTRUM_OK, "select sd for promote");
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "probe sd");
        CHECK(!weft_hw_has_feature(&p, WEFT_F_GPU_VULKAN_TIMELINE_SEM),
              "timeline not statically claimed");
        CHECK(weft_hw_profile_promote_feature(&p,
              (uint32_t)WEFT_F_GPU_VULKAN_TIMELINE_SEM), "promote timeline");
        CHECK(weft_hw_has_feature(&p, WEFT_F_GPU_VULKAN_TIMELINE_SEM),
              "timeline after promote");
        CHECK(weft_hw_profile_validate(&p) == WEFT_SPECTRUM_OK,
              "re-sealed after promote");
    }

    /* ---- real host probe (unmocked, invariant-only) -------------- */
    {
        weft_hw_profile_t p;
        weft_mock_archetype_clear();
        weft_spectrum_warmup();
        CHECK(weft_hw_probe(&p) == WEFT_SPECTRUM_OK, "host probe OK");
        CHECK(weft_hw_profile_validate(&p) == WEFT_SPECTRUM_OK,
              "host validate");
        CHECK(p.cache_line_size == 32u || p.cache_line_size == 64u ||
              p.cache_line_size == 128u, "host cacheline sane");
        CHECK(p.ram_total_bytes > 0ull, "host ram positive");
        CHECK(weft_hw_has_feature(&p, WEFT_F_SYS_LITTLE_ENDIAN), "host LE");
        CHECK(weft_hw_has_feature(&p, WEFT_F_SYS_64BIT), "host 64bit");
        CHECK(weft_governor_tier_for_profile(&p) >= 1u &&
              weft_governor_tier_for_profile(&p) <= 3u, "host tier valid");
        CHECK(p.probe_cost_ns > 0u, "host probe cost recorded");
        printf("P host-probe: cores=%u perf=%u ram=%.2fGiB tier=%u "
               "score=%u cost=%uns sources=0x%04x\n",
               p.cores_total, p.cores_performance,
               (double)p.ram_total_bytes / 1073741824.0,
               weft_governor_tier_for_profile(&p),
               weft_governor_score(&p), p.probe_cost_ns,
               p.probe_sources_ok);
    }

    printf("P-series: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
