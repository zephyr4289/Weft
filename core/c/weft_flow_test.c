// weft_flow_test.c — RFC 0017 O-series: zero-alloc operator conformance.
//
// O1  map: identity + transform + refuse-whole overflow
// O2  filter: predicate + counting
// O3  tumbling window: exact emissions + flush
// O4  sliding window: growth then ring order
// O5  demux: routing, drop-by-decision, overflow accounting
// O6  zip: pairing, tolerance gap, latest-wins coalescing
// O7  pipeline end-to-end: 10k views, pinned output checksum, structural
//     zero-alloc (all state caller-declared)

#define _GNU_SOURCE
#include "weft_flow.h"
#include "weft.h"   // weft_pat — the canonical deterministic pattern

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0, g_fails = 0;
#define CK(cond, name)                                              \
    do {                                                            \
        g_checks++;                                                 \
        if (!(cond)) {                                              \
            g_fails++;                                              \
            printf("  FAIL %s:%d %s\n", __func__, __LINE__, name);  \
        }                                                           \
    } while (0)

static weft_flow_view mkview(uint32_t seq, uint64_t t_ns, const char* s) {
    weft_flow_view v;
    v.ptr = (const uint8_t*)s;
    v.len = (uint32_t)strlen(s);
    v.seq = seq;
    v.t_ns = t_ns;
    return v;
}

// deterministic map: XOR every byte with pat(seq, i) — pure, alloc-free
static size_t map_xor(void* ctx, const weft_flow_view* src, uint8_t* dst,
                      size_t dst_cap) {
    (void)ctx;
    if (src->len > dst_cap) return src->len;  // signal overflow
    for (uint32_t i = 0; i < src->len; i++) {
        dst[i] = src->ptr[i] ^ weft_pat(src->seq, i);
    }
    return src->len;
}
static int filter_even_seq(void* ctx, const weft_flow_view* v) {
    (void)ctx;
    return (v->seq % 2) == 0;
}
static int route_by_len(void* ctx, const weft_flow_view* v) {
    (void)ctx;
    return v->len > 4 ? 1 : 0;
}

static void t_o1_map(void) {
    weft_flow_view src = mkview(7, 100, "hello");
    uint8_t dst[16];
    weft_flow_view out;
    CK(weft_flow_map(&src, &out, dst, sizeof(dst), map_xor, NULL) == 0, "map ok");
    CK(out.seq == 7 && out.t_ns == 100 && out.len == 5, "identity metadata carried");
    int xor_ok = 1;
    for (uint32_t i = 0; i < 5; i++) {
        if (out.ptr[i] != ((const uint8_t*)"hello")[i] ^ 0) { /* placeholder */ }
        if (out.ptr[i] != (uint8_t)("hello"[i] ^ weft_pat(7, i))) xor_ok = 0;
    }
    CK(xor_ok, "bytes = src ^ pat(seq, i)");
    // overflow refused WHOLE
    uint8_t tiny[2];
    weft_flow_view out2;
    memset(&out2, 0, sizeof(out2));
    CK(weft_flow_map(&src, &out2, tiny, sizeof(tiny), map_xor, NULL) == -1,
       "overflow refused whole");
    CK(out2.ptr == NULL && out2.len == 0, "out untouched on refusal");
}

static void t_o2_filter(void) {
    weft_flow_view v1 = mkview(2, 0, "aa"), v2 = mkview(3, 0, "bbb");
    CK(weft_flow_filter(filter_even_seq, NULL, &v1) == 1, "keep even");
    CK(weft_flow_filter(filter_even_seq, NULL, &v2) == 0, "drop odd");
    CK(weft_flow_filter(NULL, NULL, &v1) == 1, "NULL fn = pass-through");
}

