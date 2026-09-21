// weft_mock_archetypes.c — WSP5 golden hardware archetypes (test support).
//
// Twelve deterministic mock SOURCES spanning the mission's silicon
// spectrum — 128-core workstation flagships down to 2-4 GB budget
// phones and RISC-V boards. Each archetype feeds the REAL probe
// pipeline (weft_hw_probe) synthetic-but-faithful OS evidence: auxv
// hwcaps, /proc/cpuinfo stanzas, sysfs topology/cpufreq/NUMA files,
// cgroup ceilings, DRM PCI attributes, FastRPC device nodes, Darwin
// sysctls and Android system properties.
//
// The resulting profiles are byte-frozen into the committed golden
// ABI fixtures (tests/spectrum/golden/*.bin) and pinned semantically
// by the P/G test ladders (exact tier, exact integer score, exact
// capability bits). Values are calibrated to real hardware datasheets
// where public (cache lines, cluster counts, TDP-class envelopes) —
// see D-51 §5 for the full scoring matrix.
//
// Determinism: the mock clock is frozen at zero (now_ns() == 0), so
// probe_cost_ns == 0 and every fixture regenerates byte-identically.

#include "weft_spectrum_internal.h"

#include <string.h>
#include <stddef.h>

typedef struct {
    const char *k;
    const char *v;
} wsp_kv_t;

typedef struct {
    uint32_t id;
    const char *name;
    uint32_t arch;               /* enum weft_probe_arch                */
    uint64_t hwcap;              /* AT_HWCAP (0 = denied / unavailable) */
    uint64_t hwcap2;             /* AT_HWCAP2                           */
    long     ncpu;               /* sysconf(_SC_NPROCESSORS_ONLN)       */
    const wsp_kv_t *files;       /* /proc + /sys + /dev evidence        */
    const wsp_kv_t *sysctls;     /* Darwin sysctl evidence              */
    const wsp_kv_t *props;       /* Android property evidence           */
    uint32_t uniform_freq_khz;   /* per-cpu cpuinfo_max_freq (0 = none) */
    uint32_t uniform_min_freq_khz; /* cpu0 cpuinfo_min_freq             */
    const uint32_t *percpu_freq_khz; /* NULL = uniform                  */
    uint32_t percpu_freq_count;
} wsp_mock_env_t;

#define WSP_KV_END { NULL, NULL }

/* Shared cpuinfo Features lines (tokens mapped by the probe). */
#define WSP_FEAT_PHONE  "fp asimd evt aes pmull sha1 sha2 crc32 atomics"
#define WSP_FEAT_GH     "fp asimd evt aes pmull sha1 sha2 crc32 atomics" \
                        " sve sve2 sveaes svepmull svebitperm svesha3" \
                        " svesm4 svebf16 i8mm"

/* x86 flags lines (subset realistic for the archetype). */
#define WSP_FLAGS_GENOA  "fpu vme de pse tsc msr pae mce cx8 apic sep" \
    " clflush mmx fxsr sse sse2 ht syscall nx lm constant_tsc rep_good" \
    " nopl cpuid pni pclmulqdq monitor ssse3 fma cx16 sse4_1 sse4_2" \
    " movbe popcnt aes xsave avx f16c rdrand lahf_lm cmp_legacy svm" \
    " cr8_legacy abm sse4a misalignsse 3dnowprefetch osvw topoext" \
    " perfctr_core perfctr_nb bpext cpb hw_pstate ssbd ibrs ibpb stibp" \
    " vmmcall fsgsbase bmi1 avx2 smep bmi2 erms invpcid cqm rdt_a" \
    " avx512f avx512dq rdseed adx smap clflushopt clwb avx512cd" \
    " avx512bw avx512vl avx512_bf16 clzero wbnoinvd arat nrip_save" \
    " tsc_scale vmcb_clean flushbyasid pausefilter avic v_vmsave_vmload" \
    " vgif umip pku ospke vaes vpclmulqdq avx512_vnni avx512_vpopcntdq"

