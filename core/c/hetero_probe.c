// hetero_probe.c — RFC-0016: the heterogeneous pipeline capstone probe.
//
// THE FULL PIPELINE, one allocation end to end:
//   memfd WFSH session -> weft_gpu_wrap_dmabuf (the fd import) -> the CPU
//   publishes WTS1 tensor frames through its mmap -> the GPU consumes the
//   SAME live words through the imported allocation (tensor_reduce.spv,
//   fft_radix2.spv) -> CPU references cross-check the results.
//
// Gates (exit 1 on any failure; the shard's evidence log carries the run):
//   H1  wrap: the fd road is live on this ICD (refused -> declared exit 0,
//       the wrap-refusal gates live in gpu-extmem-test)
//   H2  tensor pass: burst of 50 f16 embeddings -> dispatch -> argmax/sum/
//       max BIT-EXACT vs the CPU mirror of the same lane partition + tree
//   H3  liveness: 50 more frames -> dispatch -> the window ADVANCES and the
//       new frame's reduction matches its own mirror
//   H4  DSP pass: a two-tone 512-sample waveform -> fft_radix2 -> peak bin
//       EXACT (37) + Parseval within the declared tolerance + magnitude
//       within 1e-3 of the CPU double-precision DFT
//
// Build: make -C core/c hetero-probe (glslangValidator rebuilds the .spv
// when present; the committed .spv is canonical otherwise).

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fanout.h"
#include "gpu_ring.h"
#include "gpu_stream.h"
#include "weft_dmabuf.h"
#include "weft_f16_codec.h"
#include "weft_tensor.h"

static int g_fail = 0;

/// f32 <-> u32 bit reinterpretation (result words are floatBitsToUint)
static float uint_as_f32(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static uint32_t f32_as_uint(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

#define CHECK(cond, name, fmt, ...)                                        \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  PASS %s\n", name);                                   \
        } else {                                                           \
            printf("  FAIL %s — " fmt "\n", name, ##__VA_ARGS__);          \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static void* read_spv(const char* fname, size_t* out_len) {
    char path[256];
    static const char* dirs[] = { "probes/compute", "../../probes/compute",
                                  "../probes/compute", "." };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], fname);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0 || (n % 4) != 0) { fclose(f); return NULL; }
        void* p = malloc((size_t)n);
        if (fread(p, 1, (size_t)n, f) != (size_t)n) {
            free(p);
            fclose(f);
            return NULL;
        }
        fclose(f);
        *out_len = (size_t)n;
        return p;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// The CPU mirrors (the shader's algorithms, exactly)
// ---------------------------------------------------------------------------

/// The lane partition + fixed tree reduction over f16 words — identical
/// to tensor_reduce.comp / .wgsl (adds only, strict-max keeps earliest).
static void tensor_reduce_ref(const uint16_t* h, uint32_t n,
                              uint32_t* argmax, float* sum, float* maxv) {
    float lane_sum[64] = {0};
    float lane_max[64] = {0};
    uint32_t lane_arg[64] = {0};
    const uint32_t chunk = (n + 63) / 64;
    for (uint32_t l = 0; l < 64; l++) {
        uint32_t lo = l * chunk;
        uint32_t hi = lo + chunk;
        if (hi > n) hi = n;
        int first = 1;
        for (uint32_t i = lo; i < hi; i++) {
            const float v = weft_f16_to_f32(h[i]);
            lane_sum[l] += v;
            if (first || v > lane_max[l]) {
                lane_max[l] = v;
                lane_arg[l] = i;
            }
            first = 0;
        }
    }
    for (uint32_t s = 1; s < 64; s <<= 1) {
        for (uint32_t l = 0; l < 64; l += 2 * s) {
            if (l + s < 64) {
                lane_sum[l] += lane_sum[l + s];
                if (lane_max[l + s] > lane_max[l]) {
                    lane_max[l] = lane_max[l + s];
                    lane_arg[l] = lane_arg[l + s];
                }
            }
        }
    }
    *argmax = lane_arg[0];
    *sum = lane_sum[0];
    *maxv = lane_max[0];
}

/// A double-precision direct DFT (the FFT's honest reference — different
/// algorithm, different precision, ULP-tolerant gates).
static void dft_peak(const float* x, uint32_t n, uint32_t* peak_bin,
                     double* peak_mag) {
    double best = 0.0;
    uint32_t best_b = 0;
    for (uint32_t b = 0; b < n; b++) {
        double re = 0.0, im = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            const double ang = -2.0 * 3.14159265358979323846 *
                               (double)b * (double)i / (double)n;
            re += (double)x[i] * cos(ang);
            im += (double)x[i] * sin(ang);
        }
        const double m = sqrt(re * re + im * im);
        if (m > best) { best = m; best_b = b; }
    }
    *peak_bin = best_b;
    *peak_mag = best;
}