static void t_o3_tumbling(void) {
    weft_flow_view buf[3];
    weft_flow_window_t w;
    weft_flow_window_init(&w, buf, 3, 0);
    weft_flow_view out[3];
    char payload[16];
    size_t emissions = 0, flushed = 0;
    for (uint32_t i = 0; i < 10; i++) {
        snprintf(payload, sizeof(payload), "v%u", i);
        weft_flow_view v = mkview(i + 1, i * 10, payload);
        emissions += weft_flow_window_step(&w, &v, out, 3);
    }
    CK(emissions == 9, "3 full windows of 3 (9 views)");
    CK(w.count == 1, "1 view held (10 % 3)");
    flushed = weft_flow_window_flush(&w, out, 3);
    CK(flushed == 1, "flush drains the partial window");
    CK(out[0].seq == 10, "flush emits the oldest-held (seq 10)");
    CK(w.t_views == 10 && w.t_emitted == 4, "telemetry exact");
}

static void t_o4_sliding(void) {
    weft_flow_view buf[3];
    weft_flow_window_t w;
    weft_flow_window_init(&w, buf, 3, 1);
    weft_flow_view out[3];
    char payload[16];
    size_t e1 = 0, e2 = 0, e3 = 0;
    for (uint32_t i = 0; i < 3; i++) {
        snprintf(payload, sizeof(payload), "v%u", i);
        weft_flow_view v = mkview(i + 1, i, payload);
        e1 += weft_flow_window_step(&w, &v, out, 3);
    }
    CK(e1 == 1 + 2 + 3, "growth phase: 1, then 2, then 3 per step");
    CK(out[0].seq == 1 && out[1].seq == 2 && out[2].seq == 3, "oldest-first");
    weft_flow_view v4 = mkview(4, 3, "v3");
    e2 = weft_flow_window_step(&w, &v4, out, 3);
    CK(e2 == 3 && out[0].seq == 2 && out[2].seq == 4, "steady state: ring rotated");
    for (uint32_t i = 4; i < 10; i++) {
        snprintf(payload, sizeof(payload), "v%u", i);
        weft_flow_view v = mkview(i + 1, i, payload);
        e3 += weft_flow_window_step(&w, &v, out, 3);
    }
    CK(e3 == 6 * 3, "every step emits 3");
    CK(out[0].seq == 8 && out[2].seq == 10, "final window contents exact");
}

static void t_o5_demux(void) {
    weft_flow_view bufs[2][4];
    weft_flow_sink_t sinks[2];
    weft_flow_sink_t init = {bufs[0], 4, 0, 0, 0};
    sinks[0] = init;
    sinks[1] = init;
    sinks[1].buf = bufs[1];
    weft_flow_view out[4];
    const char* texts[8] = {"ab", "cdefgh", "ijk", "lmnop", "qr", "stuvwx", "yz", "a"};
    for (int i = 0; i < 8; i++) {
        weft_flow_view v = mkview((uint32_t)i + 1, (uint64_t)i, texts[i]);
        weft_flow_demux_step(sinks, 2, &v, route_by_len, NULL);
    }
    // shorts: ab,ijk,qr,yz,a = 5 into cap 4 -> 1 visible overflow
    CK(sinks[0].count == 4, "short sink holds cap 4");
    CK(sinks[0].t_overflow == 1, "short sink overflowed once (Law 1: counted)");
    // longs: cdefgh,lmnop,stuvwx = 3
    CK(sinks[1].count == 3, "long sink took 3");
    weft_flow_view big = mkview(99, 99, "overflow!");
    weft_flow_demux_step(sinks, 2, &big, route_by_len, NULL);
    CK(sinks[1].count == 4 && sinks[1].t_overflow == 0, "long sink took the 4th");
    weft_flow_view big2 = mkview(100, 100, "again-overflow");
    weft_flow_demux_step(sinks, 2, &big2, route_by_len, NULL);
    CK(sinks[1].t_overflow == 1, "overflow counted (visible loss, Law 1)");
    size_t n = weft_flow_sink_drain(&sinks[1], out, 4);
    CK(n == 4 && out[0].seq == 2 && out[3].seq == 99, "FIFO drain exact");
    // drop by decision
    weft_flow_view mid = mkview(100, 100, "abcde");
    CK(weft_flow_demux_step(sinks, 2, &mid, route_by_len, NULL) == 1, "mid routed 1");
}

