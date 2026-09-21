// weft_hw_probe.c — WSP5 zero-alloc micro-architecture probing engine.
//
// Deterministic, fail-closed runtime probing across Linux
// (/proc/cpuinfo, getauxval/AT_HWCAP, sysfs, cgroups), Darwin
// (sysctlbyname), Android (system properties + the Linux paths) and
// bare-metal / containerized environments where every rich source may
// be denied.
//
// Architecture (D-51 §4): every OS touch is funneled through the
// weft_probe_source_t vtable. The real per-OS source (Linux / Darwin /
// generic stub) is the default; the test harness swaps in deterministic
// mock sources for the 12 golden archetypes so that the REAL probe
// pipeline — parsing, capability mapping, topology math, sealing — runs
// end-to-end and is byte-frozen into golden ABI fixtures.
//
// Honesty rules (normative, Law 4):
//   - A capability bit is SET only on positive evidence: an auxv bit,
//     a cpuinfo token present in EVERY matching stanza (intersection
//     semantics — heterogeneous hybrids fail closed), a readable sysfs
//     value, an existing device node, or a documented platform fact
//     (Apple ANE/AMX/unified memory, Metal compute on Darwin).
//   - A capability that cannot be verified from pure C (Vulkan
//     timeline semaphores, async compute queues, timestamp queries,
//     display refresh) stays CLEAR for the driver layer to promote.
//   - Unreadable sources degrade conservatively (cache line 64B
//     baseline, RAM 0, scalar SIMD) and clear their WEFT_SRC_* bit.
//   - The probe itself allocates NOTHING: 8 KiB stack buffer for
//     cpuinfo, 64-byte stack strings for names, zero libc stdio.

#include "weft_spectrum_internal.h"

#include <string.h>
#include <stdio.h>   /* snprintf only — never stdio buffered I/O */

#if defined(__linux__)
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <time.h>
#  if defined(__GLIBC__) || defined(__ANDROID__)
#    include <sys/auxv.h>
#    define WSP_HAVE_GETAUXVAL 1
#  endif
#  if !defined(AT_HWCAP)
#    define AT_HWCAP 16
#  endif
#  if !defined(AT_HWCAP2)
#    define AT_HWCAP2 26
#  endif
#  if defined(__aarch64__)
#    include <sys/prctl.h>
#    ifndef PR_SVE_GET_VL
#      define PR_SVE_GET_VL 61
#    endif
#  endif
#elif defined(__APPLE__)
#  include <unistd.h>
#  include <fcntl.h>
#  include <time.h>
#  include <sys/sysctl.h>
#else
#  include <unistd.h>
#  include <time.h>
#endif

/* ------------------------------------------------------------------ */
/* Kernel hwcap constants (public ABI values, defined locally so the  */
/* interpretation matrix compiles on every host).                      */
/* ------------------------------------------------------------------ */

/* arm64 (Linux uapi asm/hwcap.h). */
#define WSP_HWCAP_ARM64_ASIMD    (1u << 1)
#define WSP_HWCAP_ARM64_AES      (1u << 3)
#define WSP_HWCAP_ARM64_CRC32    (1u << 7)
#define WSP_HWCAP_ARM64_ATOMICS  (1u << 8)
#define WSP_HWCAP_ARM64_SVE      (1u << 22)
#define WSP_HWCAP2_ARM64_SVE2    (1u << 1)
#define WSP_HWCAP2_ARM64_SVEBF16 (1u << 12)

/* riscv64: letter bitmask, V = 1 << ('V' - 'A'). */
#define WSP_HWCAP_RISCV_ISA_V    (1u << 21)

/* ------------------------------------------------------------------ */
/* Frozen integrity primitives                                        */
/* ------------------------------------------------------------------ */

/* Decimal u64 -> string (Darwin numeric sysctls; no stdio needed).
 * Only referenced by the Darwin source below. */
#if defined(__APPLE__)
static void wsp_u64_to_dec(char *buf, size_t cap, uint64_t v)
{
    char tmp[24];
    size_t n = 0, i;
    if (cap < 2u) { if (cap == 1u) buf[0] = '\0'; return; }
    if (v == 0ull) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v > 0ull && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n && i + 1 < cap; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[i] = '\0';
}
#endif

/* CRC-32C (Castagnoli, reflected, poly 0x1EDC6F41) lookup table —
 * a compile-time .rodata literal: deterministic on every host, no
 * lazy init, no heap. Generated offline; the L3 test ladder pins the
 * reference check value 0xE3069283 for "123456789". */
