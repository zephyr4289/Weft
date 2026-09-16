// frame_cursor.c — RFC 0008 FrameCursor, C driver layer (see frame_cursor.h).

#include "frame_cursor.h"

#include <string.h>

void weft_frame_cursor_init(weft_frame_cursor_t* c) {
    memset(c, 0, sizeof(*c));
    c->rec.seq = 0;
    c->rec.frames_behind = 0;
    c->rec.first = false;
}

const weft_frame_sample_t* weft_frame_cursor_update(weft_frame_cursor_t* c, weft_t* w) {
    // The ordinary wait-free claim: one exchange inside the kernel (02 §2),
    // then the live envelope seq read (A3 — the LIVE held buffer, never a
    // snapshot taken at claim time).
    weft_r_claim(w);
    const uint32_t seq = weft_r_seq(w);
    uint32_t frames_behind = 0;
    if (c->has_claimed && seq > c->last_seq) {
        frames_behind = seq - c->last_seq - 1; // u32 arithmetic, exact while gap < 2^31
    }
    // A decreasing seq (u32 wrap / writer reset) resets accounting rather
    // than reporting a huge burst — RFC 0008's declared semantics: with
    // seq < last_seq, frames_behind stays 0 and the baseline simply moves.
    c->has_claimed = true;
    c->total_dropped += frames_behind;
    c->last_seq = seq;
    c->claims++;
    c->rec.seq = seq;
    c->rec.frames_behind = frames_behind;
    c->rec.first = (c->claims == 1);
    return &c->rec;
}

void weft_frame_cursor_reset(weft_frame_cursor_t* c) {
    memset(c, 0, sizeof(*c));
}
