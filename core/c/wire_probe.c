// wire_probe.c — RFC 0011 Telemetry Guardian's C wire-layout probe.
//
// Emits the OBSERVED wire layout of the C ring + Triad envelope as JSON.
// "Observed" is the operative word: the probe does not echo constants — it
// runs the REAL kernel/ring APIs with distinctive values and reads the raw
// bytes back at the documented offsets. If the implementation and the
// documentation disagree by ONE byte, the probe's numbers move and the
// guardian goes RED. Compile-time drift is caught by the static_asserts.
//
// Output: a single JSON line (guardian.py watch_wire diffs it against
// tools/guardian/wire-manifest.json). Exit 0 always when it CAN observe;
// observation failure (API returns unexpected values) is a finding in the
// JSON ("mismatch" fields), not a crash — the guardian adjudicates.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fanout.h"
#include "weft.h"

_Static_assert(sizeof(uint64_t) == 8, "guardian: u64 must be 8 bytes");
_Static_assert(WEFT_FANOUT_MAX_SLOTS == 64, "guardian: max slots constant drifted");

static uint32_t tword(uint32_t seq, uint32_t w) {
    // mirrors fanout_runner's pattern (04-LITMUS §0.1)
    extern uint32_t weft_mix32(uint32_t x);
    return weft_mix32(seq * 2654435761u + w);
}

static uint64_t rd64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