static const uint32_t WSP_CRC32C_TABLE[256] = {
    0x00000000u, 0xF26B8303u, 0xE13B70F7u, 0x1350F3F4u, 0xC79A971Fu, 0x35F1141Cu,
    0x26A1E7E8u, 0xD4CA64EBu, 0x8AD958CFu, 0x78B2DBCCu, 0x6BE22838u, 0x9989AB3Bu,
    0x4D43CFD0u, 0xBF284CD3u, 0xAC78BF27u, 0x5E133C24u, 0x105EC76Fu, 0xE235446Cu,
    0xF165B798u, 0x030E349Bu, 0xD7C45070u, 0x25AFD373u, 0x36FF2087u, 0xC494A384u,
    0x9A879FA0u, 0x68EC1CA3u, 0x7BBCEF57u, 0x89D76C54u, 0x5D1D08BFu, 0xAF768BBCu,
    0xBC267848u, 0x4E4DFB4Bu, 0x20BD8EDEu, 0xD2D60DDDu, 0xC186FE29u, 0x33ED7D2Au,
    0xE72719C1u, 0x154C9AC2u, 0x061C6936u, 0xF477EA35u, 0xAA64D611u, 0x580F5512u,
    0x4B5FA6E6u, 0xB93425E5u, 0x6DFE410Eu, 0x9F95C20Du, 0x8CC531F9u, 0x7EAEB2FAu,
    0x30E349B1u, 0xC288CAB2u, 0xD1D83946u, 0x23B3BA45u, 0xF779DEAEu, 0x05125DADu,
    0x1642AE59u, 0xE4292D5Au, 0xBA3A117Eu, 0x4851927Du, 0x5B016189u, 0xA96AE28Au,
    0x7DA08661u, 0x8FCB0562u, 0x9C9BF696u, 0x6EF07595u, 0x417B1DBCu, 0xB3109EBFu,
    0xA0406D4Bu, 0x522BEE48u, 0x86E18AA3u, 0x748A09A0u, 0x67DAFA54u, 0x95B17957u,
    0xCBA24573u, 0x39C9C670u, 0x2A993584u, 0xD8F2B687u, 0x0C38D26Cu, 0xFE53516Fu,
    0xED03A29Bu, 0x1F682198u, 0x5125DAD3u, 0xA34E59D0u, 0xB01EAA24u, 0x42752927u,
    0x96BF4DCCu, 0x64D4CECFu, 0x77843D3Bu, 0x85EFBE38u, 0xDBFC821Cu, 0x2997011Fu,
    0x3AC7F2EBu, 0xC8AC71E8u, 0x1C661503u, 0xEE0D9600u, 0xFD5D65F4u, 0x0F36E6F7u,
    0x61C69362u, 0x93AD1061u, 0x80FDE395u, 0x72966096u, 0xA65C047Du, 0x5437877Eu,
    0x4767748Au, 0xB50CF789u, 0xEB1FCBADu, 0x197448AEu, 0x0A24BB5Au, 0xF84F3859u,
    0x2C855CB2u, 0xDEEEDFB1u, 0xCDBE2C45u, 0x3FD5AF46u, 0x7198540Du, 0x83F3D70Eu,
    0x90A324FAu, 0x62C8A7F9u, 0xB602C312u, 0x44694011u, 0x5739B3E5u, 0xA55230E6u,
    0xFB410CC2u, 0x092A8FC1u, 0x1A7A7C35u, 0xE811FF36u, 0x3CDB9BDDu, 0xCEB018DEu,
    0xDDE0EB2Au, 0x2F8B6829u, 0x82F63B78u, 0x709DB87Bu, 0x63CD4B8Fu, 0x91A6C88Cu,
    0x456CAC67u, 0xB7072F64u, 0xA457DC90u, 0x563C5F93u, 0x082F63B7u, 0xFA44E0B4u,
    0xE9141340u, 0x1B7F9043u, 0xCFB5F4A8u, 0x3DDE77ABu, 0x2E8E845Fu, 0xDCE5075Cu,
    0x92A8FC17u, 0x60C37F14u, 0x73938CE0u, 0x81F80FE3u, 0x55326B08u, 0xA759E80Bu,
    0xB4091BFFu, 0x466298FCu, 0x1871A4D8u, 0xEA1A27DBu, 0xF94AD42Fu, 0x0B21572Cu,
    0xDFEB33C7u, 0x2D80B0C4u, 0x3ED04330u, 0xCCBBC033u, 0xA24BB5A6u, 0x502036A5u,
    0x4370C551u, 0xB11B4652u, 0x65D122B9u, 0x97BAA1BAu, 0x84EA524Eu, 0x7681D14Du,
    0x2892ED69u, 0xDAF96E6Au, 0xC9A99D9Eu, 0x3BC21E9Du, 0xEF087A76u, 0x1D63F975u,
    0x0E330A81u, 0xFC588982u, 0xB21572C9u, 0x407EF1CAu, 0x532E023Eu, 0xA145813Du,
    0x758FE5D6u, 0x87E466D5u, 0x94B49521u, 0x66DF1622u, 0x38CC2A06u, 0xCAA7A905u,
    0xD9F75AF1u, 0x2B9CD9F2u, 0xFF56BD19u, 0x0D3D3E1Au, 0x1E6DCDEEu, 0xEC064EEDu,
    0xC38D26C4u, 0x31E6A5C7u, 0x22B65633u, 0xD0DDD530u, 0x0417B1DBu, 0xF67C32D8u,
    0xE52CC12Cu, 0x1747422Fu, 0x49547E0Bu, 0xBB3FFD08u, 0xA86F0EFCu, 0x5A048DFFu,
    0x8ECEE914u, 0x7CA56A17u, 0x6FF599E3u, 0x9D9E1AE0u, 0xD3D3E1ABu, 0x21B862A8u,
    0x32E8915Cu, 0xC083125Fu, 0x144976B4u, 0xE622F5B7u, 0xF5720643u, 0x07198540u,
    0x590AB964u, 0xAB613A67u, 0xB831C993u, 0x4A5A4A90u, 0x9E902E7Bu, 0x6CFBAD78u,
    0x7FAB5E8Cu, 0x8DC0DD8Fu, 0xE330A81Au, 0x115B2B19u, 0x020BD8EDu, 0xF0605BEEu,
    0x24AA3F05u, 0xD6C1BC06u, 0xC5914FF2u, 0x37FACCF1u, 0x69E9F0D5u, 0x9B8273D6u,
    0x88D28022u, 0x7AB90321u, 0xAE7367CAu, 0x5C18E4C9u, 0x4F48173Du, 0xBD23943Eu,
    0xF36E6F75u, 0x0105EC76u, 0x12551F82u, 0xE03E9C81u, 0x34F4F86Au, 0xC69F7B69u,
    0xD5CF889Du, 0x27A40B9Eu, 0x79B737BAu, 0x8BDCB4B9u, 0x988C474Du, 0x6AE7C44Eu,
    0xBE2DA0A5u, 0x4C4623A6u, 0x5F16D052u, 0xAD7D5351u,
};

uint32_t weft_crc32c(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < len; i++) {
        crc = (crc >> 8) ^
              WSP_CRC32C_TABLE[(crc ^ (uint32_t)p[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

uint64_t weft_fnv1a64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint64_t)(unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Real source: Linux (also Android — the same sysfs/proc surface)    */
/* ------------------------------------------------------------------ */

#if defined(__linux__)

static uint64_t wsp_linux_auxv(uint32_t type)
{
#if defined(WSP_HAVE_GETAUXVAL)
    errno = 0;
    unsigned long v = getauxval((unsigned long)type);
    if (v == 0ul && errno != 0) {
        return 0u; /* entry absent — honest unavailable */
    }
    return (uint64_t)v;
#else
    /* Portable fallback: parse /proc/self/auxv (Elf64 pairs). */
    char buf[1024];
    int fd = open("/proc/self/auxv", O_RDONLY | O_CLOEXEC);
    size_t total = 0;
    if (fd < 0) {
        return 0u;
    }
    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total == sizeof(buf)) break;
    }
    close(fd);
    {
        size_t i;
        for (i = 0; i + 16 <= total; i += 16) {
            uint64_t a_type, a_val;
            memcpy(&a_type, buf + i, 8);
            memcpy(&a_val, buf + i + 8, 8);
            if (a_type == (uint64_t)type) {
                return a_val;
            }
            if (a_type == 0u) break; /* AT_NULL */
        }
    }
    return 0u;
#endif
}

static int wsp_linux_read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t total = 0;
    if (fd < 0) {
        return -1;
    }
    while (total + 1 < cap) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    return (int)total;
}

static int wsp_linux_file_exists(const char *path)
{
    return faccessat(AT_FDCWD, path, F_OK, 0) == 0 ? 1 : 0;
}

static long wsp_linux_sysconf(int name)
{
    return sysconf(name);
}

static int wsp_linux_sysctl(const char *name, char *buf, size_t cap)
{
    /* No sysctlbyname(3) on Linux: /proc/sys fallback for the few keys
     * the Darwin path shares (kernel release). */
    if (strcmp(name, "kern.osrelease") == 0) {
        return wsp_linux_read_file("/proc/sys/kernel/osrelease", buf, cap);
    }
    (void)buf; (void)cap;
    return -1;
}

static int wsp_linux_sysprop(const char *key, char *buf, size_t cap)
{
    (void)key; (void)buf; (void)cap;
    return -1; /* Android builds use the property source below. */
}

#if defined(__ANDROID__)
extern int __system_property_get(const char *, char *); /* bionic */
static int wsp_android_sysprop(const char *key, char *buf, size_t cap)
{
    if (cap < 92u) {
        return -1;
    }
    if (__system_property_get(key, buf) <= 0) {
        return -1;
    }
    return 0;
}
#define WSP_SYSPROP_IMPL wsp_android_sysprop
#else
#define WSP_SYSPROP_IMPL wsp_linux_sysprop
#endif

static uint32_t wsp_linux_arch_hint(void)
{
#if defined(__x86_64__) || defined(__i386__)
    return WEFT_PROBE_ARCH_X86_64;
#elif defined(__aarch64__)
    return WEFT_PROBE_ARCH_ARM64;
#elif defined(__riscv) && (__riscv_xlen == 64)
    return WEFT_PROBE_ARCH_RISCV64;
#else
    return WEFT_PROBE_ARCH_UNKNOWN;
#endif
}

static uint64_t wsp_linux_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const weft_probe_source_t wsp_linux_source = {
    .auxv       = wsp_linux_auxv,
    .sysctl     = wsp_linux_sysctl,
    .sysprop    = WSP_SYSPROP_IMPL,
    .read_file  = wsp_linux_read_file,
    .file_exists= wsp_linux_file_exists,
    .sysconf    = wsp_linux_sysconf,
    .arch_hint  = wsp_linux_arch_hint,
    .now_ns     = wsp_linux_now_ns,
};

#define WSP_DEFAULT_SOURCE (&wsp_linux_source)

/* ------------------------------------------------------------------ */
/* Real source: Darwin (macOS / iOS)                                  */
/* ------------------------------------------------------------------ */

#elif defined(__APPLE__)

static uint64_t wsp_darwin_auxv(uint32_t type)
{
    (void)type;
    return 0u; /* no auxv on Darwin — every cap comes from sysctl */
}

static int wsp_darwin_sysctl(const char *name, char *buf, size_t cap)
{
    /* String-typed keys return NUL-terminated values directly. */
    if (strcmp(name, "hw.machine") == 0 ||
        strcmp(name, "kern.osrelease") == 0) {
        size_t len = cap;
        if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
            return -1;
        }
        if (len > 0 && len < cap) {
            buf[len - 1] = '\0';
        } else if (cap > 0) {
            buf[cap - 1] = '\0';
        }
        return 0;
    }
    /* Numeric-typed keys: copy out the integer, render decimal. */
    if (strcmp(name, "hw.memsize") == 0 ||
        strcmp(name, "hw.cachelinesize") == 0 ||
        strcmp(name, "hw.perflevel0.logicalcpu") == 0 ||
        strcmp(name, "hw.perflevel1.logicalcpu") == 0) {
        uint64_t v64 = 0;
        size_t n = sizeof(v64);
        if (sysctlbyname(name, &v64, &n, NULL, 0) != 0) {
            return -1;
        }
        if (n == 4u) {
            uint32_t v32;
            memcpy(&v32, &v64, sizeof(v32));
            v64 = (uint64_t)v32;
        } else if (n != 8u) {
            return -1;
        }
        wsp_u64_to_dec(buf, cap, v64);
        return 0;
    }
    return -1;
}

