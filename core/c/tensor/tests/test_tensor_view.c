// test_tensor_view.c — WT-series (1-16): strided tensor geometry engine
// conformance (RFC-0021 §3).
//
//   WT1  dtype registry: sizes/aligns/names for all 7 dtypes + unknowns;
//        status-name registry spot checks.
//   WT2  ABI freeze pins: sizeof/offsetof(weft_tensor_view_t) == the
//        RFC-0021 §3 table (runtime mirror of the header _Static_asserts —
//        a TU that never sees the header layout could not fake these).
//   WT3  contiguous init: row-major stride derivation + byte_length for
//        1D audio / 2D spectrogram / 4D NCHW + NHWC / 5D KV-cache blocks
//        (independent arithmetic in the test, NOT the library's).
//   WT4  GOLDEN FIXTURE: every line of the Python-oracle fixture must
//        validate clean AND reproduce the exact expected offset — the
//        cross-implementation bit-exactness gate (1D audio, 2D spectrogram,
//        4D NCHW/NHWC, 5D KV, cropped sensor window, flipped plane).
//   WT5  element_offset OOB detection: index == shape, far-OOB indices.
//   WT6  validate walls: EDTYPE / EDIM (0, 9, dirty tail) / ERANGE
//        (zero shape, short byte_length = THE out-of-bounds-stride wall,
//        negative-stride window escaping the payload base) / EOVERFLOW
//        (2^61-scale extents).
//   WT7  Law 4 alignment: dtype-natural violations -> EMISALIGN for every
//        dtype class; 16/32/64/128 alignment classes enforced; the class
//        registry itself rejects 48 (EINVAL).
//   WT8  slice: audio channel + image axis narrow (offset/shape/byte_length
//        recomputed tight); OOB escapes -> ERANGE; count 0 -> ERANGE.
//   WT9  subwindow: ROI crop on NCHW; corners bit-exact; per-axis OOB.
//   WT10 permute: NCHW->NHWC shape/stride table; element set preserved
//        (address of permuted index == address of preimage index);
//        non-bijection -> EINVAL; byte_offset/byte_length invariant.
//   WT11 reshape: contiguous re-derivation chain [2,3,4]->[6,4]->[3,8]->
//        [1,24]; transposed (non-contiguous) source -> ERESHAPE; element
//        count drift -> ERESHAPE.
//   WT12 is_contiguous: contiguous / padded-row / size-1-axis / transposed
//        / zero-stride broadcast classifications.
//   WT13 element_addr / element_addr_at: pointer arithmetic equals the
//        offset math; NULL on OOB and on NULL bases.
//   WT14 negative strides: flipped-plane golden math + below-base escape.
//   WT15 overflow walls: 2^33x2^33 u8 init -> EOVERFLOW; near-2^63 strided
//        extents validate but 2^64-wrapping views do not.
//   WT16 max rank: ndim=8 end-to-end (init, validate, offsets).
//
// Build: make tensor-view-test{,-asan}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weft_tensor.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_ST(call, want, msg)                                           \
    do {                                                                    \
        const int _st = (call);                                             \
        g_checks++;                                                         \
        if (_st != (want)) {                                                \
            fprintf(stderr, "FAIL: %s: want %s got %s (line %d)\n", msg,    \
                    weft_tensor_status_name(want),                          \
                    weft_tensor_status_name(_st), __LINE__);                \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// WT1 — dtype + status registries
// ---------------------------------------------------------------------------

static void test_wt1(void) {
    static const struct { weft_dtype_t dt; uint32_t sz; const char* nm; } kDts[] = {
        {WEFT_DTYPE_U8, 1, "u8"},   {WEFT_DTYPE_I8, 1, "i8"},
        {WEFT_DTYPE_I16, 2, "i16"}, {WEFT_DTYPE_F16, 2, "f16"},
        {WEFT_DTYPE_BF16, 2, "bf16"}, {WEFT_DTYPE_F32, 4, "f32"},
        {WEFT_DTYPE_F64, 8, "f64"},
    };
    for (size_t i = 0; i < sizeof(kDts) / sizeof(kDts[0]); i++) {
        CHECK(weft_dtype_size(kDts[i].dt) == kDts[i].sz, "dtype size");
        CHECK(weft_dtype_align(kDts[i].dt) == kDts[i].sz, "dtype align == size");
        CHECK(strcmp(weft_dtype_name(kDts[i].dt), kDts[i].nm) == 0, "dtype name");
    }
    CHECK(weft_dtype_size((weft_dtype_t)0) == 0, "unknown dtype size 0");
    CHECK(weft_dtype_size((weft_dtype_t)99) == 0, "far-unknown dtype size 0");
    CHECK(strcmp(weft_dtype_name((weft_dtype_t)0), "unknown") == 0, "unknown name");
    CHECK(strcmp(weft_tensor_status_name(WEFT_TENSOR_EMISALIGN),
                 "WEFT_TENSOR_EMISALIGN") == 0, "status name EMISALIGN");
    CHECK(strcmp(weft_tensor_status_name(-999), "WEFT_TENSOR_EUNKNOWN") == 0,
          "status name unknown");
}

// ---------------------------------------------------------------------------
// WT2 — ABI freeze pins (runtime mirror of the header asserts)
// ---------------------------------------------------------------------------

static void test_wt2(void) {
    CHECK(sizeof(weft_tensor_view_t) == 176, "sizeof(weft_tensor_view_t) == 176");
    CHECK(offsetof(weft_tensor_view_t, tensor_id) == 0, "off tensor_id");
    CHECK(offsetof(weft_tensor_view_t, dtype) == 8, "off dtype");
    CHECK(offsetof(weft_tensor_view_t, ndim) == 12, "off ndim");
    CHECK(offsetof(weft_tensor_view_t, shape) == 24, "off shape");
    CHECK(offsetof(weft_tensor_view_t, strides) == 88, "off strides");
    CHECK(offsetof(weft_tensor_view_t, byte_offset) == 152, "off byte_offset");
    CHECK(offsetof(weft_tensor_view_t, byte_length) == 160, "off byte_length");
    CHECK(offsetof(weft_tensor_view_t, physical_or_shm_addr) == 168,
          "off physical_or_shm_addr");
    CHECK(sizeof(weft_dtype_t) == 4, "sizeof(weft_dtype_t) == 4");
}

// ---------------------------------------------------------------------------
// WT3 — contiguous init (test-side independent arithmetic)
// ---------------------------------------------------------------------------

static void test_wt3(void) {
    weft_tensor_view_t v;

    // 1D audio: 512 f32 samples.
    const uint64_t audio[1] = {512};
    CHECK_ST(weft_tensor_view_init(&v, 7, WEFT_DTYPE_F32, 1, audio, 0x1000, 0),
             WEFT_TENSOR_OK, "audio init");
    CHECK(v.strides[0] == 4, "audio stride");
    CHECK(v.byte_length == 2048, "audio byte_length");
    CHECK(weft_tensor_view_nelements(&v) == 512, "audio nelements");

    // 4D NCHW f32 [4,3,224,224] — strides computed by hand here.
    const uint64_t nchw[4] = {4, 3, 224, 224};
    CHECK_ST(weft_tensor_view_init(&v, 1, WEFT_DTYPE_F32, 4, nchw, 0x2000, 0),
             WEFT_TENSOR_OK, "nchw init");
    CHECK(v.strides[3] == 4, "nchw s3");
    CHECK(v.strides[2] == 224 * 4, "nchw s2");
    CHECK(v.strides[1] == 224 * 224 * 4, "nchw s1");
    CHECK(v.strides[0] == 3 * 224 * 224 * 4, "nchw s0");
    CHECK(v.byte_length == 4ull * 3 * 224 * 224 * 4, "nchw byte_length");

    // 4D NHWC u8 [1,224,224,3].
    const uint64_t nhwc[4] = {1, 224, 224, 3};
    CHECK_ST(weft_tensor_view_init(&v, 2, WEFT_DTYPE_U8, 4, nhwc, 0, 0),
             WEFT_TENSOR_OK, "nhwc init");
    CHECK(v.strides[3] == 1, "nhwc s3");
    CHECK(v.strides[2] == 3, "nhwc s2");
    CHECK(v.strides[1] == 224 * 3, "nhwc s1");
    CHECK(v.strides[0] == 224 * 224 * 3, "nhwc s0");

    // 5D KV-cache block f16 [2,8,128,64,2]: last stride 2, next 128...
    const uint64_t kv[5] = {2, 8, 128, 64, 2};
    CHECK_ST(weft_tensor_view_init(&v, 3, WEFT_DTYPE_F16, 5, kv, 0, 0),
             WEFT_TENSOR_OK, "kv init");
    CHECK(v.strides[4] == 2, "kv s4");
    CHECK(v.strides[3] == 4, "kv s3");
    CHECK(v.strides[2] == 256, "kv s2");
    CHECK(v.strides[1] == 32768, "kv s1");
    CHECK(v.strides[0] == 262144, "kv s0");

    // Bad calls: unknown dtype, ndim 0, ndim 9, zero shape.
    CHECK_ST(weft_tensor_view_init(&v, 0, (weft_dtype_t)42, 1, audio, 0, 0),
             WEFT_TENSOR_EDTYPE, "init unknown dtype");
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 0, audio, 0, 0),
             WEFT_TENSOR_EDIM, "init ndim 0");
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 9, audio, 0, 0),
             WEFT_TENSOR_EDIM, "init ndim 9");
    const uint64_t zeroed[2] = {4, 0};
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 2, zeroed, 0, 0),
             WEFT_TENSOR_ERANGE, "init zero shape");
    CHECK_ST(weft_tensor_view_init(NULL, 0, WEFT_DTYPE_F32, 1, audio, 0, 0),
             WEFT_TENSOR_EINVAL, "init NULL view");
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 1, NULL, 0, 0),
             WEFT_TENSOR_EINVAL, "init NULL shape");
}