#define WSP_FLAGS_SPR   "fpu vme de pse tsc msr pae mce cx8 apic sep" \
    " clflush mmx fxsr sse sse2 ht syscall nx lm constant_tsc rep_good" \
    " nopl cpuid pni pclmulqdq monitor ssse3 fma cx16 sse4_1 sse4_2" \
    " movbe popcnt aes xsave avx f16c rdrand lahf_lm abm cpuid" \
    " fsgsbase tsc_adjust bmi1 avx2 smep bmi2 erms invpcid rdseed adx" \
    " smap clflushopt clwb intel_pt avx512f avx512cd avx512dq avx512bw" \
    " avx512vl avx512_bf16 avx512_vnni avx_vnni gfni vaes vpclmulqdq" \
    " avx512_vbmi2 avx512_fp16 amx_tile amx_int8 amx_bf16 cldemote" \
    " movdiri movdir64b serialize tsxldtrk avx512_vp2intersect"

#define WSP_FLAGS_ZEN4  "fpu vme de pse tsc msr pae mce cx8 apic sep" \
    " clflush mmx fxsr sse sse2 ht syscall nx lm constant_tsc rep_good" \
    " nopl cpuid pni pclmulqdq monitor ssse3 fma cx16 sse4_1 sse4_2" \
    " movbe popcnt aes xsave avx f16c rdrand lahf_lm cmp_legacy svm" \
    " cr8_legacy abm sse4a misalignsse 3dnowprefetch osvw ibs" \
    " perfctr_ibp zbrv26 perfctr_llc mwaitx cpb cat_l3 cdp_l3" \
    " hw_pstate ssbd ibrs ibpb stibp vmmcall fsgsbase bmi1 avx2 smep" \
    " bmi2 erms invpcid cqm rdt_a avx512f avx512dq rdseed adx smap" \
    " clflushopt clwb avx512cd avx512bw avx512vl avx512_bf16" \
    " clzero wbnoinvd arat avx512_vnni avx_vnni gfni vaes" \
    " vpclmulqdq rdpid lbrv svm_lock nrip_save tsc_scale"

/* ------------------------------------------------------------------ */
/* Archetype evidence tables                                           */
/* ------------------------------------------------------------------ */