static int wsp_darwin_sysprop(const char *key, char *buf, size_t cap)
{
    (void)key; (void)buf; (void)cap;
    return -1;
}

static int wsp_darwin_read_file(const char *path, char *buf, size_t cap)
{
    (void)path; (void)buf; (void)cap;
    return -1; /* no /proc on Darwin */
}

static int wsp_darwin_file_exists(const char *path)
{
    return access(path, F_OK) == 0 ? 1 : 0;
}

static long wsp_darwin_sysconf(int name)
{
    return sysconf(name);
}

static uint32_t wsp_darwin_arch_hint(void)
{
#  if defined(__aarch64__)
    return WEFT_PROBE_ARCH_ARM64;
#  else
    return WEFT_PROBE_ARCH_X86_64;
#  endif
}

static uint64_t wsp_darwin_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const weft_probe_source_t wsp_darwin_source = {
    .auxv       = wsp_darwin_auxv,
    .sysctl     = wsp_darwin_sysctl,
    .sysprop    = wsp_darwin_sysprop,
    .read_file  = wsp_darwin_read_file,
    .file_exists= wsp_darwin_file_exists,
    .sysconf    = wsp_darwin_sysconf,
    .arch_hint  = wsp_darwin_arch_hint,
    .now_ns     = wsp_darwin_now_ns,
};

#define WSP_DEFAULT_SOURCE (&wsp_darwin_source)

/* ------------------------------------------------------------------ */
/* Real source: generic / bare-metal fallback — everything denied     */
/* except sysconf where the platform provides it.                     */
/* ------------------------------------------------------------------ */

#else

static uint64_t wsp_generic_auxv(uint32_t type) { (void)type; return 0u; }
static int wsp_generic_sysctl(const char *n, char *b, size_t c)
{ (void)n; (void)b; (void)c; return -1; }
static int wsp_generic_sysprop(const char *k, char *b, size_t c)
{ (void)k; (void)b; (void)c; return -1; }
static int wsp_generic_read_file(const char *p, char *b, size_t c)
{ (void)p; (void)b; (void)c; return -1; }
static int wsp_generic_file_exists(const char *p) { (void)p; return 0; }
static long wsp_generic_sysconf(int name) { return sysconf(name); }
static uint32_t wsp_generic_arch_hint(void) { return WEFT_PROBE_ARCH_UNKNOWN; }
static uint64_t wsp_generic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const weft_probe_source_t wsp_generic_source = {
    .auxv       = wsp_generic_auxv,
    .sysctl     = wsp_generic_sysctl,
    .sysprop    = wsp_generic_sysprop,
    .read_file  = wsp_generic_read_file,
    .file_exists= wsp_generic_file_exists,
    .sysconf    = wsp_generic_sysconf,
    .arch_hint  = wsp_generic_arch_hint,
    .now_ns     = wsp_generic_now_ns,
};

#define WSP_DEFAULT_SOURCE (&wsp_generic_source)

#endif /* per-OS real sources */

/* ------------------------------------------------------------------ */
/* Source selection (init-time only — see internal header contract)   */
/* ------------------------------------------------------------------ */

static const weft_probe_source_t *wsp_active_source = NULL;

void weft_probe_set_source(const weft_probe_source_t *source)
{
    wsp_active_source = (source != NULL) ? source : WSP_DEFAULT_SOURCE;
}

const weft_probe_source_t *weft_probe_get_source(void)
{
    if (wsp_active_source == NULL) {
        wsp_active_source = WSP_DEFAULT_SOURCE;
    }
    return wsp_active_source;
}

/* ------------------------------------------------------------------ */
/* Bounded, zero-alloc parsing helpers                                */
/* ------------------------------------------------------------------ */