// ---------------------------------------------------------------------------
// WT4 — golden fixture (the Python-oracle bit-exactness gate)
// ---------------------------------------------------------------------------

static int parse_u64_csv(const char* s, uint64_t* out, int max_n) {
    int n = 0;
    while (*s) {
        char* end;
        out[n++] = strtoull(s, &end, 10);
        if (n >= max_n) return -1;
        s = end;
        if (*s == ',') s++;
        else if (*s == 0) break;
        else return -1;
    }
    return n;
}

static int parse_i64_csv(const char* s, int64_t* out, int max_n) {
    int n = 0;
    while (*s) {
        char* end;
        out[n++] = strtoll(s, &end, 10);
        if (n >= max_n) return -1;
        s = end;
        if (*s == ',') s++;
        else if (*s == 0) break;
        else return -1;
    }
    return n;
}

static weft_dtype_t dtype_of(const char* nm) {
    for (weft_dtype_t d = WEFT_DTYPE_U8; d <= WEFT_DTYPE_F64; d++) {
        // enum increment order matches the registry 1..7
        if (strcmp(weft_dtype_name(d), nm) == 0) return d;
    }
    return (weft_dtype_t)0;
}

static void test_wt4(const char* fixture_path) {
    FILE* f = fopen(fixture_path, "r");
    if (f == NULL) {
        fprintf(stderr, "FAIL: fixture open %s (line %d)\n", fixture_path, __LINE__);
        g_failures++;
        return;
    }
    char line[512];
    int lines = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char case_id[64], dname[16], shape_s[128], strides_s[128], idx_s[128];
        int ndim;
        unsigned long long byte_offset, byte_length, want;
        // format: case dtype ndim shape strides byte_offset byte_length idx expected
        if (sscanf(line, "%63s %15s %d %127s %127s %llu %llu %127s %llu",
                   case_id, dname, &ndim, shape_s, strides_s, &byte_offset,
                   &byte_length, idx_s, &want) != 9) {
            fprintf(stderr, "FAIL: fixture parse %s (line %d)\n", case_id, __LINE__);
            g_failures++;
            continue;
        }
        lines++;

        uint64_t shape[WEFT_TENSOR_MAX_DIMS] = {0};
        int64_t strides[WEFT_TENSOR_MAX_DIMS] = {0};
        uint64_t idx[WEFT_TENSOR_MAX_DIMS] = {0};
        int ns = parse_u64_csv(shape_s, shape, WEFT_TENSOR_MAX_DIMS);
        int nstr = parse_i64_csv(strides_s, strides, WEFT_TENSOR_MAX_DIMS);
        int ni = parse_u64_csv(idx_s, idx, WEFT_TENSOR_MAX_DIMS);
        CHECK(ns == ndim && nstr == ndim && ni == ndim, "fixture field counts");

        weft_tensor_view_t v;
        CHECK_ST(weft_tensor_view_init_strided(&v, lines, dtype_of(dname),
                                               (uint8_t)ndim, shape, strides, 0,
                                               byte_offset, byte_length),
                 WEFT_TENSOR_OK, "fixture view init");
        CHECK_ST(weft_tensor_view_validate(&v, WEFT_TENSOR_ALIGN_NONE),
                 WEFT_TENSOR_OK, "fixture validate");

        uint64_t off = 0;
        CHECK_ST(weft_tensor_view_element_offset(&v, idx, &off),
                 WEFT_TENSOR_OK, "fixture offset status");
        char msg[96];
        snprintf(msg, sizeof(msg), "fixture %s offset %llu != %llu", case_id,
                 (unsigned long long)off, want);
        CHECK(off == (uint64_t)want, msg);
    }
    fclose(f);
    CHECK(lines >= 20, "fixture line count (expected the full battery)");
    printf("WT4 fixture: %d golden offsets reproduced bit-exact\n", lines);
}