// deterministic f32 embeddings (the quantization goes through the codec,
// so both sides read the same f16 words)
static void gen_vec(float* v, uint32_t n, uint32_t salt) {
    uint32_t st = 0x12345678u ^ (salt * 0x9E3779B9u);
    for (uint32_t i = 0; i < n; i++) {
        st = st * 1664525u + 1013904223u;
        v[i] = (float)((int32_t)(st >> 8) % 2000 - 1000) / 256.0f;
    }
}

int main(void) {
    printf("# H-series: heterogeneous pipeline capstone (RFC-0016)\n");

    const size_t pb = 4096;   // fits the 512-sample f32 waveform + header
    const unsigned slots = 8;
    const uint32_t words = (uint32_t)(pb / 4);

    // ---- H1: the wrap (the fd road) --------------------------------------
    printf("## H1 session wrap (fd import)\n");
    int fd = memfd_create("weft-hetero", 0);
    if (fd < 0) return 1;
    ftruncate(fd, (off_t)weft_dmabuf_span_bytes(pb, slots));
    weft_dmabuf_ring_t r;
    memset(&r, 0, sizeof(r));
    if (weft_dmabuf_ring_bind_fd(&r, fd, pb, slots) != 0) return 1;
    weft_fanout_t f;
    memset(&f, 0, sizeof(f));
    if (weft_fanout_attach_writer(&f, r.ring,
            weft_fanout_ring_bytes(pb, slots), pb, slots) != 0) return 1;

    weft_gpu_ring_t* g = NULL;
    if (weft_gpu_wrap_dmabuf(&g, pb, slots, fd) != 0) {
        printf("  (wrap refused on this ICD — the heterogeneous GPU proof "
               "is hardware-deferred; wrap-refusal gates: gpu-extmem-test; "
               "exit 0, DECLARED)\n");
        weft_fanout_destroy(&f);
        weft_dmabuf_ring_free(&r);
        close(fd);
        return 0;
    }
    printf("  wrapped: backend=%s import=%s device='%s'\n",
           weft_gpu_backend_name(g), weft_gpu_import_kind(g),
           weft_gpu_device_name(g));
    CHECK(strcmp(weft_gpu_import_kind(g), "dmabuf-fd") == 0,
          "H1: the fd road is live", "?");

    size_t tr_len = 0, fft_len = 0;
    void* tr_spv = read_spv("tensor_reduce.spv", &tr_len);
    void* fft_spv = read_spv("fft_radix2.spv", &fft_len);
    CHECK(tr_spv != NULL, "tensor_reduce.spv located", "%s",
          tr_spv ? "ok" : "missing");
    CHECK(fft_spv != NULL, "fft_radix2.spv located", "%s",
          fft_spv ? "ok" : "missing");

    // ---- H2/H3: the tensor pass + liveness --------------------------------
    printf("## H2 tensor pass (f16 embeddings, bit-exact)\n");
    weft_gpu_stream_t* st = NULL;
    float vec[256];
    uint16_t f16[256];
    uint32_t ref_arg = 0;
    float ref_sum = 0, ref_max = 0;
    int h2_ok = 0, h3_ok = 0;
    if (tr_spv && weft_gpu_stream_init(&st, g, tr_spv, tr_len, 0, 0, 0) ==
                     WEFT_GPU_STREAM_OK) {
        const uint32_t push[2] = { slots, words };
        // burst 1
        for (uint32_t s = 1; s <= 50; s++) {
            gen_vec(vec, 256, s);
            weft_tensor_publish_f16(&f, vec, 256);
        }
        gen_vec(vec, 256, 50);  // frame 50 is the latest
        for (uint32_t i = 0; i < 256; i++)
            f16[i] = weft_f32_to_f16(vec[i]);
        tensor_reduce_ref(f16, 256, &ref_arg, &ref_sum, &ref_max);

        if (weft_gpu_stream_dispatch(st, push, 8, 1, 1, 1) ==
            WEFT_GPU_STREAM_OK) {
            const uint32_t* res = weft_gpu_stream_result(st);
            const float got_sum = uint_as_f32(res[2]);
            const float got_max = uint_as_f32(res[3]);
            printf("  tensor-1: mismatch=%u argmax=%u (ref %u) seq=%u "
                   "elems=%u sum=%a (ref %a)\n",
                   res[0], res[1], ref_arg, res[6], res[5],
                   (double)got_sum, (double)ref_sum);
            h2_ok = res[0] == 0 && res[1] == ref_arg && res[5] == 256 &&
                    res[6] == 50 && res[4] == 0x54464557u &&
                    res[2] == f32_as_uint(ref_sum) &&
                    res[3] == f32_as_uint(ref_max);
        }
        CHECK(h2_ok, "H2: GPU reduction BIT-EXACT (argmax+sum+max) over the "
                     "imported allocation", "%d", h2_ok);

        // H3: burst 2 through the same CPU mapping — the GPU's view must
        // ADVANCE (live pages, not a snapshot)
        printf("## H3 liveness (the window advances)\n");
        for (uint32_t s = 51; s <= 100; s++) {
            gen_vec(vec, 256, s);
            weft_tensor_publish_f16(&f, vec, 256);
        }
        gen_vec(vec, 256, 100);
        for (uint32_t i = 0; i < 256; i++)
            f16[i] = weft_f32_to_f16(vec[i]);
        tensor_reduce_ref(f16, 256, &ref_arg, &ref_sum, &ref_max);
        if (weft_gpu_stream_dispatch(st, push, 8, 1, 1, 1) ==
            WEFT_GPU_STREAM_OK) {
            const uint32_t* res = weft_gpu_stream_result(st);
            printf("  tensor-2: mismatch=%u argmax=%u (ref %u) seq=%u "
                   "sum=%a (ref %a)\n",
                   res[0], res[1], ref_arg, res[6],
                   (double)uint_as_f32(res[2]), (double)ref_sum);
            h3_ok = res[0] == 0 && res[6] == 100 && res[1] == ref_arg &&
                    res[2] == f32_as_uint(ref_sum) &&
                    res[3] == f32_as_uint(ref_max);
        }
        CHECK(h3_ok, "H3: the reduction followed the LIVE window to frame 100",
              "%d", h3_ok);
        weft_gpu_stream_destroy(st);
    } else {
        CHECK(0, "H2: gpu_stream over the wrap", "init failed");
    }

    // ---- H4: the DSP pass --------------------------------------------------
    printf("## H4 DSP pass (512-point FFT, peak-bin exact)\n");
    weft_gpu_stream_t* fs = NULL;
    int h4_ok = 0;
    if (fft_spv && weft_gpu_stream_init(&fs, g, fft_spv, fft_len, 0, 0, 0) ==
                      WEFT_GPU_STREAM_OK) {
        float pcm[512];
        for (uint32_t n = 0; n < 512; n++) {
            pcm[n] = 0.7f * sinf(2.0f * 3.14159265f * 37.0f * n / 512.0f) +
                     0.3f * sinf(2.0f * 3.14159265f * 101.0f * n / 512.0f);
        }
        weft_tensor_publish_audio_f32(&f, pcm, 512);
        const uint32_t push[2] = { slots, words };
        if (weft_gpu_stream_dispatch(fs, push, 8, 1, 1, 1) ==
            WEFT_GPU_STREAM_OK) {
            const uint32_t* res = weft_gpu_stream_result(fs);
            const float perr = uint_as_f32(res[3]);
            const float peak_mag = uint_as_f32(res[2]);
            uint32_t ref_bin = 0;
            double ref_mag = 0;
            dft_peak(pcm, 512, &ref_bin, &ref_mag);
            const double rel = fabs((double)peak_mag - ref_mag) / ref_mag;
            printf("  fft: flags=%u peak_bin=%u (ref %u) peak=%a (ref %a, "
                   "rel=%.2e) perr=%.2e seq=%u N=%u\n",
                   res[0], res[1], ref_bin, (double)peak_mag, ref_mag, rel,
                   (double)perr, res[6], res[5]);
            h4_ok = res[0] == 0 && res[1] == ref_bin && ref_bin == 37 &&
                    res[5] == 512 && res[6] == 101 &&
                    res[4] == 0x54464557u && rel < 1e-3 && perr < 1e-4;
        }
        CHECK(h4_ok, "H4: peak bin EXACT (37) + Parseval < 1e-4 + magnitude "
                     "within 1e-3 of the double DFT", "%d", h4_ok);
        weft_gpu_stream_destroy(fs);
    } else {
        CHECK(0, "H4: gpu_stream over the wrap", "init failed");
    }

    weft_gpu_destroy(g);
    weft_fanout_destroy(&f);
    weft_dmabuf_ring_free(&r);
    close(fd);
    free(tr_spv);
    free(fft_spv);
    printf("verdict: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