/* Does span [s, s+len) contain `token` as a whitespace-separated field? */
static int wsp_span_has_token(const char *s, size_t len, const char *token)
{
    size_t tlen = strlen(token);
    size_t i = 0;
    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') i++;
        if (i > start) {
            size_t flen = i - start;
            if (flen == tlen && memcmp(s + start, token, tlen) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* Is this cpuinfo line a `<key><ws>:` stanza header for `key`? */
static int wsp_line_is_keyed(const char *line, size_t len, const char *key)
{
    size_t klen = strlen(key);
    size_t i = 0;
    if (len <= klen || memcmp(line, key, klen) != 0) {
        return 0;
    }
    i = klen;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] != ':') {
        return 0;
    }
    return 1;
}

/* Token present in EVERY keyed stanza of a cpuinfo blob? (intersection
 * semantics: a hybrid where any core lacks the feature fails closed).
 * Returns 0 unless at least one stanza exists. */
static int wsp_cpuinfo_token_all(const char *buf, const char *key,
                                 const char *token)
{
    int saw_stanza = 0;
    const char *p = buf;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        size_t ll = (size_t)(eol - p);
        if (wsp_line_is_keyed(p, ll, key)) {
            saw_stanza = 1;
            if (!wsp_span_has_token(p, ll, token)) {
                return 0;
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return saw_stanza;
}

/* Parse a decimal u64 from a span; returns 0 on failure. */
static int wsp_parse_u64_span(const char *s, size_t len, uint64_t *out)
{
    uint64_t v = 0;
    size_t i = 0;
    int saw = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10u + (uint64_t)(s[i] - '0');
        saw = 1;
        if (v > 0x00FFFFFFFFFFFFFFull) v = 0x00FFFFFFFFFFFFFFull; /* clamp */
    }
    if (!saw) return 0;
    *out = v;
    return 1;
}

/* Decimal u64 -> string — see the definition before the per-OS
 * sources; this stub keeps the parse-helper section self-documenting. */

/* First keyed-stanza integer value (e.g. MemTotal: N kB) — the
 * number is parsed AFTER the key's colon, never from line start. */
static int wsp_cpuinfo_keyed_u64(const char *buf, const char *key,
                                 uint64_t *out)
{
    const char *p = buf;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        size_t ll = (size_t)(eol - p);
        if (wsp_line_is_keyed(p, ll, key)) {
            size_t i = 0;
            while (i < ll && p[i] != ':') i++;
            if (i < ll) {
                return wsp_parse_u64_span(p + i + 1, ll - i - 1, out);
            }
            return 0;
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return 0;
}

/* Count distinct keyed-stanza hex values (arm64 "CPU part" hetero
 * evidence). Bounded to 8 distinct classes. */
static uint32_t wsp_cpuinfo_distinct_parts(const char *buf)
{
    uint32_t seen[8];
    uint32_t nseen = 0;
    const char *p = buf;
    while (*p && nseen < 8u) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        size_t ll = (size_t)(eol - p);
        if (wsp_line_is_keyed(p, ll, "CPU part")) {
            uint64_t v = 0;
            size_t i = 0;
            int any = 0;
            while (i < ll && p[i] != ':') i++;
            i++;                                    /* past ':'        */
            while (i < ll && (p[i] == ' ' || p[i] == '\t')) i++;
            if (i + 1 < ll && p[i] == '0' &&
                (p[i + 1] == 'x' || p[i + 1] == 'X')) {
                i += 2;                             /* skip 0x prefix  */
            }
            for (; i < ll; i++) {
                char c = p[i];
                uint32_t d;
                if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
                else break;
                v = (v << 4) | d;
                any = 1;
            }
            if (any) {
                uint32_t j;
                int dup = 0;
                for (j = 0; j < nseen; j++) {
                    if (seen[j] == (uint32_t)v) { dup = 1; break; }
                }
                if (!dup && nseen < 8u) {
                    seen[nseen++] = (uint32_t)v;
                }
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return nseen;
}

/* Range-list ("0-7", "0-11,96-107", "0") → integer count (bounded). */
static uint32_t wsp_range_list_count(const char *s)
{
    uint32_t count = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == ',') s++;
        if (!*s) break;
        {
            uint64_t lo = 0, hi = 0;
            int any = 0;
            while (*s >= '0' && *s <= '9') {
                lo = lo * 10u + (uint64_t)(*s - '0');
                hi = lo;
                any = 1;
                s++;
                if (lo > 1048576ull) return count; /* refuse garbage */
            }
            if (!any) break;
            if (*s == '-') {
                s++;
                hi = 0;
                while (*s >= '0' && *s <= '9') {
                    hi = hi * 10u + (uint64_t)(*s - '0');
                    s++;
                    if (hi > 1048576ull) return count;
                }
                if (hi < lo) return count; /* refuse inverted range */
            }
            if (hi - lo + 1 > 1048576ull) return count;
            count += (uint32_t)(hi - lo + 1u);
        }
    }
    return count;
}

/* Range-list → low-32-bit CPU mask (entries beyond bit 31 dropped —
 * documented topology-summary truncation, D-51 §2). */
static uint32_t wsp_range_list_mask(const char *s)
{
    uint32_t mask = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == ',') s++;
        if (!*s) break;
        {
            uint64_t lo = 0, hi = 0;
            int any = 0;
            while (*s >= '0' && *s <= '9') {
                lo = lo * 10u + (uint64_t)(*s - '0');
                hi = lo;
                any = 1;
                s++;
                if (lo > 1048576ull) return mask;
            }
            if (!any) break;
            if (*s == '-') {
                s++;
                hi = 0;
                while (*s >= '0' && *s <= '9') {
                    hi = hi * 10u + (uint64_t)(*s - '0');
                    s++;
                    if (hi > 1048576ull) return mask;
                }
                if (hi < lo) return mask;
            }
            if (lo > 31ull) continue; /* beyond the summary mask */
            {
                uint32_t blo = (uint32_t)lo;
                uint32_t bhi = (hi > 31ull) ? 31u : (uint32_t)hi;
                uint32_t k;
                for (k = blo; k <= bhi; k++) {
                    mask |= (1u << k);
                }
            }
        }
    }
    return mask;
}

/* Parse leading "MAJOR.MINOR" from an osrelease string. */
static int wsp_parse_kernel_major(const char *s, uint32_t *major)
{
    uint64_t v = 0;
    int saw = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint64_t)(*s - '0');
        saw = 1;
        s++;
        if (v > 4096ull) v = 4096ull;
    }
    if (!saw) return 0;
    *major = (uint32_t)v;
    return 1;
}

/* Set a capability bit by feature id (internal, bounds-checked). */
static void wsp_set_feature(weft_hw_profile_t *p, uint32_t fid)
{
    if (fid < (uint32_t)WEFT_FEATURE_COUNT) {
        p->caps[fid >> 6] |= (1ull << (fid & 63u));
    }
}

/* ------------------------------------------------------------------ */
/* Probe stages                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char cpuinfo[8192];
    char machine[64];      /* Darwin hw.machine / evidence string       */
    char platform[64];     /* Android ro.board.platform                */
    char osrelease[64];
    int  cpuinfo_len;
    int  have_cpuinfo;
    int  have_machine;     /* sysctl hw.machine answered (Darwin)       */
    int  have_platform;    /* sysprop ro.board.platform answered        */
    int  have_osrelease;
    uint32_t kernel_major;
    int  android;
    int  apple;
    int  apple_arm64;
    uint64_t t0;
} wsp_probe_ctx_t;

/* --- stage: CPU ISA (word 0) per interpretation arch ---------------- */

static const struct { const char *tok; uint32_t fid; } WSP_X86_TOKENS[] = {
    { "avx2",          WEFT_F_CPU_AVX2 },
    { "avx512f",       WEFT_F_CPU_AVX512F },
    { "avx512_bf16",   WEFT_F_CPU_AVX512_BF16 },
    { "avx_vnni",      WEFT_F_CPU_AVX_VNNI },
    { "avx512_vnni",   WEFT_F_CPU_AVX_VNNI },
    { "amx_tile",      WEFT_F_CPU_AMX },
    { "pclmulqdq",     WEFT_F_CPU_CLMUL },
    { "sse4_2",        WEFT_F_CPU_CRC32 },
    { "aes",           WEFT_F_CPU_AES },
};
#define WSP_X86_TOKEN_COUNT \
    (sizeof(WSP_X86_TOKENS) / sizeof(WSP_X86_TOKENS[0]))

static const struct { const char *tok; uint32_t fid; } WSP_ARM64_TOKENS[] = {
    { "asimd",   WEFT_F_CPU_NEON },
    { "crc32",   WEFT_F_CPU_CRC32 },
    { "atomics", WEFT_F_CPU_LSE_ATOMICS },
    { "aes",     WEFT_F_CPU_AES },
    { "pmull",   WEFT_F_CPU_CLMUL },
    { "sve2",    WEFT_F_CPU_SVE2 },
    { "svebf16", WEFT_F_CPU_SVE2_BF16 },
};
#define WSP_ARM64_TOKEN_COUNT \
    (sizeof(WSP_ARM64_TOKENS) / sizeof(WSP_ARM64_TOKENS[0]))

static void wsp_probe_isa_x86(weft_hw_profile_t *p, wsp_probe_ctx_t *ctx,
                              uint16_t *sources)
{
    size_t i;
    wsp_set_feature(p, WEFT_F_CPU_TSO); /* x86 TSO is architectural */
    if (!ctx->have_cpuinfo) {
        return;
    }
    *sources |= (uint16_t)WEFT_SRC_CPUINFO;
    for (i = 0; i < WSP_X86_TOKEN_COUNT; i++) {
        if (wsp_cpuinfo_token_all(ctx->cpuinfo, "flags",
                                  WSP_X86_TOKENS[i].tok)) {
            wsp_set_feature(p, WSP_X86_TOKENS[i].fid);
        }
    }
    /* SIMD register width from the verified intersection. */
    if (weft_hw_has_feature(p, WEFT_F_CPU_AVX512F)) {
        p->simd_max_bits = 512u;
    } else if (weft_hw_has_feature(p, WEFT_F_CPU_AVX2)) {
        p->simd_max_bits = 256u;
    } else if (wsp_cpuinfo_token_all(ctx->cpuinfo, "flags", "sse2")) {
        p->simd_max_bits = 128u;
    }
}