// ---------------------------------------------------------------------------
// WT5 — element_offset OOB detection
// ---------------------------------------------------------------------------

static void test_wt5(void) {
    const uint64_t shape[2] = {64, 100};
    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F16, 2, shape, 0, 0),
             WEFT_TENSOR_OK, "wt5 init");

    uint64_t off;
    const uint64_t ok_idx[2] = {63, 99};
    CHECK_ST(weft_tensor_view_element_offset(&v, ok_idx, &off), WEFT_TENSOR_OK,
             "wt5 in-range");
    CHECK(off == 63 * 200 + 99 * 2, "wt5 last element offset");

    const uint64_t oob0[2] = {64, 0};
    CHECK_ST(weft_tensor_view_element_offset(&v, oob0, &off), WEFT_TENSOR_ERANGE,
             "axis0 OOB");
    const uint64_t oob1[2] = {0, 100};
    CHECK_ST(weft_tensor_view_element_offset(&v, oob1, &off), WEFT_TENSOR_ERANGE,
             "axis1 OOB");
    const uint64_t far[2] = {UINT64_MAX, UINT64_MAX};
    CHECK_ST(weft_tensor_view_element_offset(&v, far, &off), WEFT_TENSOR_ERANGE,
             "far OOB rejected before any multiply");
    CHECK_ST(weft_tensor_view_element_offset(&v, NULL, &off), WEFT_TENSOR_EINVAL,
             "NULL idx");
    CHECK_ST(weft_tensor_view_element_offset(NULL, ok_idx, &off),
             WEFT_TENSOR_EINVAL, "NULL view");
}

