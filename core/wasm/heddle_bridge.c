// heddle_bridge.c — zero-copy C-to-WASM bridge for the WHP2 hot plane.
//
// The bridge exposes the hot-plane's layout as PURE ARITHMETIC (byte
// offsets into a caller-provided memory base) plus fixed-order 64-bit
// atomic primitives, so the exact same source:
//   * compiles into WebAssembly linear memory (Emscripten
//     -pthread provides i64.atomic.*; wasm32 offsets fit u32),
//   * links natively into Node.js N-API addons or FFI consumers
//     (koffi / node-ffi-rs call this plain C ABI directly),
//   * is what packages/heddle-hotplane mirrors in JavaScript over a
//     SharedArrayBuffer + Atomics — verified byte-exact against the
//     golden plane dump.
//
// The bridge deliberately does NOT link the engine: it re-derives
// every offset from the normative constants and structs in
// heddle_hotplane.h (HOTPLANE-LAYOUT-V2 §3). heddle_bridge_test.c
// proves bridge-written sessions are read correctly by the engine
// (zero-copy alias semantics), and that every bridge offset equals
// the engine's own offsetof math.
//
// Emscripten build (from repo root):
//   emcc core/wasm/heddle_bridge.c -O2 -pthread -sMODULARIZE=1
//     -sEXPORTED_FUNCTIONS=_hedbridge_describe,...  -o dist/heddle_bridge.js
// (not exercised in this sandbox: no emcc on PATH — the native build
// plus the JS golden interop test carry the verification burden).
//
// Law 3: core/c/weft.{c,h} untouched. Law 1: zero allocation. Law 2:
// offsets are pure arithmetic on normative constants. Law 4: every
// refusal returns a named heddle_err_t code.

#include "heddle_hotplane.h"

#include <string.h>

/* ---- descriptor (the pointer descriptor contract, §9) ------------------- */

typedef struct heddle_bridge_desc {
    /* identity */
    uint32_t magic;
    uint32_t ver_major;
    uint32_t ver_minor;
    uint32_t header_size;
    /* static geometry (all immutable after create) */
    uint32_t layout_rev;
    uint32_t mode;
    uint32_t lane_count;
    uint32_t lane_stride;
    uint32_t sample_size;
    uint32_t slot_capacity;
    uint32_t stat_kind;
    uint32_t flags;
    uint64_t plane_size;
    uint64_t create_stamp_ns;
    /* ladder results (1 = verified) */
    uint32_t endian_ok;
    uint32_t crc_ok;
} heddle_bridge_desc_t;

/* ---- normative offsets (pure arithmetic from the header's constants) ---- */

uint64_t hedbridge_off_header(void)        { return 0; }
uint64_t hedbridge_off_epoch(void)         { return 0x40; }
uint64_t hedbridge_off_begin_seq(void)     { return 0x48; }
uint64_t hedbridge_off_commit_seq(void)    { return 0x50; }
uint64_t hedbridge_off_dirty_mask(void)    { return 0x58; }
uint64_t hedbridge_off_render_frame(void)  { return 0x60; }
uint64_t hedbridge_off_heartbeat(void)     { return 0x68; }
uint64_t hedbridge_off_dirty_transitions(void) { return 0x78; }

uint64_t hedbridge_off_lane_desc(uint32_t lane)
{
    return (uint64_t)WHP2_HEADER_SIZE + (uint64_t)lane * WHP2_LANE_DESC_SIZE;
}
uint64_t hedbridge_off_lane_min(void)        { return 0x00; }
uint64_t hedbridge_off_lane_max(void)        { return 0x08; }
uint64_t hedbridge_off_lane_current(void)    { return 0x10; }
uint64_t hedbridge_off_lane_begin_seq(void)  { return 0x18; }
uint64_t hedbridge_off_lane_commit_seq(void) { return 0x20; }
uint64_t hedbridge_off_lane_commit_cnt(void) { return 0x28; }
uint64_t hedbridge_off_lane_bbox(void)       { return 0x30; }

uint64_t hedbridge_off_lane_data(uint32_t lane, uint32_t lane_count,
                                 uint32_t lane_stride)
{
    return (uint64_t)WHP2_HEADER_SIZE +
           (uint64_t)lane_count * WHP2_LANE_DESC_SIZE +
           (uint64_t)lane * lane_stride;
}

uint64_t hedbridge_slot_stride(uint32_t sample_size)
{
    return hplane_slot_stride(sample_size);
}
uint64_t hedbridge_cell_stride(uint32_t sample_size)
{
    return hplane_cell_stride(sample_size);
}

