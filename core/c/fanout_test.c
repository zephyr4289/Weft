// fanout_test.c — F-series battery for the C fan-out ring + FrameCursor tests
//
// Single-threaded protocol conformance (the multithreaded torture lives in
// fanout_runner.c — this file pins the SEMANTICS; that one pins the
// CONCURRENCY). Mirrors packages/core/test/fanout.test.ts (F-series) so the
// C port is pinned by the same contract the TS port is:
//
//   F1  geometry validation (payload %4, slot bounds, ring_bytes mismatch)
//   F2  begin/publish/claim roundtrip — fresh, seq, dropped=0, bytes intact
//   F3  drop accounting — publish 3 unseen frames, one claim, dropped=2
//   F4  telescoping identity: sum(dropped) == lastSeq - freshClaims, exact
//   F5  graceful skip — mid-overwrite with no newer frame skips the tick
//   F6  publish-without-begin is a detectable no-op (returns 0)
//   F7  attach to foreign/serialized ring — full handoff accounting
//   F8  debug_stats — latest, publishes, slot stamps
//   F9  heap lifecycle (_new/_free, the JNI/Dart-FFI binding pair) + ring accessor
//   FC1..FC6 — FrameCursor: first/behind/reset semantics on the kernel reader
//
// Build (see core/c/Makefile): make fanout-test        (-O2)
//                               make fanout-test-asan  (-fsanitize=address)
//                               make fanout-test-tsan  (-fsanitize=thread)
//                               make fanout-test-seq   (-DWEFT_FANOUT_SEQ_CST=1)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fanout.h"
#include "frame_cursor.h"
#include "weft.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            g_failures++;                                                   \
        } else {                                                            \
            fprintf(stdout, "ok: %s\n", msg);                               \
        }                                                                   \
    } while (0)

/// Deterministic payload word: weft_mix32-based, so the same generator is
/// available to the interop fixtures via the kernel's own pattern family.
static uint32_t tword(uint32_t seq, uint32_t w) {
    return weft_mix32(seq * 2654435761u + w);
}

static void fill_frame(weft_fanout_t* f, uint32_t seq, size_t words) {
    uint32_t buf[1024];
    if (words > 1024) { fprintf(stderr, "test payload too large\n"); exit(2); }
    for (size_t w = 0; w < words; w++) buf[w] = tword(seq, (uint32_t)w);
    uint8_t* c = weft_fanout_begin(f);
    CHECK(c != NULL, "begin returns a cursor");
    const int filled = weft_fanout_fill(f, buf, words * 4);
    CHECK(filled == (int)words, "fill writes every word");
}

static void expect_frame(const void* view, uint32_t seq, size_t words) {
    const uint32_t* v = (const uint32_t*)view;
    for (size_t w = 0; w < words; w++) {
        if (v[w] != tword(seq, (uint32_t)w)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "payload word %zu of frame %u intact", w, seq);
            CHECK(false, msg);
            return;
        }
    }
    CHECK(true, "payload words intact");
}

