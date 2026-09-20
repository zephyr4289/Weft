// c_roundtrip.c — weftc-codegen Pillar 1 test binary (C side).
//
// The C header is the ABI GROUND TRUTH for the whole suite:
//   * static_asserts inside the headers already proved sizeof/offsetof at
//     compile time — this binary re-proves them against the WEFT_*_OFF_*
//     macros, then exercises the runtime surface:
//   * zero-copy cast helpers: happy path + all three refusal modes
//   * packed read/write through deliberately misaligned buffers (Law 3)
//   * bitfield accessors, f16 codec vectors + an EXHAUSTIVE 65536-pattern
//     f16 roundtrip proof (f16 -> f32 -> f16 is the identity for every
//     non-NaN bit pattern)
//   * SIMD batch schema validation (scalar build here; the runner also runs
//     an -mavx2 build and byte-compares its output)
//   * cross-language stage bins: `stage1 <dir>` writes the canonical frames
//     the Rust suite must read bit-exact; `verify-stage2 <dir>` re-reads the
//     frames Rust mutated and asserts every mutation.
//
// Usage: c_roundtrip selftest | stage1 <dir> | verify-stage2 <dir>

#define _POSIX_C_SOURCE 200809L

#include "telemetry_frame.h"
#include "mcu_status.h"
#include "camera_exposure.h"
#include "sensor_event.h"
#include "audio_peak.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_gates = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            g_gates++;                                                       \
            printf("[c-roundtrip] PASS: %s\n", name);                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("[c-roundtrip] FAIL: %s (line %d)\n", name, __LINE__);    \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// canonical frames (mirrored bit-exactly by tests/rust/tests/roundtrip.rs)
// ---------------------------------------------------------------------------

static void fill_telemetry(weft_telemetry_frame_t* f)
{
    memset(f, 0, sizeof(*f));
    f->schema_id = WEFT_TELEMETRY_FRAME_SCHEMA_ID;
    f->velocity = 12.5f;
    f->altitude = 5400.25f;
    f->accel[0] = 0.5f; f->accel[1] = -1.25f; f->accel[2] = 9.8f;
    f->pad0 = 0xDEADBEEFu;
    f->gyro[0] = 0.01f; f->gyro[1] = -0.02f; f->gyro[2] = 0.03f;
    f->baro_pressure = 101325.0f;
    f->quaternion[0] = 1.0f; f->quaternion[1] = 0.0f;
    f->quaternion[2] = 0.0f; f->quaternion[3] = 0.0f;
}

static void fill_mcu(weft_mcu_status_t* m)
{
    memset(m, 0, sizeof(*m));
    m->schema_id = WEFT_MCU_STATUS_SCHEMA_ID;
    weft_mcu_status_set_mode(m, 2u);
    weft_mcu_status_set_armed(m, 1u);
    weft_mcu_status_set_error_code(m, 0x5Au);
    m->vbus_mv = 11800u;
    m->temp_c = weft_f32_to_f16(42.5f);        // 0x5150
    m->gyro_temp_c = weft_f32_to_f16(41.75f);  // 0x5138
    m->motor_rpm[0] = 1200u; m->motor_rpm[1] = 1250u;
    m->motor_rpm[2] = 1180u; m->motor_rpm[3] = 1225u;
    m->crc32 = 0xCAFEBABEu;
}

static void fill_camera(weft_camera_exposure_t* c)
{
    memset(c, 0, sizeof(*c));
    c->schema_id = WEFT_CAMERA_EXPOSURE_SCHEMA_ID;
    weft_camera_exposure_set_ae_lock(c, 1u);
    weft_camera_exposure_set_sensor(c, 5u);
    c->gain = 1.75f;
    c->exposure_us = 8333.0f;
    c->position[0] = 1.0f; c->position[1] = 2.0f;
    c->position[2] = 3.0f; c->position[3] = 4.0f;
    for (int i = 0; i < 16; i++) {
        c->projection[i] = (i % 5 == 0) ? 1.0f : 0.0f; // identity-ish
    }
    c->projection[12] = 5.0f; c->projection[13] = 6.0f;
    c->projection[14] = 7.0f;
    c->roi[0][0] = 10.0f; c->roi[0][1] = 20.0f; c->roi[0][2] = 30.0f; c->roi[0][3] = 40.0f;
    c->roi[1][0] = 50.0f; c->roi[1][1] = 60.0f; c->roi[1][2] = 70.0f; c->roi[1][3] = 80.0f;
}