// ---------------------------------------------------------------------------
// WT6 — validate walls
// ---------------------------------------------------------------------------

static void test_wt6(void) {
    weft_tensor_view_t v;

    // Unknown dtype.
    memset(&v, 0, sizeof(v));
    v.dtype = (weft_dtype_t)77;
    v.ndim = 1;
    v.shape[0] = 4;
    v.strides[0] = 4;
    v.byte_length = 16;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_EDTYPE, "validate dtype");

    // ndim 0 / 9.
    memset(&v, 0, sizeof(v));
    v.dtype = WEFT_DTYPE_F32;
    v.ndim = 0;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_EDIM, "validate ndim 0");
    v.ndim = 9;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_EDIM, "validate ndim 9");

    // Dirty tail (non-canonical zero-fill beyond ndim).
    const uint64_t shape[2] = {8, 8};
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 2, shape, 0, 0),
             WEFT_TENSOR_OK, "tail init");
    v.shape[5] = 3;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_EDIM, "dirty shape tail");
    v.shape[5] = 0;
    v.strides[7] = 4;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_EDIM, "dirty stride tail");

    // Zero shape axis.
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 2, shape, 0, 0),
             WEFT_TENSOR_OK, "reinit");
    v.shape[1] = 0;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_ERANGE, "zero shape axis");

    // THE out-of-bounds-stride wall: declared span too short for strides.
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 2, shape, 0, 0),
             WEFT_TENSOR_OK, "reinit 2");
    v.strides[0] = 4096;   // padded rows: needs >= 7*4096+7*4+4
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_ERANGE,
             "OOB stride (span wall)");
    v.byte_length = 7 * 4096 + 7 * 4 + 4;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_OK,
             "padded span passes");

    // Negative-stride window escaping the payload base.
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 2, shape, 0, 0),
             WEFT_TENSOR_OK, "reinit 3");
    v.strides[0] = -4096;
    v.byte_offset = 100;   // window needs >= 7*4096 below
    v.byte_length = 7 * 4096 + 7 * 4 + 4;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_ERANGE,
             "negative window below base");
    v.byte_offset = 7 * 4096;
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_OK,
             "flipped window at base boundary");

    // 2^61-scale extents overflow.
    const uint64_t big[2] = {1ull << 40, 1ull << 40};
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_U8, 2, big, 0, 0),
             WEFT_TENSOR_EOVERFLOW, "2^80 extent refused");
}

// ---------------------------------------------------------------------------
// WT7 — Law 4 alignment classes
// ---------------------------------------------------------------------------

static void test_wt7(void) {
    const uint64_t shape[1] = {16};
    weft_tensor_view_t v;

    // Every dtype class: odd byte offsets violate natural alignment
    // (the 1-byte dtypes are naturally always aligned — they take the
    // positive leg only).
    static const weft_dtype_t kAll[] = {WEFT_DTYPE_U8, WEFT_DTYPE_I8,
                                        WEFT_DTYPE_I16, WEFT_DTYPE_F16,
                                        WEFT_DTYPE_BF16, WEFT_DTYPE_F32,
                                        WEFT_DTYPE_F64};
    for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); i++) {
        if (weft_dtype_size(kAll[i]) > 1) {
            CHECK_ST(weft_tensor_view_init(&v, 0, kAll[i], 1, shape, 0, 1),
                     WEFT_TENSOR_OK, "wt7 init");
            CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_EMISALIGN,
                     "odd offset violates dtype-natural alignment");
        }
        CHECK_ST(weft_tensor_view_init(&v, 0, kAll[i], 1, shape, 0, 0),
                 WEFT_TENSOR_OK, "wt7 init aligned");
        CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_OK,
                 "aligned base+offset passes");
    }

    // Explicit alignment classes.
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 1, shape, 0x10000, 128),
             WEFT_TENSOR_OK, "wt7 class init");
    CHECK_ST(weft_tensor_view_validate(&v, WEFT_TENSOR_ALIGN_128), WEFT_TENSOR_OK,
             "128B-aligned view passes ALIGN_128");
    CHECK_ST(weft_tensor_view_validate(&v, WEFT_TENSOR_ALIGN_64), WEFT_TENSOR_OK,
             "passes ALIGN_64");
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F32, 1, shape, 0x10000, 32),
             WEFT_TENSOR_OK, "wt7 class init 32");
    CHECK_ST(weft_tensor_view_validate(&v, WEFT_TENSOR_ALIGN_64),
             WEFT_TENSOR_EMISALIGN, "32B-aligned view fails ALIGN_64");
    CHECK_ST(weft_tensor_view_validate(&v, WEFT_TENSOR_ALIGN_32), WEFT_TENSOR_OK,
             "passes ALIGN_32");
    CHECK_ST(weft_tensor_view_validate(&v, 48), WEFT_TENSOR_EINVAL,
             "class registry rejects 48");
    CHECK_ST(weft_tensor_view_validate(&v, 8), WEFT_TENSOR_EINVAL,
             "class registry rejects 8");

    // Base misalignment (not just byte_offset).
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F64, 1, shape, 2, 0),
             WEFT_TENSOR_OK, "wt7 base misalign init");
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_EMISALIGN,
             "misaligned base fails");
}

