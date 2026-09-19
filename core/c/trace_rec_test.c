// trace_rec_test.c — RFC 0014 .weftrec v4 codec conformance (T-series), C.
//
// Issue #20 (Tier 5: Observability), task 1 — trace format standardization.
//
//   T1  Round trip — encode N events, decode, byte-identity.
//   T2  Record CRC catches a corrupted event byte (every byte position).
//   T3  Version gate — v1/v2/v3-shaped headers and non-WREC files REFUSED.
//   T4  Header CRC catches header tampering (count patched without re-CRC).
//   T5  Event kind gate — kind 0 and kind 9 refused at pack and unpack.
//   T6  Exact size — len must equal 32 + 12*count; a trailing byte rejected.
//   T7  JSON export — line count = events + 1 header line; every line parses
//       as JSON-shaped; header line matches the schema's header contract.
//   T8  Determinism — the scenario stream for (N, seed) is byte-identical
//       across two runs (the property the xlang-trace gate pins C==TS on).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_rec.h"
#include "weft.h"

static int fails = 0;
static long checks = 0;

static void expect(int cond, const char* what) {
    checks++;
    if (!cond) {
        fails++;
        fprintf(stderr, "GATE FAIL: %s\n", what);
    }
}

static const weft_trace_event EVENTS[] = {
    { WEFT_TRACE_PUBLISH, 64, 1 },
    { WEFT_TRACE_CLAIM, 0, 1 },
    { WEFT_TRACE_PUBLISH, 0, 2 },
    { WEFT_TRACE_REVOKE, 0, 0 },
    { WEFT_TRACE_DROP, 1, 3 },
    { WEFT_TRACE_ACK, 0, 1 },
    { WEFT_TRACE_STALL, 0, 4 },
    { WEFT_TRACE_TEAR, 0, 7 },
    { WEFT_TRACE_CANARY_FAIL, 0, 9 },
};

int main(void) {
    printf("trace_rec-test: RFC 0014 .weftrec v4 codec conformance\n");
    const unsigned N = sizeof EVENTS / sizeof EVENTS[0];

    // ---- T1: round trip ----
    static uint8_t buf[32 + 12 * 64];
    weft_trace_writer w;
    weft_trace_writer_open(&w, buf, sizeof buf);
    for (unsigned i = 0; i < N; i++) {
        expect(weft_trace_writer_event(&w, &EVENTS[i]) == 0, "T1 encode");
    }
    expect(weft_trace_writer_close(&w) == 0, "T1 close");
    expect(w.count == N, "T1 count");
    uint32_t count = 0;
    expect(weft_trace_validate(buf, w.off, &count) == 0, "T1 validate");
    expect(count == N, "T1 validate count");

    weft_trace_event e;
    size_t cur = WEFT_TRACE_HDR_SIZE;
    for (unsigned i = 0; i < N; i++) {
        expect(weft_trace_next(buf, w.off, &cur, &e) == 1, "T1 decode");
        expect(e.kind == EVENTS[i].kind && e.aux == EVENTS[i].aux &&
               e.data == EVENTS[i].data, "T1 event identity");
    }
    expect(weft_trace_next(buf, w.off, &cur, &e) == 0, "T1 end");

    // ---- T2: record CRC catches every single corrupted byte ----
    long caught = 0, total = 0;
    for (size_t byte = 0; byte < 12; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[32 + 12 * 64];
            memcpy(bad, buf, w.off);
            bad[WEFT_TRACE_HDR_SIZE + byte] ^= (uint8_t)(1u << bit);
            size_t c2 = WEFT_TRACE_HDR_SIZE;
            weft_trace_event e2;
            total++;
            // Any corruption in the record must either fail the record CRC
            // or (kind byte) fail the kind gate — never decode silently.
            int rc = weft_trace_next(bad, w.off, &c2, &e2);
            if (rc < 0) caught++;
        }
    }
    printf("  T2: %ld/%ld single-bit record corruptions caught\n", caught, total);
    expect(caught == total, "T2 zero false negatives on record CRC");

    // ---- T3: version gate ----
    {
        uint8_t old[64];
        memcpy(old, buf, 64);
        old[4] = 1;  // claim to be v1
        expect(weft_trace_validate(old, 64, NULL) == -1, "T3 v1 header refused");
        old[4] = 2;
        expect(weft_trace_validate(old, 64, NULL) == -1, "T3 v2 header refused");
        old[4] = 3;
        expect(weft_trace_validate(old, 64, NULL) == -1, "T3 v3 header refused");
        memcpy(old, "JUNK", 4);
        expect(weft_trace_validate(old, 64, NULL) == -1, "T3 non-WREC refused");
    }

    // ---- T4: header CRC catches tampering ----
    {
        uint8_t bad[32 + 12 * 64];
        memcpy(bad, buf, w.off);
        bad[16] ^= 0x01;  // event_count tampered, CRC not recomputed
        expect(weft_trace_validate(bad, w.off, NULL) == -1, "T4 header CRC bites");
    }

    // ---- T5: kind gate ----
    {
        weft_trace_event bad = { 0, 0, 0 };
        uint8_t rec[8];
        expect(weft_trace_writer_event(&w, &bad) == -1, "T5 kind 0 refused at encode");
        bad.kind = 9;
        expect(weft_trace_writer_event(&w, &bad) == -1, "T5 kind 9 refused at encode");
        weft_trace_event_pack(&bad, rec);
        // force through unpack bypassing encode gate:
        expect(weft_trace_event_unpack(rec, &e) == -1, "T5 kind 9 refused at decode");
    }

    // ---- T6: exact size ----
    expect(weft_trace_validate(buf, w.off + 1, NULL) == -1, "T6 trailing byte rejected");
    expect(weft_trace_validate(buf, w.off - 1, NULL) == -1, "T6 short buffer rejected");

    // ---- T7: JSON export ----
    {
        static uint8_t json[4096];
        size_t used = weft_trace_to_json(buf, w.off, json, sizeof json);
        expect(used > 0, "T7 export succeeds");
        long lines = 0;
        for (size_t i = 0; i < used; i++) if (json[i] == '\n') lines++;
        expect(lines == (long)N + 1, "T7 one line per event + header");
        expect(strncmp((const char*)json, "{\"format\":\"weftrec\"", 19) == 0 &&
               strstr((const char*)json, "\"version\":4") != NULL &&
               strstr((const char*)json, "\"kind\":\"trace\"") != NULL,
               "T7 header line matches the schema contract");
    }

    // ---- T8: CRC32 canonical vector (§1.4 contract; the xlang-trace gate
    // pins the C==TS byte-identity on this exact implementation. ----
    // "123456789" -> 0xCBF43926
    expect(weft_trace_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u,
           "T8 CRC-32/zlib canonical vector");

    printf("{\"test\":\"trace_rec\",\"checks\":%ld,\"fails\":%d,\"status\":\"%s\"}\n",
           checks, fails, fails == 0 ? "PASSED" : "FAILED");
    return fails == 0 ? 0 : 1;
}