static void wsp_probe_isa_arm64(weft_hw_profile_t *p,
                                const weft_probe_source_t *src,
                                wsp_probe_ctx_t *ctx, uint16_t *sources)
{
    uint64_t hwcap = 0, hwcap2 = 0;
    size_t i;

    hwcap = src->auxv(AT_HWCAP);
    if (hwcap != 0u) {
        *sources |= (uint16_t)WEFT_SRC_AUXV;
        if (hwcap & WSP_HWCAP_ARM64_ASIMD)   wsp_set_feature(p, WEFT_F_CPU_NEON);
        if (hwcap & WSP_HWCAP_ARM64_AES)     wsp_set_feature(p, WEFT_F_CPU_AES);
        if (hwcap & WSP_HWCAP_ARM64_CRC32)   wsp_set_feature(p, WEFT_F_CPU_CRC32);
        if (hwcap & WSP_HWCAP_ARM64_ATOMICS) wsp_set_feature(p, WEFT_F_CPU_LSE_ATOMICS);
    }
    hwcap2 = src->auxv(AT_HWCAP2);
    if (hwcap2 != 0u) {
        if (hwcap2 & WSP_HWCAP2_ARM64_SVE2)    wsp_set_feature(p, WEFT_F_CPU_SVE2);
        if (hwcap2 & WSP_HWCAP2_ARM64_SVEBF16) wsp_set_feature(p, WEFT_F_CPU_SVE2_BF16);
    }

    if (ctx->have_cpuinfo) {
        *sources |= (uint16_t)WEFT_SRC_CPUINFO;
        for (i = 0; i < WSP_ARM64_TOKEN_COUNT; i++) {
            if (wsp_cpuinfo_token_all(ctx->cpuinfo, "Features",
                                      WSP_ARM64_TOKENS[i].tok)) {
                wsp_set_feature(p, WSP_ARM64_TOKENS[i].fid);
            }
        }
    }

    if (weft_hw_has_feature(p, WEFT_F_CPU_NEON) ||
        weft_hw_has_feature(p, WEFT_F_CPU_SVE2)) {
        p->simd_max_bits = 128u; /* architectural minimum for the ISA */
    }
#if defined(__aarch64__) && defined(__linux__)
    if (weft_hw_has_feature(p, WEFT_F_CPU_SVE2)) {
        /* Host is real arm64: query the thread SVE vector length
         * (bytes, PR_SVE_VL_LEN_MASK = 0xffff). On foreign hosts
         * (mock archetypes on x86) this syscall is skipped and the
         * profile reports the architectural minimum — honestly. */
        long vl = prctl(PR_SVE_GET_VL, 0ul, 0ul, 0ul, 0ul);
        if (vl > 0) {
            uint64_t bits = ((uint64_t)(vl & 0xffff)) * 8u;
            if (bits >= 128u && bits <= 2048u) {
                p->simd_max_bits = (uint32_t)bits;
            }
        }
    }
#endif

    /* Apple platform facts (documented in D-51 §4.3, not sysctl-
     * verifiable): every Apple arm64 SoC ships NEON/ASIMD (ARMv8
     * architectural baseline), the AMX matrix coprocessor and the
     * Apple Neural Engine with a zero-copy arena. */
    if (ctx->apple && ctx->apple_arm64) {
        wsp_set_feature(p, WEFT_F_CPU_NEON);
        if (p->simd_max_bits < 128u) {
            p->simd_max_bits = 128u;
        }
        wsp_set_feature(p, WEFT_F_CPU_AMX);
        wsp_set_feature(p, WEFT_F_NPU_PRESENT);
        wsp_set_feature(p, WEFT_F_NPU_APPLE_ANE);
        wsp_set_feature(p, WEFT_F_NPU_ZERO_COPY_ARENA);
        p->npu_channels = 1u;
    }
}

static void wsp_probe_isa_riscv(weft_hw_profile_t *p,
                                const weft_probe_source_t *src,
                                wsp_probe_ctx_t *ctx, uint16_t *sources)
{
    uint64_t hwcap = src->auxv(AT_HWCAP);
    if (hwcap != 0u) {
        *sources |= (uint16_t)WEFT_SRC_AUXV;
        if (hwcap & WSP_HWCAP_RISCV_ISA_V) {
            wsp_set_feature(p, WEFT_F_CPU_RVV_1_0);
            p->simd_max_bits = 128u; /* RVV mandates VLEN >= 128 */
        }
    }
    if (ctx->have_cpuinfo) {
        *sources |= (uint16_t)WEFT_SRC_CPUINFO;
        /* riscv cpuinfo: "isa\t\t: rv64imafdcv" — token scan for "v"
         * suffix spelling "_v" would misfire on "rv64imafdcv" whole
         * token, so we probe the canonical token forms explicitly. */
        if (wsp_cpuinfo_token_all(ctx->cpuinfo, "isa", "rv64imafdcv") ||
            wsp_cpuinfo_token_all(ctx->cpuinfo, "isa", "rv64gcv") ||
            wsp_cpuinfo_token_all(ctx->cpuinfo, "isa", "rv64imafdcvb")) {
            wsp_set_feature(p, WEFT_F_CPU_RVV_1_0);
            p->simd_max_bits = 128u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Hex parser (PCI class/vendor files are "0x030000\n")               */
/* ------------------------------------------------------------------ */

static int wsp_parse_hex64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    int saw = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | (uint64_t)d;
        saw = 1;
        s++;
        if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
    }
    if (!saw) return 0;
    *out = v;
    return 1;
}

/* --- stage: topology / cores / SMT / boost (word 1) ------------------ */

static void wsp_probe_topology(weft_hw_profile_t *p,
                               const weft_probe_source_t *src,
                               wsp_probe_ctx_t *ctx, uint16_t *sources)
{
    char buf[128];
    long ncpu = src->sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t total;

    if (ncpu > 0 && ncpu <= 1024) {
        total = (uint32_t)ncpu;
        *sources |= (uint16_t)WEFT_SRC_SYSCONF;
    } else {
        total = 0; /* honest unknown */
    }

    /* Darwin performance clusters come from perflevel sysctls. */
    if (ctx->apple) {
        uint64_t p0 = 0, p1 = 0;
        if (src->sysctl("hw.perflevel0.logicalcpu", buf, sizeof(buf)) == 0) {
            (void)wsp_parse_u64_span(buf, strlen(buf), &p0);
        }
        if (src->sysctl("hw.perflevel1.logicalcpu", buf, sizeof(buf)) == 0) {
            (void)wsp_parse_u64_span(buf, strlen(buf), &p1);
        }
        if (p0 > 0ull || p1 > 0ull) {
            *sources |= (uint16_t)WEFT_SRC_TOPOLOGY;
            p->cores_total = (uint16_t)(total ? total : (p0 + p1));
            p->cores_performance = (uint16_t)(p0 ? p0 : total);
            p->cores_efficiency = (uint16_t)(p1 & 0xFFFFull);
            if (p0 > 0ull && p1 > 0ull) {
                wsp_set_feature(p, WEFT_F_CPU_HETERO_CORES);
            }
            return;
        }
    }

    /* Linux: bucket cores by cpufreq cpuinfo_max_freq. Cores at the
     * GLOBAL MINIMUM hardware max form the efficiency cluster; every
     * faster core is performance. Homogeneous machines collapse to a
     * single bucket (perf == total, no hetero claim). Bounded scan of
     * the first 256 cores. */
    {
        uint32_t freqs[256];
        uint32_t scanned = 0;
        int have_freq = 0;
        int i;
        for (i = 0; i < 256; i++) {
            uint64_t fmax = 0;
            char path[96];
            if (total && (uint32_t)i >= total) break;
            if (snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
                         i) < 0) {
                break;
            }
            if (src->read_file(path, buf, sizeof(buf)) < 0) {
                continue;
            }
            if (!wsp_parse_u64_span(buf, strlen(buf), &fmax)) {
                continue;
            }
            have_freq = 1;
            *sources |= (uint16_t)WEFT_SRC_CPUFREQ;
            freqs[scanned % 256u] = (uint32_t)(fmax > 0xFFFFFFFFull
                                               ? 0xFFFFFFFFull : fmax);
            scanned++;
        }
        if (have_freq && scanned > 0u) {
            uint32_t gmin = freqs[0];
            uint32_t eff = 0, k;
            for (k = 1; k < scanned && k < 256u; k++) {
                if (freqs[k] < gmin) gmin = freqs[k];
            }
            for (k = 0; k < scanned && k < 256u; k++) {
                if (freqs[k] == gmin) eff++;
            }
            /* Homogeneous fleet (every core at the same hardware max):
             * no efficiency cluster exists — fail-closed to a single
             * performance bucket rather than an inverted split. */
            if (eff == scanned) {
                eff = 0u;
            }
            *sources |= (uint16_t)WEFT_SRC_TOPOLOGY;
            p->cores_total = (uint16_t)(total ? total : scanned);
            p->cores_performance = (uint16_t)(scanned - eff);
            p->cores_efficiency = (uint16_t)eff;
            if (eff > 0u && eff < scanned) {
                wsp_set_feature(p, WEFT_F_CPU_HETERO_CORES);
            }
            /* Boost headroom from cpu0's verified min/max pair. */
            {
                uint64_t fmax0 = 0, fmin0 = 0;
                if (src->read_file("/sys/devices/system/cpu/cpu0/cpufreq/"
                                   "cpuinfo_max_freq", buf, sizeof(buf)) >= 0 &&
                    wsp_parse_u64_span(buf, strlen(buf), &fmax0)) {
                    if (src->read_file("/sys/devices/system/cpu/cpu0/cpufreq/"
                                       "cpuinfo_min_freq", buf, sizeof(buf)) >= 0) {
                        (void)wsp_parse_u64_span(buf, strlen(buf), &fmin0);
                    }
                    if (fmax0 > fmin0) {
                        uint64_t mhz = (fmax0 - fmin0) / 1000u;
                        if (mhz <= 65535ull) {
                            p->boost_headroom_mhz = (uint32_t)mhz;
                            wsp_set_feature(p, WEFT_F_CPU_FREQ_BOOST);
                        }
                    }
                }
            }
        } else {
            /* No cpufreq evidence: no hetero split, conservative. */
            p->cores_total = (uint16_t)total;
            p->cores_performance = (uint16_t)total;
            p->cores_efficiency = 0;
            /* arm64 secondary evidence: distinct CPU parts. */
            if (ctx->have_cpuinfo &&
                wsp_cpuinfo_distinct_parts(ctx->cpuinfo) >= 2u) {
                *sources |= (uint16_t)WEFT_SRC_TOPOLOGY;
                wsp_set_feature(p, WEFT_F_CPU_HETERO_CORES);
            }
        }
    }
}