// ---------------------------------------------------------------------------
// WT8 — slice
// ---------------------------------------------------------------------------

static void test_wt8(void) {
    // Audio: 1024-sample f32 frame; slice [256, 512).
    const uint64_t audio[1] = {1024};
    weft_tensor_view_t v, s;
    CHECK_ST(weft_tensor_view_init(&v, 9, WEFT_DTYPE_F32, 1, audio, 0x1000, 0),
             WEFT_TENSOR_OK, "wt8 audio init");
    CHECK_ST(weft_tensor_view_slice(&s, &v, 0, 256, 256), WEFT_TENSOR_OK,
             "audio slice");
    CHECK(s.byte_offset == 256 * 4, "audio slice offset");
    CHECK(s.shape[0] == 256, "audio slice shape");
    CHECK(s.byte_length == 256 * 4, "audio slice tight length");
    CHECK(s.tensor_id == 9, "slice preserves tensor_id");
    uint64_t off;
    const uint64_t last[1] = {255};
    CHECK_ST(weft_tensor_view_element_offset(&s, last, &off), WEFT_TENSOR_OK, "s off");
    CHECK(off == 256 * 4 + 255 * 4, "audio slice last element");

    CHECK_ST(weft_tensor_view_slice(&s, &v, 0, 900, 200), WEFT_TENSOR_ERANGE,
             "slice escape");
    CHECK_ST(weft_tensor_view_slice(&s, &v, 0, 0, 0), WEFT_TENSOR_ERANGE,
             "slice zero count");
    CHECK_ST(weft_tensor_view_slice(&s, &v, 1, 0, 1), WEFT_TENSOR_EDIM,
             "slice axis OOB");

    // NCHW channel slice: axis 1 of [1,3,224,224] u8 -> [1,1,224,224].
    const uint64_t nchw[4] = {1, 3, 224, 224};
    CHECK_ST(weft_tensor_view_init(&v, 1, WEFT_DTYPE_U8, 4, nchw, 0x4000, 0),
             WEFT_TENSOR_OK, "wt8 nchw init");
    CHECK_ST(weft_tensor_view_slice(&s, &v, 1, 2, 1), WEFT_TENSOR_OK, "channel 2");
    CHECK(s.byte_offset == 2 * 50176, "channel 2 offset");
    CHECK(s.shape[1] == 1, "channel shape");
    CHECK(s.strides[0] == 150528 && s.strides[2] == 224 && s.strides[3] == 1,
          "channel strides unchanged");
    const uint64_t px[4] = {0, 0, 223, 223};
    CHECK_ST(weft_tensor_view_element_offset(&s, px, &off), WEFT_TENSOR_OK, "c off");
    CHECK(off == 2 * 50176 + 223 * 224 + 223, "channel 2 last pixel");
}

// ---------------------------------------------------------------------------
// WT9 — subwindow
// ---------------------------------------------------------------------------

static void test_wt9(void) {
    const uint64_t nchw[4] = {2, 3, 224, 224};
    weft_tensor_view_t v, w;
    CHECK_ST(weft_tensor_view_init(&v, 5, WEFT_DTYPE_F32, 4, nchw, 0x100000, 0),
             WEFT_TENSOR_OK, "wt9 init");

    // ROI: batch 1, all channels, rows [32,96), cols [64,128).
    const uint64_t offset[4] = {1, 0, 32, 64};
    const uint64_t count[4] = {1, 3, 64, 64};
    CHECK_ST(weft_tensor_view_subwindow(&w, &v, offset, count), WEFT_TENSOR_OK,
             "subwindow");
    CHECK(w.byte_offset == 1 * 602112 + 32 * 896 + 64 * 4, "subwindow offset");
    CHECK(w.shape[2] == 64 && w.shape[3] == 64 && w.shape[1] == 3, "subwindow shape");
    const uint64_t first[4] = {0, 0, 0, 0};
    const uint64_t last[4] = {0, 2, 63, 63};
    uint64_t off;
    CHECK_ST(weft_tensor_view_element_offset(&w, first, &off), WEFT_TENSOR_OK, "sw f");
    CHECK(off == 602112 + 32 * 896 + 64 * 4, "subwindow first");
    CHECK_ST(weft_tensor_view_element_offset(&w, last, &off), WEFT_TENSOR_OK, "sw l");
    CHECK(off == 602112 + 32 * 896 + 64 * 4 + 2 * 200704 + 63 * 896 + 63 * 4,
          "subwindow last");

    const uint64_t bad_off[4] = {0, 0, 200, 0};
    const uint64_t ok_cnt[4] = {1, 3, 64, 64};
    CHECK_ST(weft_tensor_view_subwindow(&w, &v, bad_off, ok_cnt),
             WEFT_TENSOR_ERANGE, "subwindow escape");
    const uint64_t zero_cnt[4] = {1, 3, 0, 64};
    CHECK_ST(weft_tensor_view_subwindow(&w, &v, offset, zero_cnt),
             WEFT_TENSOR_ERANGE, "subwindow zero count");
}