static void fill_sensor(weft_sensor_event_t* e, uint64_t ts)
{
    memset(e, 0, sizeof(*e));
    e->schema_id = WEFT_SENSOR_EVENT_SCHEMA_ID;
    e->timestamp_ns = ts;
    e->imu.x = -123; e->imu.y = 456; e->imu.z = -789; e->imu.pad = 0x4242u;
    e->quality = 0xC0FFEEu;
    e->crc32 = 0x1234ABCDu;
}

static void fill_audio(weft_audio_peak_t* a)
{
    memset(a, 0, sizeof(*a));
    a->schema_id = WEFT_AUDIO_PEAK_SCHEMA_ID;
    a->levels_db[0] = weft_f32_to_f16(-6.5f);
    a->levels_db[1] = weft_f32_to_f16(-12.25f);
    a->levels_db[2] = weft_f32_to_f16(-3.75f);
    a->levels_db[3] = weft_f32_to_f16(-20.0f);
}

// ---------------------------------------------------------------------------
// generic per-struct checks
// ---------------------------------------------------------------------------

// T = C type, TAG = macro tag, FILL = filler, FIELDCHK = per-field equality
static void test_telemetry(void)
{
    weft_telemetry_frame_t f, g;
    fill_telemetry(&f);
    weft_error_t err = WEFT_OK;

    CHECK(sizeof(weft_telemetry_frame_t) == (size_t)WEFT_TELEMETRY_FRAME_SIZE,
          "telemetry sizeof macro");
    CHECK(offsetof(weft_telemetry_frame_t, schema_id) == (size_t)WEFT_TELEMETRY_FRAME_OFF_SCHEMA_ID,
          "telemetry schema_id offset macro");
    CHECK(offsetof(weft_telemetry_frame_t, velocity) == (size_t)WEFT_TELEMETRY_FRAME_OFF_VELOCITY,
          "telemetry velocity offset macro (the Pillar 1 example ABI)");
    CHECK(offsetof(weft_telemetry_frame_t, quaternion) == (size_t)WEFT_TELEMETRY_FRAME_OFF_QUATERNION,
          "telemetry quaternion offset macro");

    // cast happy path
    const weft_telemetry_frame_t* v = weft_cast_telemetry_frame(&f, sizeof f, &err);
    CHECK(v == &f && err == WEFT_OK, "telemetry cast happy path");

    // refusals
    uint8_t buf[WEFT_TELEMETRY_FRAME_SIZE + 8];
    memcpy(buf, &f, sizeof f);
    CHECK(weft_cast_telemetry_frame(buf, sizeof f - 1, &err) == NULL && err == WEFT_ERR_SHORT_BUFFER,
          "telemetry cast short-buffer refusal");
    CHECK(weft_cast_telemetry_frame(buf + 1, sizeof f, &err) == NULL && err == WEFT_ERR_BAD_ALIGN,
          "telemetry cast misaligned refusal");
    buf[0] ^= 0xFF;
    CHECK(weft_cast_telemetry_frame(buf, sizeof f, &err) == NULL && err == WEFT_ERR_SCHEMA_MISMATCH,
          "telemetry cast schema refusal");
    buf[0] ^= 0xFF;

    // packed read/write through a MISALIGNED buffer (Law 3)
    uint8_t raw[WEFT_TELEMETRY_FRAME_SIZE + 1];
    CHECK(weft_telemetry_frame_write_packed(raw + 1, sizeof raw - 1, &f) == WEFT_OK,
          "telemetry packed write (misaligned dst)");
    CHECK(weft_telemetry_frame_read_packed(raw + 1, sizeof raw - 1, &g) == WEFT_OK,
          "telemetry packed read (misaligned src)");
    CHECK(g.velocity == 12.5f && g.altitude == 5400.25f, "telemetry packed scalars");
    CHECK(g.accel[0] == 0.5f && g.accel[1] == -1.25f && g.accel[2] == 9.8f, "telemetry packed vec3");
    CHECK(g.quaternion[0] == 1.0f && g.quaternion[3] == 0.0f, "telemetry packed vec4");
    CHECK(memcmp(&f, &g, sizeof f) == 0, "telemetry packed roundtrip is byte-identical");
    CHECK(weft_telemetry_frame_read_packed(raw + 1, 8, &g) == WEFT_ERR_SHORT_BUFFER,
          "telemetry packed read short refusal");
}