/* SMT evidence from thread siblings (Linux). */
static void wsp_probe_smt(weft_hw_profile_t *p, const weft_probe_source_t *src,
                          uint16_t *sources)
{
    char buf[128];
    if (src->read_file("/sys/devices/system/cpu/cpu0/topology/"
                       "thread_siblings_list", buf, sizeof(buf)) >= 0) {
        if (wsp_range_list_count(buf) > 1u) {
            *sources |= (uint16_t)WEFT_SRC_TOPOLOGY;
            wsp_set_feature(p, WEFT_F_CPU_SMT);
        }
    }
}

/* --- stage: cache line ---------------------------------------------- */

static void wsp_probe_cacheline(weft_hw_profile_t *p,
                                const weft_probe_source_t *src,
                                wsp_probe_ctx_t *ctx, uint16_t *sources)
{
    char buf[64];
    uint64_t v = 0;
    int ok = 0;

    if (src->read_file("/sys/devices/system/cpu/cpu0/cache/index0/"
                       "coherency_line_size", buf, sizeof(buf)) >= 0) {
        ok = wsp_parse_u64_span(buf, strlen(buf), &v);
    }
    if (!ok && ctx->apple) {
        if (src->sysctl("hw.cachelinesize", buf, sizeof(buf)) == 0) {
            ok = wsp_parse_u64_span(buf, strlen(buf), &v);
        }
    }
    if (ok && (v == 32ull || v == 64ull || v == 128ull)) {
        *sources |= (uint16_t)WEFT_SRC_CACHELINE;
        p->cache_line_size = (uint32_t)v;
        if (v == 128ull) wsp_set_feature(p, WEFT_F_CPU_128B_CACHELINE);
        if (v == 32ull)  wsp_set_feature(p, WEFT_F_CPU_32B_CACHELINE);
    } else {
        /* Conservative documented baseline (fail-closed: 128B stays a
         * positively-verified claim only). */
        p->cache_line_size = 64u;
    }
}

/* --- stage: NUMA ----------------------------------------------------- */

static void wsp_probe_numa(weft_hw_profile_t *p, const weft_probe_source_t *src,
                           uint16_t *sources)
{
    char buf[128];
    if (src->read_file("/sys/devices/system/node/online", buf, sizeof(buf)) >= 0) {
        uint32_t nodes = wsp_range_list_count(buf);
        if (nodes >= 1u && nodes <= WEFT_SPECTRUM_MAX_NUMA_NODES) {
            uint32_t i;
            *sources |= (uint16_t)WEFT_SRC_NUMA;
            p->numa_node_count = (uint16_t)nodes;
            if (nodes > 1u) {
                wsp_set_feature(p, WEFT_F_CPU_MULTI_NUMA);
            }
            for (i = 0; i < nodes; i++) {
                char path[96];
                if (snprintf(path, sizeof(path),
                             "/sys/devices/system/node/node%u/cpulist",
                             (unsigned)i) < 0) {
                    break;
                }
                if (src->read_file(path, buf, sizeof(buf)) >= 0) {
                    p->numa_cpu_mask[i] = wsp_range_list_mask(buf);
                }
            }
            return;
        }
    }
    p->numa_node_count = 1u; /* conservative baseline, source bit clear */
}

/* --- stage: RAM + cgroup --------------------------------------------- */

static void wsp_probe_ram(weft_hw_profile_t *p, const weft_probe_source_t *src,
                          wsp_probe_ctx_t *ctx, uint16_t *sources)
{
    char buf[128];
    uint64_t mem_total = 0, cgroup = 0;
    int have_total = 0, have_cgroup = 0;

    if (ctx->apple) {
        if (src->sysctl("hw.memsize", buf, sizeof(buf)) == 0) {
            have_total = wsp_parse_u64_span(buf, strlen(buf), &mem_total);
        }
    } else {
        char membuf[256];
        if (src->read_file("/proc/meminfo", membuf, sizeof(membuf)) >= 0) {
            uint64_t kb = 0;
            if (wsp_cpuinfo_keyed_u64(membuf, "MemTotal", &kb)) {
                mem_total = kb * 1024ull;
                have_total = 1;
            }
        }
    }
    if (have_total) {
        *sources |= (uint16_t)WEFT_SRC_MEMINFO;
    }

    /* cgroup v2 then v1 ceilings. */
    {
        char cg[64];
        if (src->read_file("/sys/fs/cgroup/memory.max", cg, sizeof(cg)) >= 0) {
            uint64_t v = 0;
            if (cg[0] != 'm' && wsp_parse_u64_span(cg, strlen(cg), &v)) {
                cgroup = v;
                have_cgroup = 1;
            }
        } else if (src->read_file("/sys/fs/cgroup/memory/memory.limit_in_bytes",
                                  cg, sizeof(cg)) >= 0) {
            uint64_t v = 0;
            if (wsp_parse_u64_span(cg, strlen(cg), &v) && v > 0ull &&
                v < 0x00FFFFFFFFFFFFFFull) {
                cgroup = v;
                have_cgroup = 1;
            }
        }
    }

    if (have_total) {
        p->ram_total_bytes = mem_total;
        if (have_cgroup && cgroup > 0ull && cgroup < mem_total) {
            *sources |= (uint16_t)WEFT_SRC_CGROUP_MEM;
            p->ram_available_bytes = cgroup;
            wsp_set_feature(p, WEFT_F_SYS_CONTAINERIZED);
        } else {
            p->ram_available_bytes = mem_total;
        }
    } else if (have_cgroup) {
        /* Container with hidden /proc: the cgroup ceiling is the only
         * honest memory statement available. */
        *sources |= (uint16_t)WEFT_SRC_CGROUP_MEM;
        p->ram_total_bytes = cgroup;
        p->ram_available_bytes = cgroup;
        wsp_set_feature(p, WEFT_F_SYS_CONTAINERIZED);
    }

    if (p->ram_total_bytes > 0ull && p->ram_total_bytes < 6442450944ull) {
        wsp_set_feature(p, WEFT_F_SYS_LOW_RAM);
    }
}

