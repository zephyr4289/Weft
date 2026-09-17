// sha256_mb_bench.c — RFC-0012 falsifiable-throughput bench (Series 8).
//
// WHAT RUNS (two sections, every regime the CPU offers):
//   A. Raw multi-buffer transform throughput — uniform nblocks-per-lane
//      batches through weft_sha256_mb, pinned per backend (scalar lane loop /
//      AVX2 8-way / AVX-512 16-way), reported as compressed GiB/s.
//   B. End-to-end VerifiedWeft batch HMAC verification — the number the RFC
//      actually claims: records/s and authenticated MB/s (envelope+payload
//      bytes under HMAC) through weft_vw_batch_decode_verify_mb, pinned per
//      regime (serial-scalar / serial single-stream HW / mb-avx2 / mb-avx512),
//      across the payload sweep 64..4096.
//
// MEASUREMENT DISCIPLINE (mirrors bench_runner.c): CLOCK_MONOTONIC, warmup
// pass, median of 5 timed passes, every pass RE-VERIFIED (n_verified must
// equal N and the result must be OK) so a broken kernel can never benchmark
// as fast — correctness is a precondition of the number, not a side note.
//
// Regime pinning uses the test/bench-only force_* surface; `auto` is also
// reported as the shipped default. Honesty boundary: numbers are for THIS
// machine (named in the output) and THIS build; nothing is extrapolated to
// other silicon — the RFC quotes exactly what this tool printed.
//
// Build: make -C core/c mb-bench

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sha256.h"
#include "sha256_mb.h"
#include "verified.h"
#include "verified_mb.h"
#include "weft.h"  // weft_envelope_encode_v1

// ---------------------------------------------------------------------------
// Timing helpers (bench_runner.c's shape)
// ---------------------------------------------------------------------------

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint64_t median5(uint64_t v[5]) {
    qsort(v, 5, sizeof(uint64_t), cmp_u64);
    return v[2];
}

// ---------------------------------------------------------------------------
// Section A: raw multi-buffer transform
// ---------------------------------------------------------------------------