// ---------------------------------------------------------------------------
// WT10 — permute (NCHW <-> NHWC)
// ---------------------------------------------------------------------------

static void test_wt10(void) {
    const uint64_t nchw[4] = {2, 3, 8, 5};  // N,C,H,W
    weft_tensor_view_t v, p;
    CHECK_ST(weft_tensor_view_init(&v, 11, WEFT_DTYPE_F32, 4, nchw, 0x8000, 0),
             WEFT_TENSOR_OK, "wt10 init");

    // NCHW -> NHWC: perm = (N,H,W,C) = (0,2,3,1).
    const uint8_t perm[4] = {0, 2, 3, 1};
    CHECK_ST(weft_tensor_view_permute(&p, &v, perm), WEFT_TENSOR_OK, "permute");
    CHECK(p.shape[0] == 2 && p.shape[1] == 8 && p.shape[2] == 5 && p.shape[3] == 3,
          "NHWC shape");
    CHECK(p.strides[0] == 480 && p.strides[1] == 20 && p.strides[2] == 4 &&
          p.strides[3] == 160, "NHWC strides");
    CHECK(p.byte_offset == v.byte_offset && p.byte_length == v.byte_length,
          "permute keeps window");

    // Element-set preservation: address of permuted idx == address of the
    // preimage idx in the source view.
    const uint64_t pidx[4] = {1, 6, 4, 2};          // n,h,w,c
    const uint64_t sidx[4] = {1, 2, 6, 4};          // n,c,h,w
    uint64_t po, so;
    CHECK_ST(weft_tensor_view_element_offset(&p, pidx, &po), WEFT_TENSOR_OK, "p off");
    CHECK_ST(weft_tensor_view_element_offset(&v, sidx, &so), WEFT_TENSOR_OK, "s off");
    CHECK(po == so, "permuted element address preserved");

    // Inverse round trip: NHWC -> NCHW restores the table.
    weft_tensor_view_t back;
    const uint8_t inv[4] = {0, 3, 1, 2};
    CHECK_ST(weft_tensor_view_permute(&back, &p, inv), WEFT_TENSOR_OK, "inverse");
    CHECK(back.shape[1] == 3 && back.shape[2] == 8 && back.shape[3] == 5,
          "inverse shape");
    CHECK(back.strides[1] == 160 && back.strides[2] == 20 && back.strides[3] == 4,
          "inverse strides");

    // Non-bijections refused.
    const uint8_t dup[4] = {0, 0, 1, 2};
    CHECK_ST(weft_tensor_view_permute(&p, &v, dup), WEFT_TENSOR_EINVAL,
             "duplicate axis");
    const uint8_t oob[4] = {0, 1, 2, 4};
    CHECK_ST(weft_tensor_view_permute(&p, &v, oob), WEFT_TENSOR_EINVAL,
             "axis out of rank");
}

// ---------------------------------------------------------------------------
// WT11 — reshape
// ---------------------------------------------------------------------------

static void test_wt11(void) {
    const uint64_t shape[3] = {2, 3, 4};
    weft_tensor_view_t v, r;
    CHECK_ST(weft_tensor_view_init(&v, 21, WEFT_DTYPE_F32, 3, shape, 0x20000, 0),
             WEFT_TENSOR_OK, "wt11 init");

    const uint64_t s1[2] = {6, 4};
    CHECK_ST(weft_tensor_view_reshape(&r, &v, 2, s1), WEFT_TENSOR_OK, "reshape 6x4");
    CHECK(r.strides[1] == 4 && r.strides[0] == 16, "reshape 6x4 strides");
    const uint64_t s2[2] = {3, 8};
    weft_tensor_view_t r2;
    CHECK_ST(weft_tensor_view_reshape(&r2, &r, 2, s2), WEFT_TENSOR_OK, "reshape 3x8");
    CHECK(r2.strides[1] == 4 && r2.strides[0] == 32, "reshape 3x8 strides");
    const uint64_t s3[1] = {24};
    weft_tensor_view_t r3;
    CHECK_ST(weft_tensor_view_reshape(&r3, &r2, 1, s3), WEFT_TENSOR_OK, "reshape 24");
    CHECK(r3.strides[0] == 4, "reshape 24 stride");
    CHECK(r3.tensor_id == 21 && r3.byte_offset == v.byte_offset,
          "reshape keeps identity/origin");

    // Element-count drift.
    const uint64_t bad[2] = {6, 5};
    CHECK_ST(weft_tensor_view_reshape(&r, &v, 2, bad), WEFT_TENSOR_ERESHAPE,
             "count drift");

    // Non-contiguous source: transpose first, then reshape -> refused.
    const uint8_t perm[3] = {2, 1, 0};
    weft_tensor_view_t t;
    CHECK_ST(weft_tensor_view_permute(&t, &v, perm), WEFT_TENSOR_OK, "transpose");
    CHECK_ST(weft_tensor_view_reshape(&r, &t, 2, s1), WEFT_TENSOR_ERESHAPE,
             "non-contiguous reshape refused");
}