uint64_t hedbridge_off_slot(uint32_t lane, uint32_t lane_count,
                            uint32_t lane_stride, uint32_t mode,
                            uint32_t sample_size, uint32_t slot_idx)
{
    uint64_t base = hedbridge_off_lane_data(lane, lane_count, lane_stride);
    uint64_t stride = (mode == WHP2_MODE_RING)
                          ? hplane_slot_stride(sample_size)
                          : hplane_cell_stride(sample_size);
    return base + (uint64_t)slot_idx * stride;
}
uint64_t hedbridge_off_slot_payload(void) { return WHP2_SLOT_HEADER_SIZE; }

/* ---- describe: identity + geometry + ladder probes ---------------------- */

int hedbridge_describe(const uint8_t *base, size_t len,
                       heddle_bridge_desc_t *out)
{
    if (!base || !out || len < WHP2_HEADER_SIZE) {
        return HEDDLE_E_ARG;
    }
    if (((uintptr_t)base & 63u) != 0) {
        return HEDDLE_E_ARG;
    }
    memset(out, 0, sizeof(*out));

    out->magic       = hplane_le32_get(base + 0x00);
    out->ver_major   = hplane_le16_get(base + 0x04);
    out->ver_minor   = hplane_le16_get(base + 0x06);
    out->header_size = hplane_le32_get(base + 0x08);
    if (out->magic != WHP2_MAGIC) {
        return HEDDLE_E_MAGIC;
    }
    if (out->ver_major != WHP2_LAYOUT_MAJOR) {
        return HEDDLE_E_VERSION;
    }
    if (out->header_size != WHP2_HEADER_SIZE ||
        hplane_le32_get(base + 0x10) != WHP2_LAYOUT_REV) {
        return HEDDLE_E_LAYOUT;
    }
    out->endian_ok =
        (hplane_le32_get(base + 0x70) == WHP2_ENDIAN_CANARY) ? 1u : 0u;
    if (!out->endian_ok) {
        return HEDDLE_E_ENDIAN;
    }
    out->crc_ok = (hplane_crc32(base + WHP2_STATIC_CRC_COVER_OFF,
                                WHP2_STATIC_CRC_COVER_LEN) ==
                   hplane_le32_get(base + 0x0C))
                      ? 1u
                      : 0u;
    if (!out->crc_ok) {
        return HEDDLE_E_CFG_CRC;
    }

    out->layout_rev     = hplane_le32_get(base + 0x10);
    out->mode           = hplane_le32_get(base + 0x14);
    out->lane_count     = hplane_le32_get(base + 0x18);
    out->lane_stride    = hplane_le32_get(base + 0x1C);
    out->sample_size    = hplane_le32_get(base + 0x20);
    out->slot_capacity  = hplane_le32_get(base + 0x24);
    out->plane_size     = hplane_le64_get(base + 0x28);
    out->create_stamp_ns = hplane_le64_get(base + 0x30);
    out->stat_kind      = hplane_le32_get(base + 0x38);
    out->flags          = hplane_le32_get(base + 0x3C);

    if ((uint64_t)len < out->plane_size) {
        return HEDDLE_E_REGION_SIZE;
    }
    return HEDDLE_OK;
}

/* ---- zero-copy atomic primitives (fixed orders; u64 at 8-aligned offs) ---
 * These are the exact operations the JS package mirrors with Atomics
 * on a BigUint64Array view of the same SharedArrayBuffer.            */

