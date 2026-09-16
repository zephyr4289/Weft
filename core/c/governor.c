// governor.c — RFC 0009: FreshnessGovernor, C driver layer
//
// Direct mirror of packages/core/src/governor.ts and core/rust/src/governor.rs.
// The action ladder, the Reseed cooldown, and the counter semantics are
// pinned identically in all three (G5 proves it with a shared trace).

#include "governor.h"

void weft_governor_init_default(weft_governor_t* g) {
    weft_governor_init(g,
                       WEFT_GOV_FAST_PATH_BEHIND_DEFAULT,
                       WEFT_GOV_SKIP_BEHIND_DEFAULT,
                       WEFT_GOV_SNAPSHOT_BEHIND_DEFAULT,
                       WEFT_GOV_RESEED_COOLDOWN_MS_DEFAULT);
}

void weft_governor_init(weft_governor_t* g,
                        uint32_t fast_path_behind,
                        uint32_t skip_behind,
                        uint32_t snapshot_behind,
                        uint32_t reseed_cooldown_ms) {
    g->fast_path_behind = fast_path_behind;
    g->skip_behind = skip_behind;
    g->snapshot_behind = snapshot_behind;
    g->reseed_cooldown_ms = reseed_cooldown_ms;
    g->last_reseed_ms = -1;
    g->decided_drops = 0;
    g->reseeds = 0;
    g->steps = 0;
    g->act.kind = WEFT_GOV_FAST_PATH;
    g->act.skip_n = 0;
}

const weft_gov_action_t* weft_governor_step(weft_governor_t* g,
                                            uint32_t frames_behind,
                                            int64_t now_ms) {
    g->steps++;
    if (frames_behind <= g->fast_path_behind) {
        g->act.kind = WEFT_GOV_FAST_PATH;
        g->act.skip_n = 0;
    } else if (frames_behind <= g->skip_behind) {
        uint32_t n = frames_behind - g->fast_path_behind;
        g->decided_drops += n; // Law 4: decided drops are decisions
        g->act.kind = WEFT_GOV_SKIP;
        g->act.skip_n = n;
    } else if (frames_behind <= g->snapshot_behind) {
        g->act.kind = WEFT_GOV_SNAPSHOT;
        g->act.skip_n = 0;
    } else {
        // behind > snapshot_behind: Reseed, rate-limited by the cooldown.
        if (g->last_reseed_ms < 0 ||
            now_ms - g->last_reseed_ms >= (int64_t)g->reseed_cooldown_ms) {
            g->last_reseed_ms = now_ms;
            g->reseeds++;
            g->act.kind = WEFT_GOV_RESEED;
            g->act.skip_n = 0;
        } else {
            // Rate-limited: degrade to the best non-rebuild action. The
            // consumer still draws the newest frame; the rebuild
            // recommendation returns after the cooldown.
            g->act.kind = WEFT_GOV_SNAPSHOT;
            g->act.skip_n = 0;
        }
    }
    return &g->act;
}

void weft_governor_reset(weft_governor_t* g) {
    g->last_reseed_ms = -1;
    g->decided_drops = 0;
    g->reseeds = 0;
    g->steps = 0;
    g->act.kind = WEFT_GOV_FAST_PATH;
    g->act.skip_n = 0;
}
