// gpu_layout_check.c — INDEPENDENT WGSL/GLSL layout verifier (double entry).
//
// The emitters compute GPU layouts with gpu/gpu_layout.c. This checker does
// NOT trust them: it re-parses the emitted .wgsl / .glsl TEXT, re-derives
// every member offset from the alignment rules with its own implementation,
// and confronts the result with the C headers' offsetof macros (ground truth
// from a completely different code path). Emitters and checker agree, or the
// gate fails.
//
// It also gates Law 4 itself: every struct the matrix expects to be refused
// MUST carry a WEFT-GPU-NOT-EXACT marker with a non-empty reason and MUST NOT
// emit any struct definition; every struct expected exact MUST.
//
// Usage: gpu_layout_check <out-root>
//   <out-root>/<fixture>/<fixture>.wgsl and .glsl (as produced by the runner)

#include "telemetry_frame.h"
#include "mcu_status.h"
#include "camera_exposure.h"
#include "sensor_event.h"
#include "audio_peak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            g_pass++;                                                         \
            printf("[gpu-check] PASS: " __VA_ARGS__);                         \
            printf("\n");                                                     \
        } else {                                                              \
            g_fail++;                                                         \
            printf("[gpu-check] FAIL: " __VA_ARGS__);                         \
            printf(" (line %d)\n", __LINE__);                                 \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// ground-truth field offsets, straight from the generated C headers
// ---------------------------------------------------------------------------

typedef struct { const char* name; size_t off; } fov;

static const fov k_tf[] = {
    {"schema_id", WEFT_TELEMETRY_FRAME_OFF_SCHEMA_ID},
    {"velocity", WEFT_TELEMETRY_FRAME_OFF_VELOCITY},
    {"altitude", WEFT_TELEMETRY_FRAME_OFF_ALTITUDE},
    {"accel", WEFT_TELEMETRY_FRAME_OFF_ACCEL},
    {"pad0", WEFT_TELEMETRY_FRAME_OFF_PAD0},
    {"gyro", WEFT_TELEMETRY_FRAME_OFF_GYRO},
    {"baro_pressure", WEFT_TELEMETRY_FRAME_OFF_BARO_PRESSURE},
    {"quaternion", WEFT_TELEMETRY_FRAME_OFF_QUATERNION},
};
static const fov k_ce[] = {
    {"schema_id", WEFT_CAMERA_EXPOSURE_OFF_SCHEMA_ID},
    {"mode", WEFT_CAMERA_EXPOSURE_OFF_MODE},
    {"gain", WEFT_CAMERA_EXPOSURE_OFF_GAIN},
    {"exposure_us", WEFT_CAMERA_EXPOSURE_OFF_EXPOSURE_US},
    {"position", WEFT_CAMERA_EXPOSURE_OFF_POSITION},
    {"projection", WEFT_CAMERA_EXPOSURE_OFF_PROJECTION},
    {"roi", WEFT_CAMERA_EXPOSURE_OFF_ROI},
};
static const fov k_ap[] = {
    {"schema_id", WEFT_AUDIO_PEAK_OFF_SCHEMA_ID},
    {"levels_db", WEFT_AUDIO_PEAK_OFF_LEVELS_DB},
};

typedef struct {
    const char* fixture;        // dir + file stem
    const fov* fields;
    uint32_t nfields;
    size_t c_size;
    const char* wgsl_name;      // storage struct name in WGSL
    const char* glsl_name;      // storage struct name in GLSL
    int wgsl_storage, wgsl_uniform;   // expected exactness
    int glsl_storage, glsl_uniform;
} expect_t;

static const expect_t k_matrix[] = {
    { "telemetry_frame", k_tf, 8, WEFT_TELEMETRY_FRAME_SIZE,
      "TelemetryFrame", "WeftTelemetryFrame", 1, 0, 1, 0 },
    { "mcu_status", NULL, 0, WEFT_MCU_STATUS_SIZE,
      "McuStatus", "WeftMcuStatus", 0, 0, 0, 0 },
    { "camera_exposure", k_ce, 7, WEFT_CAMERA_EXPOSURE_SIZE,
      "CameraExposure", "WeftCameraExposure", 1, 1, 1, 1 },
    { "sensor_event", NULL, 0, WEFT_SENSOR_EVENT_SIZE,
      "SensorEvent", "WeftSensorEvent", 0, 0, 0, 0 },
    { "audio_peak", k_ap, 2, WEFT_AUDIO_PEAK_SIZE,
      "AudioPeak", "WeftAudioPeak", 1, 0, 0, 0 },
};
static const int k_matrix_n = (int)(sizeof k_matrix / sizeof k_matrix[0]);

// ---------------------------------------------------------------------------
// independent alignment engine (do NOT consult gpu/gpu_layout.c — a different
// code path is the entire point of this checker)
// ---------------------------------------------------------------------------

static size_t ru(size_t v, size_t a) { return (v + a - 1) / a * a; }

static bool type_layout(const char* ty, bool uniform, bool glsl_syntax,
                        size_t* align, size_t* size)
{
    if (strncmp(ty, "array<", 6) == 0) {          // WGSL array<T, N>
        char elem[64] = {0};
        unsigned n = 0;
        if (sscanf(ty, "array<%63[^,], %u>", elem, &n) != 2) return false;
        size_t ea, es;
        if (!type_layout(elem, uniform, glsl_syntax, &ea, &es)) return false;
        size_t stride = ru(ea, es);
        if (uniform) stride = ru(stride, 16);
        *align = uniform ? ru(ea, 16) : ea;
        *size = stride * n;
        return true;
    }
    const char* br = strchr(ty, '[');             // GLSL T[N]
    if (br && glsl_syntax) {
        char elem[64] = {0};
        unsigned n = 0;
        if (sscanf(br, "[%u]", &n) != 1) return false;
        size_t cl = (size_t)(br - ty);
        if (cl >= sizeof elem) return false;
        memcpy(elem, ty, cl);
        elem[cl] = '\0';
        size_t ea, es;
        if (!type_layout(elem, uniform, glsl_syntax, &ea, &es)) return false;
        size_t stride = ru(ea, es);
        if (uniform) stride = ru(stride, 16);
        *align = uniform ? ru(ea, 16) : ea;
        *size = stride * n;
        return true;
    }
    if (!strcmp(ty, "u32") || !strcmp(ty, "i32") || !strcmp(ty, "f32") ||
        !strcmp(ty, "uint") || !strcmp(ty, "int") || !strcmp(ty, "float")) {
        *align = 4; *size = 4; return true;
    }
    if (!strcmp(ty, "u64") || !strcmp(ty, "i64") || !strcmp(ty, "uint64_t") ||
        !strcmp(ty, "int64_t")) {
        *align = 8; *size = 8; return true;
    }
    if (!strcmp(ty, "f16")) { *align = 2; *size = 2; return true; }
    if (!strcmp(ty, "vec2<f32>") || !strcmp(ty, "vec2")) { *align = 8; *size = 8; return true; }
    if (!strcmp(ty, "vec3<f32>") || !strcmp(ty, "vec3")) { *align = 16; *size = 12; return true; }
    if (!strcmp(ty, "vec4<f32>") || !strcmp(ty, "vec4")) { *align = 16; *size = 16; return true; }
    if (!strcmp(ty, "mat4x4<f32>") || !strcmp(ty, "mat4")) { *align = 16; *size = 64; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// file parsing
// ---------------------------------------------------------------------------

static char* slurp(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static bool g_uniform_flavor = false;

typedef struct {
    char name[64];
    char type[80];
    bool is_pad;
    size_t off, size, align;
} chk_member;

static bool parse_struct(const char* src, const char* struct_name,
                         chk_member* members, int* nmembers, bool glsl_syntax,
                         size_t* struct_align, size_t* struct_size)
{
    char open[128];
    snprintf(open, sizeof open, "struct %.80s {", struct_name);
    const char* p = strstr(src, open);
    if (!p) return false;
    p += strlen(open);
    const char* end = strstr(p, "};");
    if (!end) return false;

    int n = 0;
    size_t cur = 0, maxa = 1;
    char line[256];
    const char* line_start = p;
    while (line_start < end) {
        const char* nl = memchr(line_start, '\n', (size_t)(end - line_start));
        size_t ll = nl ? (size_t)(nl - line_start) : (size_t)(end - line_start);
        if (ll >= sizeof line) { line_start = nl ? nl + 1 : end; continue; }
        memcpy(line, line_start, ll);
        line[ll] = '\0';
        line_start = nl ? nl + 1 : end;

        char mname[64] = {0}, mtype[80] = {0};
        if (glsl_syntax) {
            char a[64] = {0}, b[80] = {0};
            if (sscanf(line, " %63s %79s", a, b) != 2) continue;
            if (a[0] == '/' || a[0] == '#' || !strcmp(a, "struct")) continue;
            char* semi = strchr(b, ';');
            if (semi) *semi = '\0';
            snprintf(mname, sizeof mname, "%s", b);
            snprintf(mtype, sizeof mtype, "%s", a);
        } else {
            char* colon = strchr(line, ':');
            if (!colon) continue;
            char* c = line;
            while (*c == ' ' || *c == '\t') c++;
            size_t cl = (size_t)(colon - c);
            if (cl == 0 || cl >= sizeof mname) continue;
            memcpy(mname, c, cl);
            mname[cl] = '\0';
            // The type runs from after the colon to the trailing comma before
            // any "//" comment. Array types contain commas of their own
            // ("array<vec4<f32>, 2>"), so the LAST comma on the member line
            // is the separator — scan from the right.
            char* ty = colon + 1;
            while (*ty == ' ' || *ty == '\t') ty++;
            char* stop = strstr(ty, "//");
            char* endc = stop ? stop : ty + strlen(ty);
            while (endc > ty && (endc[-1] == ' ' || endc[-1] == '\t')) endc--;
            if (endc > ty && endc[-1] == ',') endc--;
            while (endc > ty && (endc[-1] == ' ' || endc[-1] == '\t')) endc--;
            size_t tl = (size_t)(endc - ty);
            if (tl == 0 || tl >= sizeof mtype) continue;
            memcpy(mtype, ty, tl);
            mtype[tl] = '\0';
        }
        if (!mname[0] || !mtype[0]) continue;

        size_t align = 0, size = 0;
        if (!type_layout(mtype, g_uniform_flavor, glsl_syntax, &align, &size)) return false;
        cur = ru(cur, align);
        if (n < 64) {
            snprintf(members[n].name, sizeof members[n].name, "%s", mname);
            snprintf(members[n].type, sizeof members[n].type, "%s", mtype);
            members[n].is_pad = (strncmp(mname, "weft_pad", 8) == 0);
            members[n].off = cur;
            members[n].size = size;
            members[n].align = align;
            n++;
        }
        if (align > maxa) maxa = align;
        cur += size;
    }
    *nmembers = n;
    *struct_align = maxa;
    *struct_size = ru(cur, maxa);
    return true;
}

static bool has_refusal(const char* src, const char* ir_name)
{
    char marker[96];
    snprintf(marker, sizeof marker, "WEFT-GPU-NOT-EXACT: %s", ir_name);
    return strstr(src, marker) != NULL;
}

static bool has_struct(const char* src, const char* struct_name)
{
    char open[128];
    snprintf(open, sizeof open, "struct %.80s {", struct_name);
    return strstr(src, open) != NULL;
}

// ---------------------------------------------------------------------------
// per-struct verification
// ---------------------------------------------------------------------------

static void check_fields_against_c(const chk_member* mem, int n, const expect_t* e,
                                   const char* label)
{
    int mism = 0;
    for (int i = 0; i < n && !mism; i++) {
        if (mem[i].is_pad) continue;
        size_t want = 0;
        bool known = false;
        for (uint32_t f = 0; f < e->nfields; f++) {
            if (!strcmp(e->fields[f].name, mem[i].name)) {
                want = e->fields[f].off;
                known = true;
                break;
            }
        }
        if (!known || mem[i].off != want) {
            printf("  %s: field %s computed @%zu (want %zu, known=%d)\n",
                   label, mem[i].name, mem[i].off, want, known);
            mism = 1;
        }
    }
    CHECK(mism == 0, "%s offsets == C offsetof (all %u fields, independently recomputed)",
          label, e->nfields);
}

static void check_one(const char* out_root, const expect_t* e, bool glsl)
{
    char path[512];
    const char* ext = glsl ? "glsl" : "wgsl";
    snprintf(path, sizeof path, "%s/%s/%s.%s", out_root, e->fixture, e->fixture, ext);
    char* src = slurp(path);
    if (!src) {
        printf("[gpu-check] FAIL: cannot open %s\n", path);
        g_fail++;
        return;
    }

    const char* sname = glsl ? e->glsl_name : e->wgsl_name;
    char uname[96];
    snprintf(uname, sizeof uname, "%sUniform", sname);
    char label[128];
    snprintf(label, sizeof label, "[%s] %s", ext, e->fixture);

    int want_storage = glsl ? e->glsl_storage : e->wgsl_storage;
    int want_uniform = glsl ? e->glsl_uniform : e->wgsl_uniform;

    // ---- storage flavor
    if (want_storage) {
        chk_member mem[64];
        int n = 0;
        size_t sa = 0, ss = 0;
        g_uniform_flavor = false;
        bool found = parse_struct(src, sname, mem, &n, glsl, &sa, &ss);
        CHECK(found, "%s storage struct present", label);
        if (found) {
            check_fields_against_c(mem, n, e, label);
            CHECK(ss == e->c_size, "%s storage struct size %zu == C sizeof %zu", label, ss, e->c_size);
        }
    } else {
        CHECK(has_refusal(src, e->fixture), "%s carries WEFT-GPU-NOT-EXACT marker (Law 4)", label);
        CHECK(!has_struct(src, sname), "%s refuses to emit any struct (total refusal)", label);
    }

    // ---- uniform flavor
    if (want_uniform) {
        chk_member mem[64];
        int n = 0;
        size_t sa = 0, ss = 0;
        g_uniform_flavor = true;
        bool found = parse_struct(src, uname, mem, &n, glsl, &sa, &ss);
        g_uniform_flavor = false;
        CHECK(found, "%s uniform struct present", label);
        if (found) {
            check_fields_against_c(mem, n, e, label);
            CHECK(ss == e->c_size, "%s uniform struct size %zu == C sizeof %zu", label, ss, e->c_size);
        }
    } else {
        CHECK(!has_struct(src, uname), "%s uniform variant absent", label);
    }

    free(src);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gpu_layout_check <out-root>\n");
        return 1;
    }
    const char* root = argv[1];

    for (int i = 0; i < k_matrix_n; i++) {
        check_one(root, &k_matrix[i], false);
        check_one(root, &k_matrix[i], true);
    }

    // proof shaders exist exactly for the storage-exact roots
    struct { const char* f; int want_wgsl; int want_comp; } shaders[] = {
        { "telemetry_frame", 1, 1 },
        { "mcu_status", 0, 0 },
        { "camera_exposure", 1, 1 },
        { "sensor_event", 0, 0 },
        { "audio_peak", 1, 0 }, // wgsl exact; glsl refused -> no .comp
    };
    for (size_t i = 0; i < sizeof shaders / sizeof shaders[0]; i++) {
        char path[512];
        char* got;
        snprintf(path, sizeof path, "%s/%s/%s_validate.wgsl", root, shaders[i].f, shaders[i].f);
        got = slurp(path);
        CHECK(shaders[i].want_wgsl ? got != NULL : got == NULL,
              "%s_validate.wgsl presence matches exactness", shaders[i].f);
        free(got);
        snprintf(path, sizeof path, "%s/%s/%s_validate.comp", root, shaders[i].f, shaders[i].f);
        got = slurp(path);
        CHECK(shaders[i].want_comp ? got != NULL : got == NULL,
              "%s_validate.comp presence matches GLSL exactness", shaders[i].f);
        free(got);
    }

    if (g_fail != 0) {
        printf("[gpu-check] FAILED: %d check(s) failed (%d passed)\n", g_fail, g_pass);
        return 1;
    }
    printf("[gpu-check] ALL PASS (%d checks — WGSL+GLSL independently recomputed vs C offsetof)\n", g_pass);
    return 0;
}