static void test_mcu(void)
{
    weft_mcu_status_t m;
    fill_mcu(&m);
    weft_error_t err = WEFT_OK;

    CHECK(m.flags == ((2u & 0x3u) | (1u << 4) | (0x5Au << 8)), "mcu bitfield packing");
    CHECK(weft_mcu_status_get_mode(&m) == 2u, "mcu get mode");
    CHECK(weft_mcu_status_get_armed(&m) == 1u, "mcu get armed");
    CHECK(weft_mcu_status_get_error_code(&m) == 0x5Au, "mcu get error_code");
    weft_mcu_status_set_mode(&m, 1u);
    CHECK(weft_mcu_status_get_mode(&m) == 1u && weft_mcu_status_get_error_code(&m) == 0x5Au,
          "mcu bitfield set preserves neighbors");
    weft_mcu_status_set_mode(&m, 2u);

    CHECK(m.temp_c == 0x5150u, "f16(42.5) == 0x5150");
    CHECK(m.gyro_temp_c == 0x5138u, "f16(41.75) == 0x5138");

    const weft_mcu_status_t* v = weft_cast_mcu_status(&m, sizeof m, &err);
    CHECK(v == &m && err == WEFT_OK, "mcu cast happy path");

    uint8_t raw[WEFT_MCU_STATUS_SIZE + 1];
    weft_mcu_status_t g;
    CHECK(weft_mcu_status_write_packed(raw + 1, sizeof raw - 1, &m) == WEFT_OK, "mcu packed write");
    CHECK(weft_mcu_status_read_packed(raw + 1, sizeof raw - 1, &g) == WEFT_OK, "mcu packed read");
    CHECK(g.motor_rpm[2] == 1180u && g.vbus_mv == 11800u && g.temp_c == 0x5150u,
          "mcu packed u16 array + f16 fields");
    CHECK(memcmp(&m, &g, sizeof m) == 0, "mcu packed roundtrip byte-identical");
}

static void test_camera(void)
{
    weft_camera_exposure_t c;
    fill_camera(&c);
    weft_error_t err = WEFT_OK;

    CHECK(c.mode == 0xBu, "camera bitfield packing (ae_lock|sensor<<1)");
    CHECK(weft_camera_exposure_get_sensor(&c) == 5u, "camera get sensor");
    CHECK(weft_camera_exposure_get_ae_lock(&c) == 1u, "camera get ae_lock");

    const weft_camera_exposure_t* v = weft_cast_camera_exposure(&c, sizeof c, &err);
    CHECK(v == &c && err == WEFT_OK, "camera cast happy path (align 16 header)");

    uint8_t raw[WEFT_CAMERA_EXPOSURE_SIZE + 1];
    weft_camera_exposure_t g;
    CHECK(weft_camera_exposure_write_packed(raw + 1, sizeof raw - 1, &c) == WEFT_OK, "camera packed write");
    CHECK(weft_camera_exposure_read_packed(raw + 1, sizeof raw - 1, &g) == WEFT_OK, "camera packed read");
    CHECK(g.projection[12] == 5.0f && g.roi[1][3] == 80.0f && g.position[2] == 3.0f,
          "camera packed mat4 + vec4-array + vec4");
    CHECK(memcmp(&c, &g, sizeof c) == 0, "camera packed roundtrip byte-identical");
}

static void test_sensor(void)
{
    weft_sensor_event_t e;
    fill_sensor(&e, 0x1122334455667788ULL);
    weft_error_t err = WEFT_OK;

    const weft_sensor_event_t* v = weft_cast_sensor_event(&e, sizeof e, &err);
    CHECK(v == &e && err == WEFT_OK, "sensor cast happy path");
    CHECK(e.imu.x == -123 && e.imu.y == 456 && e.imu.z == -789, "sensor embedded struct fields");

    uint8_t raw[WEFT_SENSOR_EVENT_SIZE + 1];
    weft_sensor_event_t g;
    CHECK(weft_sensor_event_write_packed(raw + 1, sizeof raw - 1, &e) == WEFT_OK, "sensor packed write (nested)");
    CHECK(weft_sensor_event_read_packed(raw + 1, sizeof raw - 1, &g) == WEFT_OK, "sensor packed read (nested)");
    CHECK(memcmp(&e, &g, sizeof e) == 0, "sensor packed roundtrip byte-identical");
}