int main(void) {
    const size_t WORDS = 64; // 256-byte payloads
    const size_t PB = WORDS * 4;

    // ----- F1: geometry validation -----
    weft_fanout_t bad;
    CHECK(weft_fanout_init(&bad, 0, 4) == -1, "F1 payload_bytes=0 rejected");
    CHECK(weft_fanout_init(&bad, 6, 4) == -1, "F1 payload_bytes%4!=0 rejected");
    CHECK(weft_fanout_init(&bad, PB, 0) == -1, "F1 slot_count<2 rejected");
    CHECK(weft_fanout_init(&bad, PB, 1) == -1, "F1 slot_count=1 rejected");
    CHECK(weft_fanout_init(&bad, PB, WEFT_FANOUT_MAX_SLOTS + 1) == -1, "F1 slot_count>64 rejected");
    CHECK(weft_fanout_ring_bytes(PB, 4) == 16 + 8 * 4 + 4 * PB, "F1 ring_bytes formula (TS parity)");
    weft_fanout_reader_t badr;
    uint8_t dummy[8] = {0};
    CHECK(weft_fanout_reader_init(&badr, dummy, sizeof(dummy), PB, 4) == -1,
          "F1 reader geometry mismatch rejected");

    // ----- F2: roundtrip -----
    weft_fanout_t f;
    CHECK(weft_fanout_init(&f, PB, 4) == 0, "F2 init succeeds");
    weft_fanout_reader_t r;
    CHECK(weft_fanout_reader_init(&r, f.ring, weft_fanout_ring_bytes(PB, 4), PB, 4) == 0,
          "F2 reader init succeeds");
    const weft_fanout_claim_t* c = weft_fanout_claim(&r);
    CHECK(!c->fresh && c->seq == 0 && c->dropped == 0, "F2 claim before any publish: null frame");
    fill_frame(&f, 1, WORDS);
    CHECK(weft_fanout_publish(&f) == 1, "F2 publish returns frame 1");
    c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == 1 && c->dropped == 0, "F2 first publish: fresh, dropped=0");
    expect_frame(weft_fanout_view(&r), 1, WORDS);

    // ----- F3: drop accounting -----
    fill_frame(&f, 2, WORDS); (void)weft_fanout_publish(&f);
    fill_frame(&f, 3, WORDS); (void)weft_fanout_publish(&f);
    fill_frame(&f, 4, WORDS); (void)weft_fanout_publish(&f);
    c = weft_fanout_claim(&r);
    CHECK(c->fresh && c->seq == 4 && c->dropped == 2, "F3 three unseen publishes: dropped=2");
    expect_frame(weft_fanout_view(&r), 4, WORDS);

    // ----- F4: telescoping identity across an interleaved sequence -----
    // Own ring, from the null baseline, so the identity is exact from zero
    // (the same setup the TS F-series uses).
    {
        weft_fanout_t g;
        weft_fanout_reader_t rg;
        CHECK(weft_fanout_init(&g, PB, 4) == 0, "F4 ring init");
        CHECK(weft_fanout_reader_init(&rg, g.ring, weft_fanout_ring_bytes(PB, 4), PB, 4) == 0,
              "F4 reader init");
        uint64_t sum_dropped = 0, fresh_claims = 0;
        for (uint32_t seq = 1; seq <= 60; seq++) {
            fill_frame(&g, seq, WORDS);
            (void)weft_fanout_publish(&g);
            if (seq % 3 == 0) { // claim every third publish
                const weft_fanout_claim_t* cc = weft_fanout_claim(&rg);
                if (cc->fresh) { sum_dropped += cc->dropped; fresh_claims++; }
            }
        }
        const weft_fanout_claim_t* cc = weft_fanout_claim(&rg);
        if (cc->fresh) { sum_dropped += cc->dropped; fresh_claims++; }
        CHECK(sum_dropped == rg.last_seq - fresh_claims, "F4 telescoping identity exact");
        CHECK(rg.last_seq == 60, "F4 reader converged to the last frame");
        weft_fanout_stats_t st;
        weft_fanout_reader_stats(&rg, &st);
        CHECK(st.reads > 0 && st.fresh == fresh_claims, "F4 stats agree with the loop");
        weft_fanout_destroy(&g);
        weft_fanout_reader_destroy(&rg);
    }

    // ----- F5: graceful skip (mid-overwrite, no newer frame) -----
    {
        weft_fanout_t g;
        weft_fanout_reader_t rg;
        CHECK(weft_fanout_init(&g, PB, 2) == 0, "F5 M=2 ring init");
        CHECK(weft_fanout_reader_init(&rg, g.ring, weft_fanout_ring_bytes(PB, 2), PB, 2) == 0,
              "F5 reader init");
        fill_frame(&g, 1, WORDS);
        (void)weft_fanout_publish(&g);
        const weft_fanout_claim_t* cg = weft_fanout_claim(&rg);
        CHECK(cg->fresh && cg->seq == 1 && cg->dropped == 0, "F5 reader holds frame 1");
        fill_frame(&g, 2, WORDS); (void)weft_fanout_publish(&g); // slot 1
        fill_frame(&g, 3, WORDS); (void)weft_fanout_publish(&g); // slot 0 (latest)
        // begin() twice without publish: the second begin invalidates the
        // slot holding frame 3 (M=2: frame 3 lives in slot 0; frame 5 -> slot 0)
        (void)weft_fanout_begin(&g); // frame 4 -> slot 1 (frame 2's slot)
        (void)weft_fanout_begin(&g); // frame 5 -> slot 0 (frame 3's slot) — INVALIDATED
        cg = weft_fanout_claim(&rg);
        CHECK(!cg->fresh && cg->seq == 1, "F5 mid-overwrite: keeps last consistent frame");
        weft_fanout_stats_t st;
        weft_fanout_reader_stats(&rg, &st);
        CHECK(st.skipped_mid_overwrite == 1, "F5 skip counted, never silent");
        // Publish the begun frame and converge.
        (void)weft_fanout_publish(&g);
        cg = weft_fanout_claim(&rg);
        CHECK(cg->fresh && cg->seq == 5, "F5 converges after the overwrite completes");
        weft_fanout_destroy(&g);
        weft_fanout_reader_destroy(&rg);
    }

    // ----- F6: publish without begin is a no-op -----
    {
        weft_fanout_t g;
        CHECK(weft_fanout_init(&g, PB, 4) == 0, "F6 ring init");
        CHECK(weft_fanout_publish(&g) == 0, "F6 publish with no begin returns 0");
        fill_frame(&g, 1, WORDS);
        CHECK(weft_fanout_publish(&g) == 1, "F6 publish after begin returns 1");
        uint32_t odd[1] = {0};
        CHECK(weft_fanout_fill(&g, odd, 3) == -1, "F6 fill len%4!=0 rejected");
        weft_fanout_destroy(&g);
    }

    // ----- F7: full ring handoff (the in-process xlang story) -----
    {
        weft_fanout_t a;
        CHECK(weft_fanout_init(&a, PB, 4) == 0, "F7 producer init");
        const uint32_t N = 97;
        for (uint32_t seq = 1; seq <= N; seq++) {
            fill_frame(&a, seq, WORDS);
            (void)weft_fanout_publish(&a);
        }
        // "Serialize" the ring (in-process memcpy stands in for the file the
        // interop fixture writes — same bytes, same claim).
        const size_t rb = weft_fanout_ring_bytes(PB, 4);
        uint8_t* copy = (uint8_t*)malloc(rb);
        memcpy(copy, a.ring, rb);
        weft_fanout_reader_t rr;
        CHECK(weft_fanout_reader_init(&rr, copy, rb, PB, 4) == 0, "F7 reader attaches to the copy");
        const weft_fanout_claim_t* cr = weft_fanout_claim(&rr);
        CHECK(cr->fresh && cr->seq == N && cr->dropped == N - 1,
              "F7 handoff: fresh final frame, dropped = N-1");
        expect_frame(weft_fanout_view(&rr), N, WORDS);
        // Attach a WRITER to another copy and continue the frame numbering.
        uint8_t* copy2 = (uint8_t*)malloc(rb);
        memcpy(copy2, a.ring, rb);
        weft_fanout_t aw;
        memset(&aw, 0, sizeof(aw)); // attach expects an EMPTY broadcaster
        CHECK(weft_fanout_attach_writer(&aw, copy2, rb, PB, 4) == 0, "F7 writer attach");
        // Attach on a LIVE broadcaster must be refused (state intact, no leak).
        CHECK(weft_fanout_attach_writer(&aw, copy2, rb, PB, 4) == -1,
              "F7 second attach on the live broadcaster is refused");
        fill_frame(&aw, N + 1, WORDS);
        CHECK(weft_fanout_publish(&aw) == N + 1, "F7 attached writer continues numbering");
        weft_fanout_reader_t rr2;
        CHECK(weft_fanout_reader_init(&rr2, copy2, rb, PB, 4) == 0, "F7 second reader attach");
        const weft_fanout_claim_t* cr2 = weft_fanout_claim(&rr2);
        CHECK(cr2->fresh && cr2->seq == N + 1 && cr2->dropped == N,
              "F7 cross-producer accounting telescopes");
        weft_fanout_destroy(&a);
        weft_fanout_destroy(&aw);
        weft_fanout_reader_destroy(&rr);
        weft_fanout_reader_destroy(&rr2);
        free(copy);
        free(copy2);
    }

    // ----- F8: debug stats -----
    {
        weft_fanout_debug_t d;
        weft_fanout_debug_stats(&f, &d);
        CHECK(d.latest_seq == 4 && d.publishes == 4 && d.slot_count == 4,
              "F8 debug latest/publishes/slots");
        int stamped_ok = 1;
        for (unsigned k = 0; k < 4; k++) {
            if (d.slot_stamps[k] == 0 || d.slot_stamps[k] > 4) stamped_ok = 0;
        }
        CHECK(stamped_ok, "F8 slot stamps populated and in range");
    }

    // ----- F9: heap lifecycle (_new/_free) + ring accessor -----
    // The pair the JNI and Dart-FFI bridges bind: allocate+init and
    // destroy+free must each be ONE call (FFI-finalizer discipline), and
    // weft_fanout_ring() must expose the byte-layout contract base.
    {
        CHECK(weft_fanout_new(12, 0) == NULL && weft_fanout_new(12, 65) == NULL &&
              weft_fanout_new(6, 4) == NULL,
              "F9 new rejects bad geometry (slots, mod-4)");
        weft_fanout_t* fn = weft_fanout_new(64, 4);
        CHECK(fn != NULL, "F9 new allocates");
        const void* rp = weft_fanout_ring(fn);
        CHECK(rp != NULL, "F9 ring accessor non-NULL");
        // The layout contract, read raw: latestSeq at +0, publishes at +8.
        const _Atomic uint64_t* raw = (const _Atomic uint64_t*)rp;
        uint8_t* b = weft_fanout_begin(fn);
        for (int i = 0; i < 64; i++) b[i] = (uint8_t)(0xF0 ^ i);
        CHECK(weft_fanout_publish(fn) == 1, "F9 new ring publishes frame 1");
        CHECK(atomic_load_explicit(raw, memory_order_acquire) == 1,
              "F9 raw latestSeq == 1 at offset 0");
        CHECK(atomic_load_explicit(raw + 1, memory_order_relaxed) == 1,
              "F9 raw publishes == 1 at offset 8");
        weft_fanout_reader_t* rn = weft_fanout_reader_new(rp, weft_fanout_ring_bytes(64, 4), 64, 4);
        CHECK(rn != NULL, "F9 reader_new attaches to the ring pointer");
        CHECK(weft_fanout_claim(rn)->fresh && weft_fanout_claim(rn)->seq == 1,
              "F9 reader on the new/free ring claims frame 1");
        CHECK(weft_fanout_reader_new(rp, 8, 64, 4) == NULL,
              "F9 reader_new rejects ring_bytes mismatch");
        weft_fanout_reader_free(rn);
        weft_fanout_reader_free(NULL); // NULL-safe
        weft_fanout_free(fn);
        weft_fanout_free(NULL); // NULL-safe
        CHECK(1, "F9 free pair NULL-safe, no crash");
    }

    // ----- FC1..FC6: FrameCursor on the kernel reader -----
    {
        weft_t w;
        CHECK(weft_init(&w, 64) == 0, "FC kernel init");
        weft_frame_cursor_t cur;
        weft_frame_cursor_init(&cur);
        const weft_frame_sample_t* s = weft_frame_cursor_update(&cur, &w);
        CHECK(s->first && s->frames_behind == 0 && s->seq == 0,
              "FC1 null frame baseline: first, behind 0");

        // Publish 1; claim; behind 0 (advance from null).
        uint8_t* wc = weft_w_begin(&w);
        for (int i = 0; i < 64; i++) wc[i] = (uint8_t)(i + 1);
        (void)weft_publish(&w, 1, 64);
        s = weft_frame_cursor_update(&cur, &w);
        CHECK(s->first == false && s->frames_behind == 0 && s->seq == 1,
              "FC2 first real frame: behind 0, not first");

        // Publish 2,3,4 without claiming; claim; behind 2.
        for (uint32_t seq = 2; seq <= 4; seq++) {
            uint8_t* b = weft_w_begin(&w);
            for (int i = 0; i < 64; i++) b[i] = (uint8_t)(seq + i);
            (void)weft_publish(&w, seq, 64);
        }
        s = weft_frame_cursor_update(&cur, &w);
        CHECK(s->seq == 4 && s->frames_behind == 2, "FC3 gap accounting: behind 2");

        // Back-to-back claim against a quiet writer: the kernel hands the
        // reader its recycled previous hold (the buffer it returned to
        // `latest` at the last claim — frame 1's envelope here). Nothing new
        // was published, so nothing is counted as dropped; the decreasing
        // seq hits the RFC-0008 reset rule. Mirrors cursor.test.ts exactly.
        s = weft_frame_cursor_update(&cur, &w);
        CHECK(s->seq == 1 && s->frames_behind == 0,
              "FC4 idle claim: recycled hold, reset rule, behind 0");
        CHECK(cur.total_dropped == 2, "FC4 idle claim adds no drops");

        // FC5 (own kernel): a decreasing seq is a writer reset — no burst.
        {
            weft_t w2;
            CHECK(weft_init(&w2, 64) == 0, "FC5 kernel init");
            weft_frame_cursor_t c2;
            weft_frame_cursor_init(&c2);
            uint8_t* b = weft_w_begin(&w2);
            for (int i = 0; i < 64; i++) b[i] = (uint8_t)(i + 1);
            (void)weft_publish(&w2, 10, 64);
            s = weft_frame_cursor_update(&c2, &w2); // baseline: seq 10
            CHECK(s->seq == 10, "FC5 baseline seq 10");
            b = weft_w_begin(&w2);
            for (int i = 0; i < 64; i++) b[i] = (uint8_t)(i);
            (void)weft_publish(&w2, 1, 64); // writer reset (new epoch semantics)
            s = weft_frame_cursor_update(&c2, &w2);
            CHECK(s->seq == 1 && s->frames_behind == 0,
                  "FC5 decreasing seq resets accounting (no burst)");
            CHECK(c2.total_dropped == 0, "FC5 reset adds no drops");
            weft_destroy(&w2);
        }

        // FC6: accumulators, then reset -> next update is first again.
        CHECK(cur.total_dropped == 2 && cur.claims == 4,
              "FC6 accumulators: totalDropped=2, claims=4");
        weft_frame_cursor_reset(&cur);
        s = weft_frame_cursor_update(&cur, &w);
        CHECK(s->first && s->frames_behind == 0, "FC6 reset: next update is first again");
        CHECK(cur.claims == 1, "FC6 reset zeroed the counters");
        weft_destroy(&w);
    }

    weft_fanout_destroy(&f);
    weft_fanout_reader_destroy(&r);

    fprintf(stdout, "\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