/* 1. Apple M4 Max (MacBook Pro, 16 P-cores, 64 GiB unified). */
static const wsp_kv_t WSP_ENV_M4MAX_SYSCTL[] = {
    { "hw.machine",                 "Mac16,9" },
    { "hw.cachelinesize",           "128" },
    { "hw.memsize",                 "68719476736" },
    { "hw.perflevel0.logicalcpu",   "16" },
    { "hw.perflevel1.logicalcpu",   "0" },
    { "kern.osrelease",             "24.1.0" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_M4MAX = {
    WEFT_ARCHETYPE_APPLE_M4_MAX, "apple-m4-max",
    WEFT_PROBE_ARCH_ARM64, 0, 0, 16,
    NULL, WSP_ENV_M4MAX_SYSCTL, NULL,
    0, 0, NULL, 0
};

/* 2. Apple A17 Pro (iPhone 15 Pro, 2P+4E, 8 GiB). */
static const wsp_kv_t WSP_ENV_A17_SYSCTL[] = {
    { "hw.machine",                 "iPhone15,2" },
    { "hw.cachelinesize",           "128" },
    { "hw.memsize",                 "8589934592" },
    { "hw.perflevel0.logicalcpu",   "2" },
    { "hw.perflevel1.logicalcpu",   "4" },
    { "kern.osrelease",             "23.0.0" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_A17 = {
    WEFT_ARCHETYPE_APPLE_A17_PRO, "apple-a17-pro",
    WEFT_PROBE_ARCH_ARM64, 0, 0, 6,
    NULL, WSP_ENV_A17_SYSCTL, NULL,
    0, 0, NULL, 0
};

/* 3. Snapdragon 8 Gen 3 (1x X4 + 5x A720 + 2x A520, 16 GB). */
#define WSP_CPUINFO_SD8G3 \
    "processor\t: 0\nCPU part\t: 0x00d4e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 1\nCPU part\t: 0x00d4e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 2\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 3\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 4\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 5\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 6\nCPU part\t: 0x00d01\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 7\nCPU part\t: 0x00d01\nFeatures\t: " WSP_FEAT_PHONE "\n\n"
static const wsp_kv_t WSP_ENV_SD8G3_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_SD8G3 },
    { "/proc/meminfo",
      "MemTotal:       15583040 kB\nMemFree:         8123400 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "0" },
    { "/sys/devices/system/node/online", "0" },
    { "/sys/devices/system/node/node0/cpulist", "0-7" },
    { "/proc/sys/kernel/osrelease", "6.1.75-android14-8-gen3" },
    { "/dev/dri/renderD128", "yes" },
    { "/dev/fastrpc-adsp", "yes" },
    { "/dev/fastrpc-cdsp", "yes" },
    { "/sys/class/power_supply/battery", "Li-ion" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_kv_t WSP_ENV_SD8G3_PROPS[] = {
    { "ro.board.platform", "kalama" },
    WSP_KV_END
};
static const uint32_t WSP_FREQ_SD8G3[] = {
    3400000u, 3200000u, 3200000u, 3200000u, 3200000u, 3200000u,
    2800000u, 2800000u
};
static const wsp_mock_env_t WSP_ENV_SD8G3 = {
    WEFT_ARCHETYPE_SNAPDRAGON_8GEN3, "snapdragon-8gen3",
    WEFT_PROBE_ARCH_ARM64,
    (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
        (1u << 7) | (1u << 8),         /* ASIMD|AES|PMULL|SHA1|SHA2|CRC32|ATOM */
    0, 8,
    WSP_ENV_SD8G3_FILES, NULL, WSP_ENV_SD8G3_PROPS,
    0, 300000u, WSP_FREQ_SD8G3, 8
};

/* 4. MediaTek Dimensity 9300 (4x X4 + 4x A720, 16 GB, Neuropilot). */
#define WSP_CPUINFO_D93 \
    "processor\t: 0\nCPU part\t: 0x00d4e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 1\nCPU part\t: 0x00d4e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 2\nCPU part\t: 0x00d4e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 3\nCPU part\t: 0x00d4e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 4\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 5\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 6\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 7\nCPU part\t: 0x00d80\nFeatures\t: " WSP_FEAT_PHONE "\n\n"
static const wsp_kv_t WSP_ENV_D93_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_D93 },
    { "/proc/meminfo",
      "MemTotal:       15805800 kB\nMemFree:         9210400 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "0" },
    { "/sys/devices/system/node/online", "0" },
    { "/sys/devices/system/node/node0/cpulist", "0-7" },
    { "/proc/sys/kernel/osrelease", "6.1.29-android14-d93" },
    { "/dev/dri/renderD128", "yes" },
    { "/vendor/lib64/libneuropilot.so", "elf" },
    { "/sys/class/power_supply/battery", "Li-ion" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_kv_t WSP_ENV_D93_PROPS[] = {
    { "ro.board.platform", "mt6989" },
    WSP_KV_END
};
static const uint32_t WSP_FREQ_D93[] = {
    3250000u, 3250000u, 3250000u, 3250000u,
    2000000u, 2000000u, 2000000u, 2000000u
};
static const wsp_mock_env_t WSP_ENV_D93 = {
    WEFT_ARCHETYPE_DIMENSITY_9300, "dimensity-9300",
    WEFT_PROBE_ARCH_ARM64,
    (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
        (1u << 7) | (1u << 8),
    0, 8,
    WSP_ENV_D93_FILES, NULL, WSP_ENV_D93_PROPS,
    0, 325000u, WSP_FREQ_D93, 8
};

/* 5. MediaTek Helio G88 budget phone (2x A75 + 6x A55, 4 GB). */
#define WSP_CPUINFO_G88 \
    "processor\t: 0\nCPU part\t: 0x00d09\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 1\nCPU part\t: 0x00d09\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 2\nCPU part\t: 0x00d05\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 3\nCPU part\t: 0x00d05\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 4\nCPU part\t: 0x00d05\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 5\nCPU part\t: 0x00d05\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 6\nCPU part\t: 0x00d05\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 7\nCPU part\t: 0x00d05\nFeatures\t: " WSP_FEAT_PHONE "\n\n"
static const wsp_kv_t WSP_ENV_G88_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_G88 },
    { "/proc/meminfo",
      "MemTotal:        3768116 kB\nMemFree:         1912244 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "0" },
    { "/sys/devices/system/node/online", "0" },
    { "/sys/devices/system/node/node0/cpulist", "0-7" },
    { "/proc/sys/kernel/osrelease", "5.10.198-android12-g88" },
    { "/dev/dri/renderD128", "yes" },
    { "/sys/class/power_supply/battery", "Li-poly" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_kv_t WSP_ENV_G88_PROPS[] = {
    { "ro.board.platform", "mt6765" },
    WSP_KV_END
};
static const uint32_t WSP_FREQ_G88[] = {
    2050000u, 2050000u,
    1800000u, 1800000u, 1800000u, 1800000u, 1800000u, 1800000u
};
static const wsp_mock_env_t WSP_ENV_G88 = {
    WEFT_ARCHETYPE_HELIO_G88, "helio-g88",
    WEFT_PROBE_ARCH_ARM64,
    (1u << 1) | (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8),
    0, 8,
    WSP_ENV_G88_FILES, NULL, WSP_ENV_G88_PROPS,
    0, 1000000u, WSP_FREQ_G88, 8
};

/* 6. Raspberry Pi 5 (4x Cortex-A76 @ 2.4 GHz, 8 GB, VideoCore VII). */
#define WSP_CPUINFO_PI5 \
    "processor\t: 0\nCPU part\t: 0x00d0e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 1\nCPU part\t: 0x00d0e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 2\nCPU part\t: 0x00d0e\nFeatures\t: " WSP_FEAT_PHONE "\n\n" \
    "processor\t: 3\nCPU part\t: 0x00d0e\nFeatures\t: " WSP_FEAT_PHONE "\n\n"
static const wsp_kv_t WSP_ENV_PI5_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_PI5 },
    { "/proc/meminfo",
      "MemTotal:        7800120 kB\nMemFree:         5981100 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "0" },
    { "/sys/devices/system/node/online", "0" },
    { "/sys/devices/system/node/node0/cpulist", "0-3" },
    { "/proc/sys/kernel/osrelease", "6.6.31-v8-16k+" },
    { "/dev/dri/renderD128", "yes" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_PI5 = {
    WEFT_ARCHETYPE_RASPBERRY_PI_5, "raspberry-pi-5",
    WEFT_PROBE_ARCH_ARM64,
    (1u << 1) | (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8),
    0, 4,
    WSP_ENV_PI5_FILES, NULL, NULL,
    2400000u, 600000u, NULL, 0
};

/* 7. StarFive VisionFive 2 (4x SiFive U74 rv64imafdc — no RVV). */
#define WSP_CPUINFO_VF2 \
    "processor\t: 0\nisa\t\t: rv64imafdc\nmmu\t\t: sv39\n\n" \
    "processor\t: 1\nisa\t\t: rv64imafdc\nmmu\t\t: sv39\n\n" \
    "processor\t: 2\nisa\t\t: rv64imafdc\nmmu\t\t: sv39\n\n" \
    "processor\t: 3\nisa\t\t: rv64imafdc\nmmu\t\t: sv39\n\n"
static const wsp_kv_t WSP_ENV_VF2_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_VF2 },
    { "/proc/meminfo",
      "MemTotal:        7895468 kB\nMemFree:         6031200 kB\n" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "0" },
    { "/sys/devices/system/node/online", "0" },
    { "/sys/devices/system/node/node0/cpulist", "0-3" },
    { "/proc/sys/kernel/osrelease", "5.15.0-starfive2" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_VF2 = {
    WEFT_ARCHETYPE_VISIONFIVE_2, "visionfive-2",
    WEFT_PROBE_ARCH_RISCV64,
    (1u << 8) | (1u << 13) | (1u << 0) | (1u << 5) | (1u << 3) |
        (1u << 2),                       /* I M A F D C letters, no V */
    0, 4,
    WSP_ENV_VF2_FILES, NULL, NULL,
    0, 0, NULL, 0
};

/* 8. AMD EPYC 9654 Genoa (96C/192T AVX-512, 8 NUMA nodes, dGPU). */
#define WSP_CPUINFO_EPYC \
    "processor\t: 0\nvendor_id\t: AuthenticAMD\n" \
    "flags\t\t: " WSP_FLAGS_GENOA "\n\n" \
    "processor\t: 1\nvendor_id\t: AuthenticAMD\n" \
    "flags\t\t: " WSP_FLAGS_GENOA "\n\n"
static const wsp_kv_t WSP_ENV_EPYC_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_EPYC },
    { "/proc/meminfo",
      "MemTotal:      772903328 kB\nMemFree:      701200544 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list",
      "0,96" },
    { "/sys/devices/system/node/online", "0-7" },
    { "/sys/devices/system/node/node0/cpulist", "0-11,96-107" },
    { "/sys/devices/system/node/node1/cpulist", "12-23,108-119" },
    { "/sys/devices/system/node/node2/cpulist", "24-35,120-131" },
    { "/sys/devices/system/node/node3/cpulist", "36-47,132-143" },
    { "/sys/devices/system/node/node4/cpulist", "48-59,144-155" },
    { "/sys/devices/system/node/node5/cpulist", "60-71,156-167" },
    { "/sys/devices/system/node/node6/cpulist", "72-83,168-179" },
    { "/sys/devices/system/node/node7/cpulist", "84-95,180-191" },
    { "/proc/sys/kernel/osrelease", "6.1.0-13-amd64" },
    { "/dev/dri/renderD128", "yes" },
    { "/sys/class/drm/card0/device/class", "0x030000" },
    { "/sys/class/drm/card0/device/vendor", "0x1002" },
    { "/sys/class/drm/card0/device/mem_info_vram_total", "17179869184" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_EPYC = {
    WEFT_ARCHETYPE_EPYC_9654, "epyc-9654-genoa",
    WEFT_PROBE_ARCH_X86_64, 0, 0, 96,
    WSP_ENV_EPYC_FILES, NULL, NULL,
    3700000u, 2150000u, NULL, 0
};

/* 9. Intel Xeon w9-3495X Sapphire Rapids (56C/112T AMX, 2 NUMA). */
#define WSP_CPUINFO_SPR \
    "processor\t: 0\nvendor_id\t: GenuineIntel\n" \
    "flags\t\t: " WSP_FLAGS_SPR "\n\n" \
    "processor\t: 1\nvendor_id\t: GenuineIntel\n" \
    "flags\t\t: " WSP_FLAGS_SPR "\n\n"
static const wsp_kv_t WSP_ENV_SPR_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_SPR },
    { "/proc/meminfo",
      "MemTotal:      264412160 kB\nMemFree:      240012800 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list",
      "0,56" },
    { "/sys/devices/system/node/online", "0-1" },
    { "/sys/devices/system/node/node0/cpulist", "0-27,56-83" },
    { "/sys/devices/system/node/node1/cpulist", "28-55,84-111" },
    { "/proc/sys/kernel/osrelease", "5.14.0-spr" },
    { "/dev/dri/renderD128", "yes" },
    { "/sys/class/drm/card0/device/class", "0x030000" },
    { "/sys/class/drm/card0/device/vendor", "0x10de" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_SPR = {
    WEFT_ARCHETYPE_XEON_SPR_AMX, "xeon-sapphire-rapids-amx",
    WEFT_PROBE_ARCH_X86_64, 0, 0, 56,
    WSP_ENV_SPR_FILES, NULL, NULL,
    4800000u, 800000u, NULL, 0
};

/* 10. Nvidia Grace Hopper (72x Neoverse V2 SVE2, 480 GB LPDDR5X). */
#define WSP_CPUINFO_GH \
    "processor\t: 0\nCPU part\t: 0x00d4f\nFeatures\t: " WSP_FEAT_GH "\n\n" \
    "processor\t: 1\nCPU part\t: 0x00d4f\nFeatures\t: " WSP_FEAT_GH "\n\n" \
    "processor\t: 2\nCPU part\t: 0x00d4f\nFeatures\t: " WSP_FEAT_GH "\n\n" \
    "processor\t: 3\nCPU part\t: 0x00d4f\nFeatures\t: " WSP_FEAT_GH "\n\n"
static const wsp_kv_t WSP_ENV_GH_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_GH },
    { "/proc/meminfo",
      "MemTotal:     491284480 kB\nMemFree:     470001200 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/node/online", "0" },
    { "/sys/devices/system/node/node0/cpulist", "0-71" },
    { "/proc/sys/kernel/osrelease", "6.5.0-cmmx" },
    { "/dev/dri/renderD128", "yes" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_GH = {
    WEFT_ARCHETYPE_GRACE_HOPPER, "grace-hopper",
    WEFT_PROBE_ARCH_ARM64,
    (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
        (1u << 7) | (1u << 8) | (1u << 22),   /* + HWCAP_SVE            */
    (1u << 1) | (1u << 12) | (1u << 13),      /* SVE2 | SVEBF16 | I8MM  */
    72,
    WSP_ENV_GH_FILES, NULL, NULL,
    0, 0, NULL, 0
};

/* 11. Desktop Ryzen 5 7600 + RX 7900 XTX (6C/12T AVX-512, 128 GB). */
#define WSP_CPUINFO_ZEN4 \
    "processor\t: 0\nvendor_id\t: AuthenticAMD\n" \
    "flags\t\t: " WSP_FLAGS_ZEN4 "\n\n" \
    "processor\t: 1\nvendor_id\t: AuthenticAMD\n" \
    "flags\t\t: " WSP_FLAGS_ZEN4 "\n\n"
static const wsp_kv_t WSP_ENV_ZEN4_FILES[] = {
    { "/proc/cpuinfo", WSP_CPUINFO_ZEN4 },
    { "/proc/meminfo",
      "MemTotal:     131330024 kB\nMemFree:     100021200 kB\n" },
    { "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
      "64" },
    { "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list",
      "0,6" },
    { "/sys/devices/system/node/online", "0" },
    { "/sys/devices/system/node/node0/cpulist", "0-5" },
    { "/proc/sys/kernel/osrelease", "6.8.0-amd64" },
    { "/dev/dri/renderD128", "yes" },
    { "/sys/class/drm/card0/device/class", "0x030000" },
    { "/sys/class/drm/card0/device/vendor", "0x1002" },
    { "/sys/class/drm/card0/device/mem_info_vram_total", "25769803776" },
    { "/sys/fs/cgroup/memory.max", "max" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_ZEN4 = {
    WEFT_ARCHETYPE_DESKTOP_ZEN4, "desktop-zen4-rx7900xtx",
    WEFT_PROBE_ARCH_X86_64, 0, 0, 6,
    WSP_ENV_ZEN4_FILES, NULL, NULL,
    5300000u, 3000000u, NULL, 0
};

/* 12. Containerized CI runner (/proc hidden, 8 GiB cgroup ceiling). */
static const wsp_kv_t WSP_ENV_CT_FILES[] = {
    { "/sys/fs/cgroup/memory.max", "8589934592" },
    WSP_KV_END
};
static const wsp_mock_env_t WSP_ENV_CT = {
    WEFT_ARCHETYPE_CONTAINER_FALLBACK, "container-fallback",
    WEFT_PROBE_ARCH_X86_64, 0, 0, 4,
    WSP_ENV_CT_FILES, NULL, NULL,
    0, 0, NULL, 0
};

static const wsp_mock_env_t *const WSP_ENVS[] = {
    &WSP_ENV_M4MAX, &WSP_ENV_A17,  &WSP_ENV_SD8G3, &WSP_ENV_D93,
    &WSP_ENV_G88,   &WSP_ENV_PI5,  &WSP_ENV_VF2,   &WSP_ENV_EPYC,
    &WSP_ENV_SPR,   &WSP_ENV_GH,   &WSP_ENV_ZEN4,  &WSP_ENV_CT
};

#define WSP_ENV_COUNT (sizeof(WSP_ENVS) / sizeof(WSP_ENVS[0]))

/* ------------------------------------------------------------------ */
/* Mock source implementation                                          */
/* ------------------------------------------------------------------ */

static const wsp_mock_env_t *wsp_mock_env = NULL;

static const wsp_kv_t *wsp_kv_find(const wsp_kv_t *table, const char *key)
{
    if (table == NULL) {
        return NULL;
    }
    for (; table->k != NULL; table++) {
        if (strcmp(table->k, key) == 0) {
            return table;
        }
    }
    return NULL;
}

static int wsp_kv_copy(const char *v, char *buf, size_t cap)
{
    size_t n = strlen(v);
    if (cap < 2u) {
        return -1;
    }
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    memcpy(buf, v, n);
    buf[n] = '\0';
    return (int)n;
}

/* Decimal u64 -> string (synthesized cpufreq values). */
static int wsp_kv_copy_dec(char *buf, size_t cap, uint64_t v)
{
    char tmp[24];
    size_t n = 0, i;
    if (cap < 2u) {
        return -1;
    }
    if (v == 0ull) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    while (v > 0ull && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n && i + 1u < cap; i++) {
        buf[i] = tmp[n - 1u - i];
    }
    buf[i] = '\0';
    return (int)i;
}

/* Synthesize "/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_{max,min}_freq"
 * from the env's frequency model. */
static int wsp_mock_cpufreq(const char *path, char *buf, size_t cap)
{
    static const char prefix[] = "/sys/devices/system/cpu/cpu";
    static const char maxsfx[] = "/cpufreq/cpuinfo_max_freq";
    static const char minsfx[] = "/cpufreq/cpuinfo_min_freq";
    const size_t plen = sizeof(prefix) - 1u;
    uint32_t cpu = 0;
    size_t i, restlen;
    const char *rest;
    int is_max, is_min;

    if (strncmp(path, prefix, plen) != 0) {
        return -1;
    }
    i = plen;
    if (path[i] < '0' || path[i] > '9') {
        return -1;
    }
    while (path[i] >= '0' && path[i] <= '9') {
        cpu = cpu * 10u + (uint32_t)(path[i] - '0');
        i++;
        if (cpu > 4096u) {
            return -1;
        }
    }
    rest = path + i;
    restlen = strlen(rest);
    is_max = (restlen == sizeof(maxsfx) - 1u) &&
             (memcmp(rest, maxsfx, restlen) == 0);
    is_min = (restlen == sizeof(minsfx) - 1u) &&
             (memcmp(rest, minsfx, restlen) == 0);
    if (!is_max && !is_min) {
        return -1;
    }
    if (wsp_mock_env == NULL) {
        return -1;
    }
    if (is_max) {
        uint32_t f = wsp_mock_env->uniform_freq_khz;
        if (wsp_mock_env->percpu_freq_khz != NULL &&
            cpu < wsp_mock_env->percpu_freq_count) {
            f = wsp_mock_env->percpu_freq_khz[cpu];
        }
        if (f == 0u) {
            return -1;
        }
        return wsp_kv_copy_dec(buf, cap, (uint64_t)f);
    }
    /* cpuinfo_min_freq is only read for cpu0. */
    if (cpu != 0u || wsp_mock_env->uniform_min_freq_khz == 0u) {
        return -1;
    }
    return wsp_kv_copy_dec(buf, cap,
                           (uint64_t)wsp_mock_env->uniform_min_freq_khz);
}

static uint64_t wsp_mock_auxv(uint32_t type)
{
    if (wsp_mock_env == NULL) {
        return 0u;
    }
    if (type == 16u) { /* AT_HWCAP */
        return wsp_mock_env->hwcap;
    }
    if (type == 26u) { /* AT_HWCAP2 */
        return wsp_mock_env->hwcap2;
    }
    return 0u;
}

static int wsp_mock_sysctl(const char *name, char *buf, size_t cap)
{
    const wsp_kv_t *kv;
    int rc;
    if (wsp_mock_env == NULL) {
        return -1;
    }
    kv = wsp_kv_find(wsp_mock_env->sysctls, name);
    if (kv == NULL) {
        return -1;
    }
    rc = wsp_kv_copy(kv->v, buf, cap);
    return (rc < 0) ? -1 : 0; /* contract: 0 on success */
}

static int wsp_mock_sysprop(const char *key, char *buf, size_t cap)
{
    const wsp_kv_t *kv;
    int rc;
    if (wsp_mock_env == NULL) {
        return -1;
    }
    kv = wsp_kv_find(wsp_mock_env->props, key);
    if (kv == NULL) {
        return -1;
    }
    rc = wsp_kv_copy(kv->v, buf, cap);
    return (rc < 0) ? -1 : 0; /* contract: 0 on success */
}

static int wsp_mock_read_file(const char *path, char *buf, size_t cap)
{
    const wsp_kv_t *kv;
    if (wsp_mock_env == NULL) {
        return -1;
    }
    kv = wsp_kv_find(wsp_mock_env->files, path);
    if (kv != NULL) {
        return wsp_kv_copy(kv->v, buf, cap);
    }
    return wsp_mock_cpufreq(path, buf, cap);
}

static int wsp_mock_file_exists(const char *path)
{
    if (wsp_mock_env == NULL) {
        return 0;
    }
    return wsp_kv_find(wsp_mock_env->files, path) != NULL ? 1 : 0;
}

static long wsp_mock_sysconf(int name)
{
    (void)name;
    if (wsp_mock_env == NULL) {
        return -1;
    }
    return wsp_mock_env->ncpu;
}

static uint32_t wsp_mock_arch_hint(void)
{
    if (wsp_mock_env == NULL) {
        return WEFT_PROBE_ARCH_UNKNOWN;
    }
    return wsp_mock_env->arch;
}

static uint64_t wsp_mock_now_ns(void)
{
    return 0u; /* frozen deterministic clock */
}

static const weft_probe_source_t WSP_MOCK_SOURCE = {
    .auxv        = wsp_mock_auxv,
    .sysctl      = wsp_mock_sysctl,
    .sysprop     = wsp_mock_sysprop,
    .read_file   = wsp_mock_read_file,
    .file_exists = wsp_mock_file_exists,
    .sysconf     = wsp_mock_sysconf,
    .arch_hint   = wsp_mock_arch_hint,
    .now_ns      = wsp_mock_now_ns,
};

/* ------------------------------------------------------------------ */
/* Public test surface                                                 */
/* ------------------------------------------------------------------ */

weft_spectrum_status_t weft_mock_archetype_select(uint32_t archetype_id)
{
    size_t i;
    for (i = 0; i < WSP_ENV_COUNT; i++) {
        if ((uint32_t)WSP_ENVS[i]->id == archetype_id) {
            wsp_mock_env = WSP_ENVS[i];
            weft_probe_set_source(&WSP_MOCK_SOURCE);
            return WEFT_SPECTRUM_OK;
        }
    }
    return WEFT_SPECTRUM_ENOARCHETYPE;
}

const char *weft_mock_archetype_name(uint32_t archetype_id)
{
    size_t i;
    for (i = 0; i < WSP_ENV_COUNT; i++) {
        if ((uint32_t)WSP_ENVS[i]->id == archetype_id) {
            return WSP_ENVS[i]->name;
        }
    }
    return "unknown";
}

void weft_mock_archetype_clear(void)
{
    wsp_mock_env = NULL;
    weft_probe_set_source(NULL);
}