static void test_audio(void)
{
    weft_audio_peak_t a;
    fill_audio(&a);
    weft_error_t err = WEFT_OK;

    const weft_audio_peak_t* v = weft_cast_audio_peak(&a, sizeof a, &err);
    CHECK(v == &a && err == WEFT_OK, "audio cast happy path");

    uint8_t raw[WEFT_AUDIO_PEAK_SIZE + 1];
    weft_audio_peak_t g;
    CHECK(weft_audio_peak_write_packed(raw + 1, sizeof raw - 1, &a) == WEFT_OK, "audio packed write");
    CHECK(weft_audio_peak_read_packed(raw + 1, sizeof raw - 1, &g) == WEFT_OK, "audio packed read");
    CHECK(g.levels_db[0] == a.levels_db[0] && g.levels_db[3] == a.levels_db[3],
          "audio packed f16 array");
    CHECK(memcmp(&a, &g, sizeof a) == 0, "audio packed roundtrip byte-identical");
}

// ---------------------------------------------------------------------------
// f16 codec: known vectors + EXHAUSTIVE 65536-pattern identity proof
// ---------------------------------------------------------------------------

static void test_f16(void)
{
    CHECK(weft_f32_to_f16(1.0f) == 0x3C00u, "f16 vector 1.0");
    CHECK(weft_f32_to_f16(0.5f) == 0x3800u, "f16 vector 0.5");
    CHECK(weft_f32_to_f16(-2.75f) == 0xC180u, "f16 vector -2.75");
    CHECK(weft_f32_to_f16(65504.0f) == 0x7BFFu, "f16 vector max normal");
    CHECK(weft_f32_to_f16(65520.0f) == 0x7C00u, "f16 RNE overflow -> inf");
    CHECK(weft_f32_to_f16(5.9604644775390625e-08f) == 0x0001u, "f16 vector min subnormal");
    CHECK(weft_f32_to_f16(8.8817841970012523e-016f) == 0x0000u, "f16 tie-to-even underflow");
    CHECK(weft_f32_to_f16(0.1f) == 0x2E66u, "f16 vector 0.1 -> 0x2E66");
    CHECK(weft_f32_to_f16(1.0f / 0.0f) == 0x7C00u, "f16 inf");
    uint16_t nanbits = weft_f32_to_f16(0.0f / 0.0f);
    CHECK((nanbits & 0x7C00u) == 0x7C00u && (nanbits & 0x03FFu) != 0u, "f16 nan payload");
    CHECK(weft_f16_to_f32(0x3C00u) == 1.0f, "f16 -> 1.0f");
    CHECK(weft_f16_to_f32(0x8000u) == 0.0f && signbit(weft_f16_to_f32(0x8000u)),
          "f16 -0.0 sign bit");
    CHECK(weft_f16_to_f32(0x7BFFu) == 65504.0f, "f16 max normal -> f32");

    // EXHAUSTIVE: f16 -> f32 -> f16 is the identity for every non-NaN pattern
    uint32_t bad = 0;
    for (uint32_t h = 0; h <= 0xFFFFu; h++) {
        uint16_t bits = (uint16_t)h;
        if (((bits >> 10) & 0x1Fu) == 0x1Fu) continue; // NaN/inf: payload not preserved
        float f = weft_f16_to_f32(bits);
        uint16_t back = weft_f32_to_f16(f);
        if (back != bits) {
            if (bad < 4) {
                printf("  mismatch: %04X -> %f -> %04X\n", bits, (double)f, back);
            }
            bad++;
        }
    }
    CHECK(bad == 0, "f16 exhaustive identity (65536 patterns, NaN excluded)");
}

// ---------------------------------------------------------------------------
// SIMD batch validation
// ---------------------------------------------------------------------------

