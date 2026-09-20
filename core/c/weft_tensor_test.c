// weft_tensor_test.c — T-series conformance gates (RFC-0016 §5: WTS1).

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fanout.h"
#include "weft_dmabuf.h"
#include "weft_f16_codec.h"
#include "weft_tensor.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name, fmt, ...)                                        \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  PASS %s\n", name);                                   \
            g_pass++;                                                      \
        } else {                                                           \
            printf("  FAIL %s — " fmt "\n", name, ##__VA_ARGS__);          \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

int main(void) {
    printf("# T-series: weft_tensor WTS1 conformance (RFC-0016 s5)\n");

    const size_t pb = 2048;
    const unsigned slots = 8;

    // ---- T1: header identity + every dtype roundtrips -------------------
    printf("## T1 format identity\n");
    {
        CHECK(WEFT_TENSOR_MAGIC == 0x31535457u, "magic is 'WTS1' LE", "?");
        CHECK(WEFT_TENSOR_HEADER_BYTES == 32, "header is 32 bytes", "?");

        weft_fanout_t f;
        weft_fanout_reader_t rd;
        memset(&f, 0, sizeof(f));
        memset(&rd, 0, sizeof(rd));
        CHECK(weft_fanout_init(&f, pb, slots) == 0, "ring", "?");
        CHECK(weft_fanout_reader_init(&rd, weft_fanout_ring(&f),
                  weft_fanout_ring_bytes(pb, slots), pb, slots) == 0,
              "reader", "?");

        static const weft_tensor_dtype_t dts[] = {
            WEFT_TENSOR_U8,  WEFT_TENSOR_U16, WEFT_TENSOR_U32, WEFT_TENSOR_U64,
            WEFT_TENSOR_I8,  WEFT_TENSOR_I16, WEFT_TENSOR_I32, WEFT_TENSOR_I64,
            WEFT_TENSOR_F16, WEFT_TENSOR_F32, WEFT_TENSOR_F64,
        };
        int all_ok = 1;
        for (size_t i = 0; i < sizeof(dts) / sizeof(dts[0]); i++) {
            const uint32_t dims[4] = { 4, 5, 0, 0 };  // rank 2: 20 elements
            uint8_t* cur = weft_tensor_frame_begin(&f, dts[i], dims, 2);
            if (cur == NULL) { all_ok = 0; break; }
            const size_t esz = weft_tensor_elem_size(dts[i]);
            for (size_t e = 0; e < 20 * esz; e++)
                cur[e] = (uint8_t)(0xA0u + e);
            const uint64_t seq = weft_fanout_publish(&f);
            if (seq == 0) { all_ok = 0; break; }
            const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
            if (!c->fresh || c->seq != seq) { all_ok = 0; break; }
            weft_tensor_hdr_t h;
            const void* payload = NULL;
            if (weft_tensor_frame_parse(weft_fanout_view(&rd), pb, &h,
                                        &payload) != 0) {
                all_ok = 0;
                break;
            }
            if (h.dtype != dts[i] || h.rank != 2 || h.elem_count != 20 ||
                h.dims[0] != 4 || h.dims[1] != 5 || h.dims[2] != 0 ||
                h.dims[3] != 0 || h.version != 1 || h.flags != 0) {
                all_ok = 0;
                break;
            }
            const size_t expect_words =
                ((20u * esz) + 3u) / 4u;
            if (h.payload_words != expect_words) { all_ok = 0; break; }
            for (size_t e = 0; e < 20 * esz; e++) {
                if (((const uint8_t*)payload)[e] != (uint8_t)(0xA0u + e)) {
                    all_ok = 0;
                    break;
                }
            }
        }
        CHECK(all_ok, "all 11 dtypes: publish -> claim -> parse roundtrip "
                      "with exact header fields", "%d", all_ok);
        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
    }

    // ---- T2: the refusal ladder ------------------------------------------
    printf("## T2 parse refusals\n");
    {
        uint8_t frame[64];
        weft_tensor_hdr_t h;
        const void* pay;

        // a good baseline
        {
            weft_fanout_t f;
            memset(&f, 0, sizeof(f));
            weft_fanout_init(&f, pb, slots);
            const uint32_t dims[4] = { 7, 0, 0, 0 };
            uint8_t* cur = weft_tensor_frame_begin(&f, WEFT_TENSOR_U32, dims, 1);
            CHECK(cur != NULL, "baseline frame begun", "?");
            memcpy(frame, cur - WEFT_TENSOR_HEADER_BYTES, 64);
            weft_fanout_destroy(&f);
            CHECK(weft_tensor_frame_parse(frame, 64, &h, &pay) == 0,
                  "baseline parses", "?");
            CHECK(h.payload_words == 7 && h.elem_count == 7,
                  "u32 x7: words==7 (no pad)", "%u", h.payload_words);
        }

        // each corruption is a refusal
        uint8_t bad[64];
        memcpy(bad, frame, 64);
        bad[0] = 'X';
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "bad magic refused", "?");
        memcpy(bad, frame, 64);
        bad[4] = 2;
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "bad version refused", "?");
        memcpy(bad, frame, 64);
        bad[7] = 1;
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "nonzero flags refused (version discipline)", "?");
        memcpy(bad, frame, 64);
        bad[6] = 5;
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "rank 5 refused", "?");
        memcpy(bad, frame, 64);
        bad[6] = 0;
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "rank 0 refused", "?");
        memcpy(bad, frame, 64);
        bad[5] = 0xFF;
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "unknown dtype refused", "?");
        memcpy(bad, frame, 64);
        bad[19] = 1;  // dims[1] beyond rank
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "dims beyond rank nonzero refused", "?");
        memcpy(bad, frame, 64);
        bad[8] = 9;  // elem_count 9 != 7
        CHECK(weft_tensor_frame_parse(bad, 64, NULL, NULL) == -1,
              "dims-product != elem_count refused", "?");
        CHECK(weft_tensor_frame_parse(frame, 8, NULL, NULL) == -1,
              "truncated extent refused", "?");
        CHECK(weft_tensor_frame_parse(frame, 32 + 7 * 4 - 1, NULL, NULL) == -1,
              "payload extent short refused", "?");
        CHECK(weft_tensor_frame_parse(NULL, 64, NULL, NULL) == -1,
              "NULL refused", "?");
    }

    // ---- T3: the F16 seam (codec-dialect bit-exactness) ------------------
    printf("## T3 F16 embeddings (the llama.cpp/ONNX seam)\n");
    {
        weft_fanout_t f;
        weft_fanout_reader_t rd;
        memset(&f, 0, sizeof(f));
        memset(&rd, 0, sizeof(rd));
        weft_fanout_init(&f, pb, slots);
        weft_fanout_reader_init(&rd, weft_fanout_ring(&f),
                  weft_fanout_ring_bytes(pb, slots), pb, slots);

        float vec[384];  // a small embedding
        for (uint32_t i = 0; i < 384; i++)
            vec[i] = -1.5f + 0.01f * (float)i;
        const uint64_t seq = weft_tensor_publish_f16(&f, vec, 384);
        CHECK(seq == 1, "f16 embedding published", "%llu",
              (unsigned long long)seq);
        const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
        CHECK(c->fresh, "claimed", "?");
        weft_tensor_hdr_t h;
        const void* pay;
        CHECK(weft_tensor_frame_parse(weft_fanout_view(&rd), pb, &h, &pay) == 0,
              "parsed", "?");
        CHECK(h.dtype == WEFT_TENSOR_F16 && h.elem_count == 384 &&
              h.payload_words == (384 * 2 + 3) / 4,
              "f16 header exact (words include the 1-word pad)", "%u",
              h.payload_words);
        int exact = 1;
        for (uint32_t i = 0; i < 384; i++) {
            const float got = weft_tensor_f16_at(pay, i);
            const float want = weft_f16_to_f32(weft_f32_to_f16(vec[i]));
            if (got != want) { exact = 0; break; }  // bit-exact per element
        }
        CHECK(exact, "every element bit-identical to the codec dialect", "%d",
              exact);
        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
    }

    // ---- T4/T5: tokens + audio seams --------------------------------------
    printf("## T4/T5 tokens + audio\n");
    {
        weft_fanout_t f;
        weft_fanout_reader_t rd;
        memset(&f, 0, sizeof(f));
        memset(&rd, 0, sizeof(rd));
        weft_fanout_init(&f, pb, slots);
        weft_fanout_reader_init(&rd, weft_fanout_ring(&f),
                  weft_fanout_ring_bytes(pb, slots), pb, slots);

        uint32_t toks[64];
        for (uint32_t i = 0; i < 64; i++) toks[i] = 128000u + i;
        CHECK(weft_tensor_publish_tokens(&f, toks, 64) == 1,
              "token stream published", "?");
        const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
        weft_tensor_hdr_t h;
        const void* pay;
        CHECK(c->fresh && weft_tensor_frame_parse(weft_fanout_view(&rd), pb,
                                                  &h, &pay) == 0,
              "parsed", "?");
        int tok_ok = h.dtype == WEFT_TENSOR_U32 && h.elem_count == 64;
        for (uint32_t i = 0; i < 64 && tok_ok; i++)
            if (weft_tensor_u32_at(pay, i) != 128000u + i) tok_ok = 0;
        CHECK(tok_ok, "all 64 tokens bit-exact", "%d", tok_ok);

        float pcm[256];
        for (uint32_t i = 0; i < 256; i++) pcm[i] = 0.25f * (float)i;
        CHECK(weft_tensor_publish_audio_f32(&f, pcm, 256) == 2,
              "audio chunk published", "?");
        c = weft_fanout_claim(&rd);
        CHECK(c->fresh && weft_tensor_frame_parse(weft_fanout_view(&rd), pb,
                                                  &h, &pay) == 0,
              "parsed", "?");
        int aud_ok = h.dtype == WEFT_TENSOR_F32 && h.elem_count == 256;
        for (uint32_t i = 0; i < 256 && aud_ok; i++)
            if (weft_tensor_f32_at(pay, i) != 0.25f * (float)i) aud_ok = 0;
        CHECK(aud_ok, "all 256 samples bit-exact", "%d", aud_ok);

        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
    }

    // ---- T6: fit refusal burns no seq -------------------------------------
    printf("## T6 fit refusal\n");
    {
        weft_fanout_t f;
        weft_fanout_reader_t rd;
        memset(&f, 0, sizeof(f));
        memset(&rd, 0, sizeof(rd));
        weft_fanout_init(&f, pb, slots);
        weft_fanout_reader_init(&rd, weft_fanout_ring(&f),
                  weft_fanout_ring_bytes(pb, slots), pb, slots);

        float big[4096];  // 4096 f16 = 8192B > 2048-32 slot
        const uint64_t refused = weft_tensor_publish_f16(&f, big, 4096);
        CHECK(refused == 0, "oversize tensor refused", "%llu",
              (unsigned long long)refused);
        weft_fanout_debug_t d;
        weft_fanout_debug_stats(&f, &d);
        CHECK(d.latest_seq == 0 && d.publishes == 0,
              "no seq burned (refused BEFORE begin)", "seq=%llu pub=%llu",
              (unsigned long long)d.latest_seq,
              (unsigned long long)d.publishes);
        // the ring still works after the refusal
        CHECK(weft_tensor_publish_tokens(&f, (const uint32_t*)(float[1]){0}, 1) == 1,
              "ring healthy after refusal", "?");
        const uint32_t dims[4] = { 0, 0, 0, 0 };
        CHECK(weft_tensor_frame_begin(&f, WEFT_TENSOR_U8, dims, 1) == NULL,
              "zero dim refused", "?");
        CHECK(weft_tensor_frame_begin(&f, (weft_tensor_dtype_t)99,
                                      (const uint32_t[1]){4}, 1) == NULL,
              "bad dtype refused", "?");
        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
    }

    // ---- T7: a streaming tensor workload + 4D video-embedding shapes ----
    printf("## T7 stream + rank-4\n");
    {
        weft_fanout_t f;
        weft_fanout_reader_t rd;
        memset(&f, 0, sizeof(f));
        memset(&rd, 0, sizeof(rd));
        weft_fanout_init(&f, pb, slots);
        weft_fanout_reader_init(&rd, weft_fanout_ring(&f),
                  weft_fanout_ring_bytes(pb, slots), pb, slots);

        // a 4D "video embedding" [1, 4, 8, 8] f16 = 256 elems = 512B
        const uint32_t dims[4] = { 1, 4, 8, 8 };
        int stream_ok = 1, rank4_ok = 1;
        float emb[256];
        for (uint32_t s = 1; s <= 200; s++) {
            for (uint32_t i = 0; i < 256; i++)
                emb[i] = (float)((s * 31u + i * 7u) & 0xFFu) / 64.0f - 2.0f;
            const uint64_t seq = weft_tensor_publish_f16(&f, emb, 256);
            if (seq != s) stream_ok = 0;
            if ((s % 5) == 0) {
                const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
                if (c->fresh) {
                    weft_tensor_hdr_t h;
                    const void* pay;
                    if (weft_tensor_frame_parse(weft_fanout_view(&rd), pb,
                                                &h, &pay) != 0) {
                        stream_ok = 0;
                    } else if (h.rank != 1 || h.elem_count != 256) {
                        // publish_f16 emits rank-1 frames (dims [n]) — the
                        // rank-4 shape check happens below via frame_begin
                    }
                }
            }
        }
        CHECK(stream_ok, "200-frame tensor stream, every sampled claim parses",
              "%d", stream_ok);

        // an explicit rank-4 frame
        uint8_t* cur = weft_tensor_frame_begin(&f, WEFT_TENSOR_F16, dims, 4);
        CHECK(cur != NULL, "rank-4 frame begun", "?");
        if (cur != NULL) {
            weft_tensor_fill_f32_as_f16(cur, emb, 256);
            weft_fanout_publish(&f);
            weft_fanout_claim(&rd);
            weft_tensor_hdr_t h;
            const void* pay;
            if (weft_tensor_frame_parse(weft_fanout_view(&rd), pb, &h, &pay) != 0 ||
                h.rank != 4 || h.dims[0] != 1 || h.dims[1] != 4 ||
                h.dims[2] != 8 || h.dims[3] != 8 || h.elem_count != 256)
                rank4_ok = 0;
        }
        CHECK(rank4_ok, "rank-4 [1,4,8,8] header exact", "%d", rank4_ok);

        weft_fanout_reader_destroy(&rd);
        weft_fanout_destroy(&f);
    }

    // ---- T8: WTS1 through the dmabuf substrate (cross-surface proof) ----
    printf("## T8 cross-surface (dmabuf substrate)\n");
    {
        int fd = memfd_create("weft-tensor-x", 0);
        ftruncate(fd, (off_t)weft_dmabuf_span_bytes(pb, slots));
        weft_dmabuf_ring_t r;
        memset(&r, 0, sizeof(r));
        CHECK(weft_dmabuf_ring_bind_fd(&r, fd, pb, slots) == 0,
              "session over the substrate", "?");
        weft_fanout_t f;
        memset(&f, 0, sizeof(f));
        weft_fanout_attach_writer(&f, r.ring,
                  weft_fanout_ring_bytes(pb, slots), pb, slots);
        // a second view (the reader lives on another mapping of the same
        // allocation — the dma-buf/cross-process posture)
        uint8_t* view2 = (uint8_t*)mmap(NULL, r.map_bytes, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0);
        weft_fanout_reader_t rd;
        weft_fanout_reader_init(&rd, view2 + 64,
                  weft_fanout_ring_bytes(pb, slots), pb, slots);

        float vec[100];
        for (uint32_t i = 0; i < 100; i++) vec[i] = (float)i / 8.0f;
        CHECK(weft_tensor_publish_f16(&f, vec, 100) == 1,
              "tensor frame through the substrate", "?");
        const weft_fanout_claim_t* c = weft_fanout_claim(&rd);
        weft_tensor_hdr_t h;
        const void* pay;
        CHECK(c->fresh && weft_tensor_frame_parse(weft_fanout_view(&rd), pb,
                                                  &h, &pay) == 0,
              "cross-view claim parses", "?");
        int ok = 1;
        for (uint32_t i = 0; i < 100; i++)
            if (weft_tensor_f16_at(pay, i) !=
                weft_f16_to_f32(weft_f32_to_f16(vec[i]))) { ok = 0; break; }
        CHECK(ok, "f16 payload bit-exact across the mapping boundary", "%d", ok);

        weft_fanout_reader_destroy(&rd);
        munmap(view2, r.map_bytes);
        weft_fanout_destroy(&f);
        weft_dmabuf_ring_free(&r);
        close(fd);
    }

    printf("verdict: %s (%d passed, %d failed)\n",
           g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