// ---------------------------------------------------------------------------
// WT12 — is_contiguous classification
// ---------------------------------------------------------------------------

static void test_wt12(void) {
    const uint64_t shape[2] = {64, 100};
    weft_tensor_view_t v;

    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_F16, 2, shape, 0, 0),
             WEFT_TENSOR_OK, "wt12 init");
    CHECK(weft_tensor_view_is_contiguous(&v) == 1, "contiguous");

    // Padded rows.
    const int64_t padded[2] = {256, 2};
    CHECK_ST(weft_tensor_view_init_strided(&v, 0, WEFT_DTYPE_F16, 2, shape, padded,
                                           0, 0, 16328),
             WEFT_TENSOR_OK, "wt12 padded init");
    CHECK(weft_tensor_view_is_contiguous(&v) == 0, "padded rows non-contiguous");

    // Size-1 axes are unconstrained: [64,1,100] with any middle stride.
    const uint64_t s3[3] = {64, 1, 100};
    const int64_t weird[3] = {200, 999, 2};
    CHECK_ST(weft_tensor_view_init_strided(&v, 0, WEFT_DTYPE_F16, 3, s3, weird, 0,
                                           0, 12800),
             WEFT_TENSOR_OK, "wt12 size1 init");
    CHECK(weft_tensor_view_is_contiguous(&v) == 1, "size-1 axis unconstrained");

    // Transposed.
    const int64_t transposed[2] = {2, 200};
    CHECK_ST(weft_tensor_view_init_strided(&v, 0, WEFT_DTYPE_F16, 2, shape,
                                           transposed, 0, 0, 16328),
             WEFT_TENSOR_OK, "wt12 transposed init");
    CHECK(weft_tensor_view_is_contiguous(&v) == 0, "transposed non-contiguous");

    // Zero-stride broadcast.
    const int64_t bcast[2] = {0, 2};
    CHECK_ST(weft_tensor_view_init_strided(&v, 0, WEFT_DTYPE_F16, 2, shape, bcast,
                                           0, 0, 202),
             WEFT_TENSOR_OK, "wt12 bcast init");
    CHECK(weft_tensor_view_is_contiguous(&v) == 0, "broadcast non-contiguous");
    CHECK(weft_tensor_view_is_contiguous(NULL) == 0, "NULL non-contiguous");
}

// ---------------------------------------------------------------------------
// WT13 — element_addr / element_addr_at
// ---------------------------------------------------------------------------

static void test_wt13(void) {
    static uint8_t buf[8192] __attribute__((aligned(64)));
    const uint64_t shape[2] = {16, 32};
    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init(&v, 3, WEFT_DTYPE_F32, 2, shape,
                                   (uintptr_t)buf, 128),
             WEFT_TENSOR_OK, "wt13 init");
    const uint64_t idx[2] = {7, 11};
    uint64_t off;
    CHECK_ST(weft_tensor_view_element_offset(&v, idx, &off), WEFT_TENSOR_OK, "off");
    void* a = weft_tensor_view_element_addr(&v, idx);
    CHECK(a == (void*)(buf + off), "element_addr == base + offset");
    void* b = weft_tensor_view_element_addr_at(&v, idx, buf);
    CHECK(b == a, "element_addr_at explicit base");

    const uint64_t oob[2] = {16, 0};
    CHECK(weft_tensor_view_element_addr(&v, oob) == NULL, "OOB addr NULL");
    CHECK(weft_tensor_view_element_addr_at(&v, idx, NULL) == NULL,
          "NULL base NULL");
    weft_tensor_view_t z = v;
    z.physical_or_shm_addr = 0;
    CHECK(weft_tensor_view_element_addr(&z, idx) == NULL, "zero base NULL");
}

// ---------------------------------------------------------------------------
// WT14 — negative strides (flipped plane)
// ---------------------------------------------------------------------------