static void test_batches(void)
{
    weft_sensor_event_t ev[8];
    for (int i = 0; i < 8; i++) fill_sensor(&ev[i], (uint64_t)i + 1);
    ev[5].schema_id ^= 1; // corrupt frame 5

    size_t ok = 0, bad_i = 99;
    weft_error_t rc = weft_validate_batch_sensor_event(ev, sizeof ev, &ok, &bad_i);
    CHECK(rc == WEFT_ERR_SCHEMA_MISMATCH && bad_i == 5 && ok == 5,
          "sensor batch finds first bad frame (index 5)");

    ev[5].schema_id ^= 1;
    rc = weft_validate_batch_sensor_event(ev, sizeof ev, &ok, &bad_i);
    CHECK(rc == WEFT_OK && ok == 8, "sensor batch all-good (8 frames)");

    rc = weft_validate_batch_sensor_event(ev, sizeof ev - 4, &ok, &bad_i);
    CHECK(rc == WEFT_ERR_SHORT_BUFFER, "sensor batch ragged length refusal");

    // width-32 schema batch (camera) — covers the u32 SIMD lanes
    weft_camera_exposure_t cs[16];
    for (int i = 0; i < 16; i++) fill_camera(&cs[i]);
    cs[13].schema_id ^= 1;
    rc = weft_validate_batch_camera_exposure(cs, sizeof cs, &ok, &bad_i);
    CHECK(rc == WEFT_ERR_SCHEMA_MISMATCH && bad_i == 13 && ok == 13,
          "camera batch (u32 schema) finds bad frame 13");
    cs[13].schema_id ^= 1;
    rc = weft_validate_batch_camera_exposure(cs, sizeof cs, &ok, &bad_i);
    CHECK(rc == WEFT_OK && ok == 16, "camera batch all-good (16 frames)");

    // mcu batch: 12 frames (>= 4 so the AVX2 build exercises packed lanes)
    weft_mcu_status_t ms[12];
    for (int i = 0; i < 12; i++) fill_mcu(&ms[i]);
    rc = weft_validate_batch_mcu_status(ms, sizeof ms, &ok, &bad_i);
    CHECK(rc == WEFT_OK && ok == 12, "mcu batch all-good (12 frames)");
}

// ---------------------------------------------------------------------------
// informational cast timing (sandbox-timed; NOT a gate)
// ---------------------------------------------------------------------------

static void timing_probe(void)
{
    static weft_telemetry_frame_t f;
    fill_telemetry(&f);
    volatile const weft_telemetry_frame_t* sink = NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    enum { N = 10000000 };
    for (int i = 0; i < N; i++) {
        weft_error_t e;
        sink = weft_cast_telemetry_frame(&f, sizeof f, &e);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void)sink;
    double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / N;
    printf("[c-roundtrip] INFO: weft_cast_telemetry_frame = %.2f ns/op "
           "[SANDBOX-TIMED x86_64, informational — not a gate]\n", ns);
}

// ---------------------------------------------------------------------------
// stage bins (cross-language bit-exact protocol)
// ---------------------------------------------------------------------------

static int write_all(const char* dir)
{
    char path[512];
    FILE* fp;

    weft_telemetry_frame_t tf; fill_telemetry(&tf);
    weft_mcu_status_t mc; fill_mcu(&mc);
    weft_camera_exposure_t ce; fill_camera(&ce);
    weft_sensor_event_t se; fill_sensor(&se, 0x1122334455667788ULL);
    weft_audio_peak_t ap; fill_audio(&ap);

    struct { const void* p; size_t n; const char* f; } bins[] = {
        { &tf, sizeof tf, "telemetry_frame.bin" },
        { &mc, sizeof mc, "mcu_status.bin" },
        { &ce, sizeof ce, "camera_exposure.bin" },
        { &se, sizeof se, "sensor_event.bin" },
        { &ap, sizeof ap, "audio_peak.bin" },
    };
    for (size_t i = 0; i < sizeof bins / sizeof bins[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, bins[i].f);
        fp = fopen(path, "wb");
        if (!fp) { perror(path); return -1; }
        fwrite(bins[i].p, 1, bins[i].n, fp);
        fclose(fp);
        printf("[c-roundtrip] wrote %s (%zu bytes)\n", path, bins[i].n);
    }
    return 0;
}