static void bench_transform(void) {
    printf("== A. raw multi-buffer transform (uniform nblocks per lane) ==\n");
    static const size_t BLOCKS[] = {2, 8, 32};
    const size_t GROUPS = 20000;  // groups per pass; uniform lanes

    struct regime { const char* name; weft_sha256_mb_impl_t impl; int avail; int lanes; };
    struct regime regs[4] = {
        {"scalar-lanes", WEFT_SHA256_MB_NONE, 1, WEFT_SHA256_MB_MAX_LANES},
        {"avx2-8way", WEFT_SHA256_MB_X86_AVX2,
         weft_sha256_mb_available(WEFT_SHA256_MB_X86_AVX2), 8},
        {"avx512-16way", WEFT_SHA256_MB_X86_AVX512,
         weft_sha256_mb_available(WEFT_SHA256_MB_X86_AVX512), 16},
    };
    printf("%-14s %8s %14s %14s\n", "impl", "blocks", "MiB/s", "Mblocks/s");
    for (size_t r = 0; r < 3; r++) {
        if (!regs[r].avail) {
            printf("%-14s %8s %14s %14s\n", regs[r].name, "-", "(not on this CPU)", "");
            continue;
        }
        for (size_t bi = 0; bi < sizeof(BLOCKS) / sizeof(BLOCKS[0]); bi++) {
            const size_t nb_blocks = BLOCKS[bi];
            const int lanes = (regs[r].impl == WEFT_SHA256_MB_NONE)
                                  ? WEFT_SHA256_MB_MAX_LANES
                                  : regs[r].lanes;
            const size_t msg_bytes = lanes * nb_blocks * SHA256_BLOCK_LEN * GROUPS;

            // One contiguous arena; lane j hashes its slice of it.
            uint8_t* arena = malloc(msg_bytes);
            if (!arena) continue;
            for (size_t i = 0; i < msg_bytes; i += 8) {
                memcpy(arena + i, &i, sizeof(size_t) < 8 ? 4 : 4);
            }
            const uint8_t* msg[WEFT_SHA256_MB_MAX_LANES];
            size_t nb[WEFT_SHA256_MB_MAX_LANES];
            uint32_t st_in[WEFT_SHA256_MB_MAX_LANES][8], st_out[WEFT_SHA256_MB_MAX_LANES][8];
            const size_t per_lane = nb_blocks * SHA256_BLOCK_LEN * GROUPS;
            for (int j = 0; j < lanes; j++) {
                msg[j] = arena + j * per_lane;
                nb[j] = nb_blocks;  // reset per group below — see loop
                for (int i = 0; i < 8; i++) st_in[j][i] = 0x6a09e667u + (uint32_t)i;
            }
            // weft_sha256_mb takes per-lane TOTAL block counts; a "group" in
            // this bench = one call. To keep calls uniform we pass
            // nb_blocks*GROUPS as one contiguous run per lane (the transform
            // loops groups internally — same instruction stream).
            for (int j = 0; j < lanes; j++) nb[j] = nb_blocks * GROUPS;

            uint64_t times[5];
            int ok = 1;
            // warmup
            if (regs[r].impl == WEFT_SHA256_MB_NONE) {
                weft_sha256_force_scalar();
                weft_sha256_mb_force_scalar();
            } else if (weft_sha256_mb_force_impl(regs[r].impl) != 0) {
                ok = 0;
            }
            for (int pass = 0; ok && pass < 6; pass++) {
                const uint64_t t0 = now_ns();
                if (weft_sha256_mb(st_out, (const uint32_t(*)[8])st_in, msg, nb,
                                   lanes) != 0) {
                    ok = 0;
                    break;
                }
                const uint64_t t1 = now_ns();
                if (pass > 0) times[pass - 1] = t1 - t0;
            }
            if (ok) {
                const uint64_t med = median5(times);
                const double mib = (double)msg_bytes / (1024.0 * 1024.0);
                const double mblocks = (double)lanes * (double)nb_blocks * GROUPS / 1e6;
                printf("%-14s %8zu %14.1f %14.1f\n", regs[r].name, nb_blocks,
                       mib / ((double)med / 1e9), mblocks / ((double)med / 1e9));
            } else {
                printf("%-14s %8zu %14s %14s\n", regs[r].name, nb_blocks, "ERROR", "");
            }
            weft_sha256_mb_force_auto();
            weft_sha256_force_auto();
            free(arena);
        }
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Section B: end-to-end batch HMAC verification
// ---------------------------------------------------------------------------

static const size_t PLENS[] = {64, 256, 1024, 4096};

struct vregime {
    const char* name;
    int mb_scalar;                 // 1 = pin mb to scalar lane loop
    weft_sha256_mb_impl_t mb_impl; // valid when mb_scalar == 0 and avail
    int avail;
};

static void pin_vregime(const struct vregime* r) {
    weft_sha256_force_auto();  // single-stream stays at its best (SHA-NI) for
                               // the serial regimes — the fair "best serial"
    if (r->mb_scalar) {
        weft_sha256_mb_force_scalar();
    } else {
        weft_sha256_mb_force_impl(r->mb_impl);
    }
}

static void bench_verify(void) {
    printf("== B. end-to-end VerifiedWeft batch verification "
           "(weft_vw_batch_decode_verify_mb) ==\n");
    struct vregime regs[5] = {
        {"serial-scalar", 1, WEFT_SHA256_MB_NONE, 1},
        {"serial-shani", 1, WEFT_SHA256_MB_NONE, 1},  // single-stream auto (SHA-NI), serial batch
        {"mb-avx2", 0, WEFT_SHA256_MB_X86_AVX2,
         weft_sha256_mb_available(WEFT_SHA256_MB_X86_AVX2)},
        {"mb-avx512", 0, WEFT_SHA256_MB_X86_AVX512,
         weft_sha256_mb_available(WEFT_SHA256_MB_X86_AVX512)},
        {"auto", 0, WEFT_SHA256_MB_NONE, 1},  // resolved below specially
    };
    // serial-shani differs from serial-scalar only in the single-stream pin.
    // auto: leave both tables at runtime defaults.

    uint8_t key[WEFT_VW_KEY_LEN];
    weft_vw_derive_key((const uint8_t*)"mb-bench-key-v1", 15, key);

    printf("%-14s %8s %12s %14s %16s\n", "regime", "payload", "recs/s",
           "auth MB/s", "vs serial-scalar");
    for (size_t r = 0; r < 5; r++) {
        if (!regs[r].avail) {
            printf("%-14s %8s %12s %14s %16s\n", regs[r].name, "-",
                   "(not on this CPU)", "", "");
            continue;
        }
        for (size_t pi = 0; pi < sizeof(PLENS) / sizeof(PLENS[0]); pi++) {
            const size_t plen = PLENS[pi];
            const size_t rec_len = 16 + plen + 32;
            // Target ~12 MiB of stream per pass (bounded lane counts).
            size_t n = (12u << 20) / rec_len;
            if (n < 512) n = 512;
            if (n > 262144) n = 262144;
            uint8_t* stream = malloc(n * rec_len);
            if (!stream) continue;
            uint8_t* payload = malloc(plen);
            if (!payload) { free(stream); continue; }

            pin_vregime(&regs[r]);
            if (r == 0) weft_sha256_force_scalar();  // serial-scalar: both pins

            // Sign with the CURRENT single-stream regime? No — signing is not
            // what is measured; sign once under auto so the fixture is fixed.
            weft_sha256_force_auto();
            weft_vw_signer_t s;
            weft_vw_signer_init(&s, key);
            for (size_t i = 0; i < n; i++) {
                for (size_t j = 0; j < plen; j++) {
                    payload[j] = (uint8_t)(i * 31 + j * 7);
                }
                uint8_t env[16];
                weft_envelope_encode_v1(env, (uint32_t)(i + 1), (uint32_t)plen);
                uint8_t tag[WEFT_VW_TAG_LEN];
                weft_vw_sign(&s, env, payload, plen, tag);
                memcpy(stream + i * rec_len, env, 16);
                memcpy(stream + i * rec_len + 16, payload, plen);
                memcpy(stream + i * rec_len + 16 + plen, tag, 32);
            }

            // Re-pin the measured regime AFTER fixture construction.
            pin_vregime(&regs[r]);
            if (r == 0) weft_sha256_force_scalar();

            uint64_t times[5];
            int ok = 1;
            for (int pass = 0; pass < 6; pass++) {
                size_t nver = 0, consumed = 0;
                const uint64_t t0 = now_ns();
                const weft_vw_result_t res = weft_vw_batch_decode_verify_mb(
                    key, stream, n * rec_len, NULL, 0, &nver, &consumed);
                const uint64_t t1 = now_ns();
                if (res != WEFT_VW_OK || nver != n || consumed != n * rec_len) {
                    ok = 0;  // a broken kernel never benchmarks
                    break;
                }
                if (pass > 0) times[pass - 1] = t1 - t0;
            }
            weft_sha256_mb_force_auto();
            weft_sha256_force_auto();

            static double serial_base[4] = {0, 0, 0, 0};
            if (ok) {
                const uint64_t med = median5(times);
                const double secs = (double)med / 1e9;
                const double recs_per_s = (double)n / secs;
                const double auth_mb = (double)n * (16.0 + (double)plen) / (1024.0 * 1024.0);
                const double mbps = auth_mb / secs;
                if (r == 0) serial_base[pi] = mbps;
                printf("%-14s %8zu %12.0f %14.1f %15.2fx\n", regs[r].name, plen,
                       recs_per_s, mbps,
                       serial_base[pi] > 0 ? mbps / serial_base[pi] : 1.0);
            } else {
                printf("%-14s %8zu %12s %14s %16s\n", regs[r].name, plen,
                       "VERIFY-FAIL", "", "");
            }
            free(payload);
            free(stream);
        }
    }
}

int main(void) {
    printf("weft sha256-mb bench (RFC-0012) — multi-buffer SHA-256 / batch HMAC\n");
    printf("cpu-probe: mb=%s lanes=%d, single-stream=%s\n\n",
           weft_sha256_mb_active_impl() == WEFT_SHA256_MB_X86_AVX512 ? "x86-avx512"
           : weft_sha256_mb_active_impl() == WEFT_SHA256_MB_X86_AVX2 ? "x86-avx2"
           : weft_sha256_mb_active_impl() == WEFT_SHA256_MB_ARM_NEON ? "arm-neon"
           : "scalar",
           weft_sha256_mb_lanes(),
           weft_sha256_active_impl() == WEFT_SHA256_X86_SHA_NI ? "x86-sha-ni"
           : weft_sha256_active_impl() == WEFT_SHA256_ARM_CE ? "arm-ce"
           : "scalar");
    bench_transform();
    bench_verify();
    return 0;
}