static void test_wt14(void) {
    const uint64_t shape[2] = {480, 640};
    const int64_t flip[2] = {-2560, 4};
    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init_strided(&v, 0, WEFT_DTYPE_F32, 2, shape, flip,
                                           0, 1226240, 1228800),
             WEFT_TENSOR_OK, "wt14 init");
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_OK, "flipped validate");

    const uint64_t first[2] = {0, 0};
    const uint64_t last[2] = {479, 639};
    uint64_t off;
    CHECK_ST(weft_tensor_view_element_offset(&v, first, &off), WEFT_TENSOR_OK, "f");
    CHECK(off == 1226240, "flipped first == last-row origin");
    CHECK_ST(weft_tensor_view_element_offset(&v, last, &off), WEFT_TENSOR_OK, "l");
    CHECK(off == 639 * 4, "flipped last == row 0 tail");

    // Below-base escape.
    weft_tensor_view_t bad = v;
    bad.byte_offset = 1226240 - 2560;
    CHECK_ST(weft_tensor_view_validate(&bad, 0), WEFT_TENSOR_ERANGE,
             "flipped escape below base");
    const uint64_t idx[2] = {480, 0};  // row 480 == out of range anyway
    CHECK(weft_tensor_view_element_offset(&bad, idx, &off) == WEFT_TENSOR_ERANGE,
          "flipped OOB row");
}

// ---------------------------------------------------------------------------
// WT15 — overflow walls
// ---------------------------------------------------------------------------

static void test_wt15(void) {
    weft_tensor_view_t v;
    const uint64_t huge[2] = {1ull << 33, 1ull << 33};
    CHECK_ST(weft_tensor_view_init(&v, 0, WEFT_DTYPE_U8, 2, huge, 0, 0),
             WEFT_TENSOR_EOVERFLOW, "2^66 elements refused");

    // Near-2^63 strided extent: legal to describe, wall-checked on walk.
    const uint64_t shape2[2] = {2, 2};
    const int64_t strides[2] = {(int64_t)((1ull << 62) + 8), 1};
    CHECK_ST(weft_tensor_view_init_strided(&v, 0, WEFT_DTYPE_U8, 2, shape2,
                                           strides, 0, 0, (1ull << 62) + 18),
             WEFT_TENSOR_OK, "near-2^62 init");
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_OK,
             "near-2^62 validates");
    const uint64_t idx[2] = {1, 1};
    uint64_t off;
    CHECK_ST(weft_tensor_view_element_offset(&v, idx, &off), WEFT_TENSOR_OK,
             "near-2^62 offset");
    CHECK(off == (1ull << 62) + 9, "near-2^62 exact");

    // byte_offset + U wrapping 2^64.
    weft_tensor_view_t wrap = v;
    wrap.byte_offset = UINT64_MAX - 100;
    CHECK_ST(weft_tensor_view_validate(&wrap, 0), WEFT_TENSOR_EOVERFLOW,
             "2^64 wrap refused");

    // nelements saturation.
    CHECK(weft_tensor_view_nelements(&wrap) == 4, "nelements small");
    const uint64_t mid[2] = {1ull << 32, 1ull << 32};
    weft_tensor_view_t mv;
    CHECK_ST(weft_tensor_view_init_strided(&mv, 0, WEFT_DTYPE_U8, 2, mid,
                                           strides, 0, 0, UINT64_MAX),
             WEFT_TENSOR_OK, "nelem sat init");
    CHECK(weft_tensor_view_nelements(&mv) == UINT64_MAX,
          "nelements saturates at 2^64-1");
}

// ---------------------------------------------------------------------------
// WT16 — maximum rank
// ---------------------------------------------------------------------------

static void test_wt16(void) {
    const uint64_t shape[8] = {2, 3, 4, 5, 6, 7, 8, 9};
    weft_tensor_view_t v;
    CHECK_ST(weft_tensor_view_init(&v, 33, WEFT_DTYPE_F16, 8, shape, 0x7000, 0),
             WEFT_TENSOR_OK, "rank-8 init");
    CHECK_ST(weft_tensor_view_validate(&v, 0), WEFT_TENSOR_OK, "rank-8 validate");
    CHECK(v.strides[7] == 2, "rank-8 last stride");
    CHECK(v.strides[0] == 3 * 4 * 5 * 6 * 7 * 8 * 9 * 2, "rank-8 first stride");
    const uint64_t idx[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t off;
    CHECK_ST(weft_tensor_view_element_offset(&v, idx, &off), WEFT_TENSOR_OK,
             "rank-8 offset");
    // Independent: total elements 2*3*4*5*6*7*8*9 = 362880; last element
    // offset = (362880-1)*2.
    CHECK(off == (2ull * 3 * 4 * 5 * 6 * 7 * 8 * 9 - 1) * 2, "rank-8 exact");
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const char* fixture =
        argc > 1 ? argv[1] : "tests/fixtures/tensor_view_golden.txt";

    test_wt1();
    test_wt2();
    test_wt3();
    test_wt4(fixture);
    test_wt5();
    test_wt6();
    test_wt7();
    test_wt8();
    test_wt9();
    test_wt10();
    test_wt11();
    test_wt12();
    test_wt13();
    test_wt14();
    test_wt15();
    test_wt16();

    printf("tensor-view: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