static int verify_stage2(const char* dir)
{
    char path[512];
    weft_error_t err;

    // telemetry
    weft_telemetry_frame_t tfb;
    {
        snprintf(path, sizeof path, "%s/telemetry_frame.bin", dir);
        FILE* fp = fopen(path, "rb");
        if (!fp || fread(&tfb, 1, sizeof tfb, fp) != sizeof tfb) { perror(path); return -1; }
        fclose(fp);
    }
    const weft_telemetry_frame_t* tf = weft_cast_telemetry_frame(&tfb, sizeof tfb, &err);
    CHECK(tf && err == WEFT_OK && tf->velocity == 99.5f && tf->quaternion[3] == -1.0f && tf->accel[0] == 2.5f,
          "stage2 telemetry: rust mutations read back bit-exact");

    // mcu
    weft_mcu_status_t mcb;
    {
        snprintf(path, sizeof path, "%s/mcu_status.bin", dir);
        FILE* fp = fopen(path, "rb");
        if (!fp || fread(&mcb, 1, sizeof mcb, fp) != sizeof mcb) { perror(path); return -1; }
        fclose(fp);
    }
    const weft_mcu_status_t* mc = weft_cast_mcu_status(&mcb, sizeof mcb, &err);
    CHECK(mc && err == WEFT_OK && weft_mcu_status_get_mode(mc) == 3u &&
          mc->temp_c == weft_f32_to_f16(-10.5f) && mc->motor_rpm[2] == 999u,
          "stage2 mcu: rust bitfield/f16/u16 mutations bit-exact");

    // camera
    weft_camera_exposure_t ceb;
    {
        snprintf(path, sizeof path, "%s/camera_exposure.bin", dir);
        FILE* fp = fopen(path, "rb");
        if (!fp || fread(&ceb, 1, sizeof ceb, fp) != sizeof ceb) { perror(path); return -1; }
        fclose(fp);
    }
    const weft_camera_exposure_t* ce = weft_cast_camera_exposure(&ceb, sizeof ceb, &err);
    CHECK(ce && err == WEFT_OK && ce->gain == 3.5f && ce->roi[1][0] == 1.0f && ce->roi[1][3] == 4.0f,
          "stage2 camera: rust mutations bit-exact");

    // sensor
    weft_sensor_event_t seb;
    {
        snprintf(path, sizeof path, "%s/sensor_event.bin", dir);
        FILE* fp = fopen(path, "rb");
        if (!fp || fread(&seb, 1, sizeof seb, fp) != sizeof seb) { perror(path); return -1; }
        fclose(fp);
    }
    const weft_sensor_event_t* se = weft_cast_sensor_event(&seb, sizeof seb, &err);
    CHECK(se && err == WEFT_OK && se->quality == 0x0BADBEEFu && se->imu.y == 777,
          "stage2 sensor: rust nested + scalar mutations bit-exact");

    // audio
    weft_audio_peak_t apb;
    {
        snprintf(path, sizeof path, "%s/audio_peak.bin", dir);
        FILE* fp = fopen(path, "rb");
        if (!fp || fread(&apb, 1, sizeof apb, fp) != sizeof apb) { perror(path); return -1; }
        fclose(fp);
    }
    const weft_audio_peak_t* ap = weft_cast_audio_peak(&apb, sizeof apb, &err);
    CHECK(ap && err == WEFT_OK && ap->levels_db[2] == weft_f32_to_f16(-1.5f),
          "stage2 audio: rust f16 mutation bit-exact");

    return g_fail == 0 ? 0 : -1;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "selftest";

    test_telemetry();
    test_mcu();
    test_camera();
    test_sensor();
    test_audio();
    test_f16();
    test_batches();
    timing_probe();

    if (strcmp(mode, "stage1") == 0 && argc > 2) {
        if (write_all(argv[2]) != 0) return 2;
    } else if (strcmp(mode, "verify-stage2") == 0 && argc > 2) {
        if (verify_stage2(argv[2]) != 0) {
            printf("[c-roundtrip] VERIFY-STAGE2 FAILED\n");
            return 1;
        }
    }

    if (g_fail != 0) {
        printf("[c-roundtrip] FAILED: %d gate(s) failed (%d passed)\n", g_fail, g_gates);
        return 1;
    }
    printf("[c-roundtrip] ALL PASS (%d gates)\n", g_gates);
    return 0;
}