uint64_t hedbridge_a_load_acq64(const uint8_t *base, uint64_t off)
{
    return __atomic_load_n((const uint64_t *)(const void *)(base + off),
                           __ATOMIC_ACQUIRE);
}
uint64_t hedbridge_a_load_rel64(const uint8_t *base, uint64_t off)
{
    return __atomic_load_n((const uint64_t *)(const void *)(base + off),
                           __ATOMIC_RELAXED);
}
void hedbridge_a_store_rel64(uint8_t *base, uint64_t off, uint64_t v)
{
    __atomic_store_n((uint64_t *)(void *)(base + off), v, __ATOMIC_RELAXED);
}
void hedbridge_fence_release(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
uint64_t hedbridge_a_or64(uint8_t *base, uint64_t off, uint64_t bits)
{
    return __atomic_fetch_or((uint64_t *)(void *)(base + off), bits,
                             __ATOMIC_ACQ_REL);
}
uint64_t hedbridge_a_exchange64(uint8_t *base, uint64_t off, uint64_t v)
{
    return __atomic_exchange_n((uint64_t *)(void *)(base + off), v,
                               __ATOMIC_ACQ_REL);
}
uint64_t hedbridge_a_fetchadd64(uint8_t *base, uint64_t off, uint64_t v)
{
    return __atomic_fetch_add((uint64_t *)(void *)(base + off), v,
                              __ATOMIC_ACQ_REL);
}

/* ---- payload copies (8-aligned chunked, exactly like the engine) -------- */

void hedbridge_copy_from(void *dst, const uint8_t *base, uint64_t off,
                        uint32_t len)
{
    const uint8_t *s = base + off;
    uint8_t *d = (uint8_t *)dst;
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w = __atomic_load_n(
            (const uint64_t *)(const void *)(s + i), __ATOMIC_RELAXED);
        memcpy(d + i, &w, 8);
    }
    for (; i < len; i++) {
        d[i] = __atomic_load_n(s + i, __ATOMIC_RELAXED);
    }
}
void hedbridge_copy_to(uint8_t *base, uint64_t off, const void *src,
                      uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = base + off;
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        memcpy(&w, s + i, 8);
        __atomic_store_n((uint64_t *)(void *)(d + i), w, __ATOMIC_RELAXED);
    }
    for (; i < len; i++) {
        __atomic_store_n(d + i, s[i], __ATOMIC_RELAXED);
    }
}

/* ---- full two-store session driven ENTIRELY through the bridge ----------
 * Proves the bridge is a faithful zero-copy alias: a session written
 * with these primitives is read consistently by the engine's own
 * cell_read/lane readers. session_in/out let callers chain monotonic
 * session ids across calls (the caller owns the id, like the ctx).  */

int hedbridge_session_write(uint8_t *base, size_t len, uint32_t lane,
                            uint32_t cell, const void *sample,
                            uint32_t sample_len, uint64_t *session_io)
{
    if (!base || !sample || !session_io) {
        return HEDDLE_E_ARG;
    }
    heddle_bridge_desc_t d;
    int rc = hedbridge_describe(base, len, &d);
    if (rc != HEDDLE_OK) {
        return rc;
    }
    if (d.mode != WHP2_MODE_STATE) {
        return HEDDLE_E_MODE;
    }
    if (lane >= d.lane_count) {
        return HEDDLE_E_LANE_OVERFLOW;
    }
    if (cell >= d.slot_capacity) {
        return HEDDLE_E_CAPACITY;
    }
    if (sample_len == 0 || sample_len > d.sample_size) {
        return HEDDLE_E_PAYLOAD;
    }

    uint64_t v = *session_io + 1;

    /* plane pair open (store #1) */
    hedbridge_a_store_rel64(base, hedbridge_off_begin_seq(), v);
    hedbridge_fence_release();

    /* lane pair open (per-lane two-store, same session id) */
    uint64_t ld_off = hedbridge_off_lane_desc(lane);
    hedbridge_a_store_rel64(base, ld_off + hedbridge_off_lane_begin_seq(), v);
    hedbridge_fence_release();

    /* payload: ring slots carry the 16-byte two-store header, state
     * cells carry payload DIRECTLY at the cell base (no header). */
    uint64_t cell_off =
        hedbridge_off_slot(lane, d.lane_count, d.lane_stride, d.mode,
                           d.sample_size, cell);
    uint64_t pay_off = (d.mode == WHP2_MODE_RING)
                           ? cell_off + WHP2_SLOT_HEADER_SIZE
                           : cell_off;
    hedbridge_copy_to(base, pay_off, sample, sample_len);

    /* lane pair close (counter before COMMIT) */
    hedbridge_a_fetchadd64(base, ld_off + hedbridge_off_lane_commit_cnt(), 1);
    hedbridge_fence_release();
    hedbridge_a_store_rel64(base, ld_off + hedbridge_off_lane_commit_seq(), v);

    /* dirty signal flush (one 0->1 flush + transition accounting) */
    uint64_t old = hedbridge_a_or64(base, hedbridge_off_dirty_mask(),
                                    1ull << lane);
    uint64_t risen = (1ull << lane) & ~old;
    if (risen) {
        hedbridge_a_fetchadd64(base, hedbridge_off_dirty_transitions(), 1);
    }

    /* plane pair close (store #2 — COMMIT, final) */
    hedbridge_fence_release();
    hedbridge_a_store_rel64(base, hedbridge_off_commit_seq(), v);

    *session_io = v;
    return HEDDLE_OK;
}