int main(void) {
    const unsigned SLOTS = 4;
    const size_t PAYLOAD = 16; // 4 words
    weft_fanout_t* f = weft_fanout_new(PAYLOAD, SLOTS);
    if (!f) { fprintf(stderr, "probe: fanout_new failed\n"); return 1; }

    const size_t ring_bytes = weft_fanout_ring_bytes(PAYLOAD, SLOTS);
    const uint8_t* ring = (const uint8_t*)weft_fanout_ring(f);

    // --- publish frame 1 with the pattern; observe where the bytes land ---
    uint32_t src[4];
    for (uint32_t w = 0; w < 4; w++) src[w] = tword(1, w);
    (void)weft_fanout_begin(f);
    (void)weft_fanout_fill(f, src, PAYLOAD);
    (void)weft_fanout_publish(f);

    // Raw byte reads at the DOCUMENTED offsets (the observation):
    const uint64_t latest_at_0   = rd64(ring + 0);
    const uint64_t publishes_at_8 = rd64(ring + 8);
    const uint64_t slot0_at_16   = rd64(ring + 16);
    const uint64_t slot1_at_24   = rd64(ring + 16 + 8);
    const size_t   payload_base  = 16 + 8 * SLOTS; // 48
    const uint8_t* payload_ptr   = ring + payload_base;
    const int payload_word0_ok   = memcmp(payload_ptr, src, 4) == 0;
    const int payload_word3_ok   = memcmp(payload_ptr + 12, src + 3, 4) == 0;

    // --- the 16-byte Triad envelope, via the kernel API ---
    weft_t w;
    int wrc = weft_init(&w, PAYLOAD);
    int env_ok = 0, magic_ok = 0, version_ok = 0, hdr_ok = 0, seq_ok = 0,
        plen_ok = 0, canary_ok = 0;
    uint32_t env_seq_val = 0, env_plen_val = 0;
    size_t env_header_val = 0;
    if (wrc == 0) {
        uint32_t buf[4];
        for (uint32_t j = 0; j < 4; j++) buf[j] = 0x5A5A0000u + j;
        if (weft_publish(&w, 7u, PAYLOAD) == WEFT_PUB_OK) {
            (void)weft_r_claim(&w);
            // live_ptr(offset 0) = buffer base = the envelope itself
            // ([0..16) envelope, [16..16+payload) payload, canary at tail).
            const uint8_t* env = (const uint8_t*)weft_r_live_ptr(&w, 0);
            if (env != NULL) {
                magic_ok   = memcmp(env, "WEFT", 4) == 0;
                version_ok = env[4] == 1 && env[5] == 0;  // u16le 1
                env_header_val = (size_t)env[6] | ((size_t)env[7] << 8);
                hdr_ok     = env_header_val == 16;
                env_seq_val = (uint32_t)env[8] | ((uint32_t)env[9] << 8) |
                              ((uint32_t)env[10] << 16) | ((uint32_t)env[11] << 24);
                seq_ok     = env_seq_val == 7;
                env_plen_val = (uint32_t)env[12] | ((uint32_t)env[13] << 8) |
                               ((uint32_t)env[14] << 16) | ((uint32_t)env[15] << 24);
                plen_ok    = env_plen_val == PAYLOAD;
                uint64_t canary;
                memcpy(&canary, env + w.buf_size - 8, 8); // canary at buf tail
                canary_ok = canary == 7;                  // canary == seq
                env_ok = magic_ok && version_ok && hdr_ok && seq_ok && plen_ok && canary_ok;
            }
        }
        weft_destroy(&w);
    }

    // The mixer vectors from the manifest, verified at runtime (the probe
    // re-derives them from weft_mix32 — a mixer drift moves these numbers).
    const uint32_t tw10  = tword(1, 0);
    const uint32_t tw73  = tword(7, 3);
    const uint32_t tw10015 = tword(100, 15);
    const int mixer_ok = tw10 == 1834104592u && tw73 == 2500287888u &&
                         tw10015 == 4197121613u;

    // The ctrl offsets the API's behavior IMPLIES (cross-check):
    const int latest_ok    = latest_at_0 == 1;
    const int publishes_ok = publishes_at_8 == 1;
    const int slot0_ok     = slot0_at_16 == 1;
    const int slot1_ok     = slot1_at_24 == 0;   // untouched slot stays 0
    const int payload_ok   = payload_word0_ok && payload_word3_ok;
    const int observed_ok  = latest_ok && publishes_ok && slot0_ok && slot1_ok &&
                             payload_ok && env_ok;

    printf(
        "{\"v\":1,"
        "\"fanout_ring\":{"
        "\"latestSeq\":{\"offset\":0,\"bytes\":8,\"observed_value\":%llu},"
        "\"publishes\":{\"offset\":8,\"bytes\":8,\"observed_value\":%llu},"
        "\"slotSeq\":{\"base\":16,\"stride\":8,\"bytes\":8,\"slot0_observed\":%llu,\"slot1_observed\":%llu},"
        "\"payload\":{\"base\":%llu,\"word0_ok\":%d,\"word3_ok\":%d},"
        "\"invariants\":{\"payload_bytes_multiple_of\":4,\"slot_count_min\":2,\"slot_count_max\":64,\"claim_attempts_max\":4}"
        "},"
        "\"triad_envelope\":{"
        "\"magic\":{\"offset\":0,\"bytes\":4,\"ok\":%d},"
        "\"version\":{\"offset\":4,\"bytes\":2,\"value\":%d,\"ok\":%d},"
        "\"header_size\":{\"offset\":6,\"bytes\":2,\"value\":%llu,\"ok\":%d},"
        "\"seq\":{\"offset\":8,\"bytes\":4,\"value\":%llu,\"ok\":%d},"
        "\"payload_len\":{\"offset\":12,\"bytes\":4,\"value\":%llu,\"ok\":%d},"
        "\"canary\":{\"bytes\":8,\"matches_seq\":%d}"
        "},"
        "\"mixer\":{"
        "\"vectors\":{\"tword_1_0\":%llu,\"tword_7_3\":%llu,\"tword_100_15\":%llu,\"ok\":%d}"
        "},"
        "\"observed\":{\"ring_bytes\":%llu,\"all_ok\":%d}}\n",
        (unsigned long long)latest_at_0,
        (unsigned long long)publishes_at_8,
        (unsigned long long)slot0_at_16,
        (unsigned long long)slot1_at_24,
        (unsigned long long)payload_base,
        payload_word0_ok ? 1 : 0,
        payload_word3_ok ? 1 : 0,
        magic_ok ? 1 : 0,
        version_ok ? 1 : 0, version_ok ? 1 : 0,
        (unsigned long long)env_header_val, hdr_ok ? 1 : 0,
        (unsigned long long)env_seq_val, seq_ok ? 1 : 0,
        (unsigned long long)env_plen_val, plen_ok ? 1 : 0,
        canary_ok ? 1 : 0,
        (unsigned long long)tw10,
        (unsigned long long)tw73,
        (unsigned long long)tw10015,
        mixer_ok ? 1 : 0,
        (unsigned long long)ring_bytes,
        observed_ok && mixer_ok ? 1 : 0);

    weft_fanout_free(f);
    return (observed_ok && mixer_ok) ? 0 : 1;
}