static void t_o6_zip(void) {
    weft_flow_zip_t z;
    weft_flow_zip_init(&z, 1000);  // 1 µs tolerance
    weft_flow_view out[2];
    weft_flow_view a1 = mkview(1, 1000, "a1");
    weft_flow_view b1 = mkview(1, 1500, "b1");
    CK(weft_flow_zip_step(&z, 0, &a1, out) == 0, "A held");
    CK(weft_flow_zip_step(&z, 1, &b1, out) == 1, "pair emitted");
    CK(out[0].seq == 1 && out[1].seq == 1 && out[1].t_ns == 1500, "pair contents");
    // tolerance gap: refused whole, both dropped
    weft_flow_view a2 = mkview(2, 100000, "a2");
    weft_flow_view b2 = mkview(2, 900000, "b2");
    CK(weft_flow_zip_step(&z, 0, &a2, out) == 0, "a2 held");
    CK(weft_flow_zip_step(&z, 1, &b2, out) == -1, "gap refused whole");
    CK(z.t_gap == 1 && !z.has[0] && !z.has[1], "gap counted, halves released");
    // latest-wins coalescing: two A views before one B
    weft_flow_view a3 = mkview(3, 2000, "a3");
    weft_flow_view a4 = mkview(4, 2100, "a4");
    weft_flow_view b4 = mkview(4, 2200, "b4");
    weft_flow_zip_step(&z, 0, &a3, out);
    CK(weft_flow_zip_step(&z, 0, &a4, out) == 0 && z.t_coalesce == 1,
       "stale A coalesced (counted)");
    CK(weft_flow_zip_step(&z, 1, &b4, out) == 1 && out[0].seq == 4,
       "newest A paired");
}

static void t_o7_pipeline(void) {
    // filter(even seq) -> map(xor) -> tumbling window(4); 10k views;
    // pinned output checksum (FNV-1a over emitted (seq, first byte))
    weft_flow_view wbuf[4];
    weft_flow_window_t win;
    weft_flow_window_init(&win, wbuf, 4, 0);
    static uint8_t scratch[8 * 1024];
    weft_flow_pipeline_t p = {0};
    p.filter_fn = filter_even_seq;
    p.map_fn = map_xor;
    p.map_dst = scratch;
    p.map_cap = 1024;         // per-buffer size; ring of 8 buffers below
    p.map_ring = 8;           // scratch RING: windowed views stay intact
    p.window = &win;

    uint8_t payload[32];
    weft_flow_view out[4];
    uint64_t h = 0xcbf29ce484222325ull;
    uint64_t emitted_views = 0;
    for (uint32_t i = 1; i <= 10000; i++) {
        for (uint32_t j = 0; j < 8; j++) payload[j] = weft_pat(i, j);
        weft_flow_view v = {payload, 8, i, (uint64_t)i * 100};
        size_t n = weft_flow_pipeline_step(&p, &v, out, 4);
        for (size_t k = 0; k < n; k++) {
            h ^= out[k].seq & 0xff;
            h *= 0x100000001b3ull;
            h ^= out[k].ptr[0];
            h *= 0x100000001b3ull;
            emitted_views++;
        }
    }
    size_t flushed = weft_flow_window_flush(&win, out, 4);
    for (size_t k = 0; k < flushed; k++) {
        h ^= out[k].seq & 0xff;
        h *= 0x100000001b3ull;
        h ^= out[k].ptr[0];
        h *= 0x100000001b3ull;
        emitted_views++;
    }
    CK(p.t_in == 10000, "10k in");
    CK(p.t_filtered == 5000, "5000 filtered (odd seqs)");
    CK(emitted_views == 5000, "5000 emitted (1250 full windows x 4)");
    CK(h == 0x16a50e3abf678f35ull, "pinned pipeline checksum (parity vector)");
}

int main(void) {
    printf("== weft_flow_test — RFC 0017 O-series ==\n");
    t_o1_map();
    t_o2_filter();
    t_o3_tumbling();
    t_o4_sliding();
    t_o5_demux();
    t_o6_zip();
    t_o7_pipeline();
    printf("== O-series: %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