/* --- stage: GPU (word 2) ---------------------------------------------- */

static void wsp_probe_gpu(weft_hw_profile_t *p, const weft_probe_source_t *src,
                          wsp_probe_ctx_t *ctx, uint16_t *sources)
{
    char buf[64];

    if (ctx->apple) {
        /* Every Darwin target ships a GPU (platform fact); Metal
         * compute is available on all of them; Metal 3 argument
         * buffers arrive with Darwin kernel >= 22 on arm64. */
        wsp_set_feature(p, WEFT_F_GPU_PRESENT);
        wsp_set_feature(p, WEFT_F_GPU_COMPUTE_SHADER);
        p->gpu_engines = 1u;
        if (ctx->apple_arm64) {
            wsp_set_feature(p, WEFT_F_GPU_UNIFIED_MEMORY);
        }
        if (ctx->apple_arm64 && ctx->have_osrelease &&
            ctx->kernel_major >= 22u) {
            wsp_set_feature(p, WEFT_F_GPU_METAL3_ARG_BUFFERS);
        }
        *sources |= (uint16_t)WEFT_SRC_SYSCTL;
        return;
    }

    /* Linux: DRM render nodes. */
    {
        int r, render_nodes = 0;
        for (r = 128; r < 132; r++) {
            char path[48];
            if (snprintf(path, sizeof(path), "/dev/dri/renderD%d", r) < 0) {
                break;
            }
            if (src->file_exists(path)) {
                render_nodes++;
            }
        }
        if (render_nodes == 0) {
            return; /* fail-closed: no GPU evidence */
        }
        *sources |= (uint16_t)WEFT_SRC_DRM;
        wsp_set_feature(p, WEFT_F_GPU_PRESENT);
        p->gpu_engines = (uint16_t)render_nodes;

        /* Android SoC evidence: unified system memory. */
        if (ctx->android) {
            wsp_set_feature(p, WEFT_F_GPU_UNIFIED_MEMORY);
        }

        /* dma-buf import path: render node + kernel >= 4. */
        if (ctx->have_osrelease && ctx->kernel_major >= 4u) {
            wsp_set_feature(p, WEFT_F_GPU_DMABUF_IMPORT);
            *sources |= (uint16_t)WEFT_SRC_KERNEL_REL;
        }

        /* Discrete VRAM evidence: PCI class 0x03xx devices.
         * Discrete is claimed for NVIDIA vendor 0x10de, or AMD 0x1002
         * with verified mem_info_vram_total >= 2 GiB. Intel (0x8086)
         * stays unset (iGPU/Arc indistinguishable without the driver
         * layer) — fail-closed, promoted by Engineer 2. */
        {
            int c;
            for (c = 0; c < 4; c++) {
                char path[96];
                uint64_t class_id = 0, vendor = 0, vram = 0;
                if (snprintf(path, sizeof(path),
                             "/sys/class/drm/card%d/device/class", c) < 0) {
                    break;
                }
                if (src->read_file(path, buf, sizeof(buf)) < 0) continue;
                if (!wsp_parse_hex64(buf, &class_id)) continue;
                if ((class_id >> 16ull) != 0x03ull) continue; /* display */
                if (snprintf(path, sizeof(path),
                             "/sys/class/drm/card%d/device/vendor", c) < 0) {
                    break;
                }
                if (src->read_file(path, buf, sizeof(buf)) < 0) continue;
                if (!wsp_parse_hex64(buf, &vendor)) continue;

                if (vendor == 0x10deull) {
                    wsp_set_feature(p, WEFT_F_GPU_DISCRETE_VRAM);
                    continue;
                }
                if (vendor == 0x1002ull) {
                    if (snprintf(path, sizeof(path),
                                 "/sys/class/drm/card%d/device/"
                                 "mem_info_vram_total", c) < 0) {
                        break;
                    }
                    if (src->read_file(path, buf, sizeof(buf)) >= 0 &&
                        wsp_parse_u64_span(buf, strlen(buf), &vram) &&
                        vram >= 2147483648ull) {
                        wsp_set_feature(p, WEFT_F_GPU_DISCRETE_VRAM);
                        p->gpu_memory_bytes = vram;
                    }
                }
            }
        }
    }
}

/* --- stage: NPU (word 3) ---------------------------------------------- */

static void wsp_probe_npu(weft_hw_profile_t *p, const weft_probe_source_t *src,
                          uint16_t *sources)
{
    /* Qualcomm Hexagon FastRPC channels: positive device-node evidence. */
    int channels = 0;
    if (src->file_exists("/dev/fastrpc-adsp")) channels++;
    if (src->file_exists("/dev/fastrpc-cdsp")) channels++;
    if (channels > 0) {
        *sources |= (uint16_t)WEFT_SRC_FASTRPC;
        wsp_set_feature(p, WEFT_F_NPU_PRESENT);
        wsp_set_feature(p, WEFT_F_NPU_HEXAGON_FASTRPC);
        wsp_set_feature(p, WEFT_F_NPU_ZERO_COPY_ARENA);
        if (channels > 1) {
            wsp_set_feature(p, WEFT_F_NPU_MULTI_CHANNEL);
        }
        p->npu_channels = (uint16_t)channels;
    }

    /* MediaTek Neuropilot runtime: positive library evidence. */
    if (src->file_exists("/vendor/lib64/libneuropilot.so") ||
        src->file_exists("/vendor/lib/libneuropilot.so") ||
        src->file_exists("/odm/lib64/libneuropilot.so")) {
        *sources |= (uint16_t)WEFT_SRC_NEUROPILOT;
        wsp_set_feature(p, WEFT_F_NPU_PRESENT);
        wsp_set_feature(p, WEFT_F_NPU_MTK_NEUROPILOT);
        wsp_set_feature(p, WEFT_F_NPU_ZERO_COPY_ARENA);
        if (p->npu_channels == 0u) {
            p->npu_channels = 1u;
        }
    }
}

/* --- stage: thermal class --------------------------------------------- */

static void wsp_probe_thermal(weft_hw_profile_t *p,
                              const weft_probe_source_t *src,
                              wsp_probe_ctx_t *ctx, uint16_t *sources)
{
    if (src->file_exists("/sys/class/power_supply/BAT0") ||
        src->file_exists("/sys/class/power_supply/BAT1") ||
        src->file_exists("/sys/class/power_supply/battery")) {
        *sources |= (uint16_t)WEFT_SRC_THERMAL;
        wsp_set_feature(p, WEFT_F_SYS_THERMAL_CAPPED);
        p->thermal_limit_mw = 8000u; /* battery-class sustained envelope */
        return;
    }
    if (ctx->apple && ctx->have_machine &&
        (strncmp(ctx->machine, "iPhone", 6) == 0 ||
         strncmp(ctx->machine, "iPad", 4) == 0)) {
        *sources |= (uint16_t)WEFT_SRC_THERMAL;
        wsp_set_feature(p, WEFT_F_SYS_THERMAL_CAPPED);
        p->thermal_limit_mw = 8000u;
    }
    /* else: thermal class unknown (0) — honest, driver refines. */
}

/* --- stage: system word ----------------------------------------------- */

static void wsp_probe_sysword(weft_hw_profile_t *p, wsp_probe_ctx_t *ctx,
                              uint16_t sources)
{
    wsp_set_feature(p, WEFT_F_SYS_64BIT);        /* host compile fact */
    wsp_set_feature(p, WEFT_F_SYS_LITTLE_ENDIAN);/* host compile fact */
    if (ctx->android) {
        wsp_set_feature(p, WEFT_F_SYS_ANDROID);
    }
    if (ctx->apple) {
        wsp_set_feature(p, WEFT_F_SYS_APPLE_OS);
    }
    /* Bare fallback: none of the rich sources answered. */
    if ((sources & (uint16_t)(WEFT_SRC_AUXV | WEFT_SRC_CPUINFO |
                              WEFT_SRC_MEMINFO | WEFT_SRC_SYSCTL |
                              WEFT_SRC_SYSPROP)) == 0u) {
        wsp_set_feature(p, WEFT_F_SYS_BARE_FALLBACK);
    }
}

/* ------------------------------------------------------------------ */
/* Public probe entry                                                  */
/* ------------------------------------------------------------------ */

weft_spectrum_status_t weft_hw_probe(weft_hw_profile_t *out_profile)
{
    const weft_probe_source_t *src;
    wsp_probe_ctx_t ctx;
    uint16_t sources = 0;
    uint32_t arch;
    uint64_t t1;

    if (out_profile == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }

    src = weft_probe_get_source();
    memset(out_profile, 0, sizeof(*out_profile));
    memset(&ctx, 0, sizeof(ctx));

    out_profile->magic = WEFT_SPECTRUM_MAGIC;
    out_profile->abi_version = WEFT_SPECTRUM_ABI_VERSION;
    out_profile->schema_hash = WEFT_SPECTRUM_SCHEMA_HASH;

    ctx.t0 = src->now_ns();

    /* --- gather shared evidence -------------------------------- */
    ctx.cpuinfo_len = src->read_file("/proc/cpuinfo", ctx.cpuinfo,
                                     sizeof(ctx.cpuinfo));
    ctx.have_cpuinfo = (ctx.cpuinfo_len > 0);

    if (src->sysctl("hw.machine", ctx.machine, sizeof(ctx.machine)) == 0 &&
        ctx.machine[0] != '\0') {
        ctx.have_machine = 1;
        ctx.apple = 1;
        sources |= (uint16_t)WEFT_SRC_SYSCTL;
    }
    if (src->sysprop("ro.board.platform", ctx.platform,
                     sizeof(ctx.platform)) == 0 &&
        ctx.platform[0] != '\0') {
        ctx.have_platform = 1;
        ctx.android = 1;
        sources |= (uint16_t)WEFT_SRC_SYSPROP;
    }
    {
        char rel[64];
        if (src->sysctl("kern.osrelease", rel, sizeof(rel)) == 0 ||
            src->read_file("/proc/sys/kernel/osrelease", rel,
                           sizeof(rel)) >= 0) {
            if (wsp_parse_kernel_major(rel, &ctx.kernel_major)) {
                ctx.have_osrelease = 1;
                sources |= (uint16_t)WEFT_SRC_KERNEL_REL;
            }
            memcpy(ctx.osrelease, rel, sizeof(ctx.osrelease));
            ctx.osrelease[sizeof(ctx.osrelease) - 1] = '\0';
        }
    }

    arch = src->arch_hint();
    ctx.apple_arm64 = (ctx.apple && arch == WEFT_PROBE_ARCH_ARM64);

    /* --- stages ------------------------------------------------- */
    switch (arch) {
    case WEFT_PROBE_ARCH_X86_64:
        wsp_probe_isa_x86(out_profile, &ctx, &sources);
        break;
    case WEFT_PROBE_ARCH_ARM64:
        wsp_probe_isa_arm64(out_profile, src, &ctx, &sources);
        break;
    case WEFT_PROBE_ARCH_RISCV64:
        wsp_probe_isa_riscv(out_profile, src, &ctx, &sources);
        break;
    default:
        /* Unknown arch: no ISA claims at all — scalar, fail-closed. */
        break;
    }

    wsp_probe_topology(out_profile, src, &ctx, &sources);
    wsp_probe_smt(out_profile, src, &sources);
    wsp_probe_cacheline(out_profile, src, &ctx, &sources);
    wsp_probe_numa(out_profile, src, &sources);
    wsp_probe_ram(out_profile, src, &ctx, &sources);
    wsp_probe_gpu(out_profile, src, &ctx, &sources);
    wsp_probe_npu(out_profile, src, &sources);
    wsp_probe_thermal(out_profile, src, &ctx, &sources);
    wsp_probe_sysword(out_profile, &ctx, sources);

    /* --- close out ---------------------------------------------- */
    out_profile->display_max_hz = 0u; /* unknown — driver promotes */
    t1 = src->now_ns();
    out_profile->probe_cost_ns =
        (uint32_t)((t1 > ctx.t0) ? (t1 - ctx.t0) : 0ull);
    out_profile->probe_sources_ok = sources;

    weft_hw_profile_seal(out_profile);
    return WEFT_SPECTRUM_OK;
}

/* ------------------------------------------------------------------ */
/* Seal / validate / promote / warmup                                  */
/* ------------------------------------------------------------------ */

void weft_hw_profile_seal(weft_hw_profile_t *profile)
{
    if (profile == NULL) {
        return;
    }
    profile->tail_magic = WEFT_SPECTRUM_TAIL_MAGIC;
    profile->crc32c = weft_crc32c(profile, 0xf8);
}

weft_spectrum_status_t weft_hw_profile_validate(const weft_hw_profile_t *p)
{
    uint32_t crc;
    if (p == NULL) {
        return WEFT_SPECTRUM_EINVAL;
    }
    if (p->magic != WEFT_SPECTRUM_MAGIC ||
        p->abi_version != WEFT_SPECTRUM_ABI_VERSION ||
        p->schema_hash != WEFT_SPECTRUM_SCHEMA_HASH) {
        return WEFT_SPECTRUM_EABI;
    }
    if (p->tail_magic != WEFT_SPECTRUM_TAIL_MAGIC) {
        return WEFT_SPECTRUM_EABI;
    }
    crc = weft_crc32c(p, 0xf8);
    if (crc != p->crc32c) {
        return WEFT_SPECTRUM_ECHECKSUM;
    }
    return WEFT_SPECTRUM_OK;
}

bool weft_hw_profile_promote_feature(weft_hw_profile_t *p, uint32_t feature_id)
{
    if (p == NULL || feature_id >= (uint32_t)WEFT_FEATURE_COUNT) {
        return false;
    }
    p->caps[feature_id >> 6] |= (1ull << (feature_id & 63u));
    weft_hw_profile_seal(p);
    return true;
}

void weft_spectrum_warmup(void)
{
    /* One-time touch of the lazy libc caches so the measured zero-alloc
     * windows are provably clean (sysconf caches its /proc reads,
     * getauxval caches the auxv table). None of these allocate. */
    const weft_probe_source_t *src = weft_probe_get_source();
    (void)src->sysconf(_SC_NPROCESSORS_ONLN);
    (void)src->auxv(AT_HWCAP);
}
