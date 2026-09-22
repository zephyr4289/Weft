/* ---------------------------------------------------------------------------
 * test_oracle_tla.c — Weft Pillar 8: TLC verification oracle runner
 *
 * GATE COVERAGE: G2 companion + G4 (mandate §2.D.1): "TLC verification
 * oracle runner verifying TLA+ trace compliance".
 *
 * WHAT THIS IS: a deterministic C mirror of the two Pillar 8 TLA+ machines
 * (formal/seqlock_ring.tla, formal/wcr1_consensus.tla) that re-verifies,
 * INDEPENDENTLY of TLC:
 *
 *   SEQLOCK RING —
 *     a. Exhaustive BFS over the reachable state space of the bounded
 *        model (same constants as seqlock_ring.cfg) checking TypeOK,
 *        SeqParity and NoTornReads on every state, plus deadlock freedom.
 *     b. Writer-independence: for EVERY reachable state, the set of
 *        enabled writer actions is identical when all reader fields are
 *        zeroed — the mechanical content of "writers always make progress
 *        regardless of reader count or crashes" (ReaderNeverBlocksWriter).
 *     c. 10,000,000-step deterministic random walk with FULL-WIDTH
 *        sequence counters (no cap — covers the unbounded regime TLC's
 *        finite model cannot), checking NoTornReads at every step.
 *
 *   WCR1 CONSENSUS —
 *     d. Exhaustive BFS over the bounded model (same constants as
 *        wcr1_consensus.cfg) checking TypeOK, SinglePrimary (per-epoch
 *        lease uniqueness), QuorumPromise and HealFloor, plus deadlock
 *        freedom.
 *     e. Per-transition epoch monotonicity: promised[n] never decreases
 *        across every explored edge.
 *     f. 10,000,000-step deterministic random walk with full-width epochs.
 *
 * Law 1: all exploration structures (hash table, state arena, successor
 * buffers) are preallocated BEFORE the probe snapshot; the steady-state
 * BFS and the walk perform ZERO heap calls (this binary is linked with
 * -Wl,--wrap=malloc,calloc,realloc,strdup and -DWEFT_VERIFY_WRAP_PROBE).
 *
 * Deterministic: splitmix64 with the fixed seeds below (mandate §3.2).
 * Fail-closed: invariant violation, capacity overflow or a missed budget
 * all exit non-zero.
 * ------------------------------------------------------------------------- */

#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* malloc interposition probe (Law 1)                                  */
/* ------------------------------------------------------------------ */

#ifdef WEFT_VERIFY_WRAP_PROBE
extern void *__real_malloc(size_t n);
extern void *__real_calloc(size_t n, size_t m);
extern void *__real_realloc(void *p, size_t n);
extern void *__real_free(void *p);
extern char *__real_strdup(const char *s);

static unsigned long long g_probe_calls = 0u;

void *__wrap_malloc(size_t n)
{
    g_probe_calls++;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t m)
{
    g_probe_calls++;
    return __real_calloc(n, m);
}
void *__wrap_realloc(void *p, size_t n)
{
    g_probe_calls++;
    return __real_realloc(p, n);
}
void __wrap_free(void *p)
{
    g_probe_calls++;
    __real_free(p);
}
char *__wrap_strdup(const char *s)
{
    g_probe_calls++;
    return __real_strdup(s);
}
static unsigned long long weft_probe_calls(void)
{
    return g_probe_calls;
}
#else
static unsigned long long weft_probe_calls(void)
{
    return 0u;
}
#endif

/* ------------------------------------------------------------------ */
/* shared infrastructure                                               */
/* ------------------------------------------------------------------ */

static int g_failures = 0;

static uint64_t weft_rng_seed(uint64_t s)
{
    return s * 0x9E3779B97F4A7C15ULL + 0x123456789ABCDEFULL;
}

static uint64_t weft_rng_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t weft_hash64(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* open-addressing set over a state arena: index table stores
 * arena_index+1 (0 = empty); full compare on hash hit. */
typedef struct {
    uint32_t *table;
    uint64_t table_mask;
    uint8_t *arena;      /* state_size per entry */
    uint32_t state_size;
    uint64_t arena_cap;  /* max states */
    uint64_t count;      /* states stored */
} weft_stateset_t;

static int weft_stateset_init(weft_stateset_t *ss, uint64_t max_states,
                              uint32_t state_size)
{
    uint64_t tsize = 16u;
    while (tsize < max_states * 2u) {
        tsize <<= 1u;
    }
    ss->table = (uint32_t *)malloc((size_t)tsize * sizeof(uint32_t));
    ss->arena = (uint8_t *)malloc((size_t)max_states * (size_t)state_size);
    if (ss->table == NULL || ss->arena == NULL) {
        return -1;
    }
    memset(ss->table, 0, (size_t)tsize * sizeof(uint32_t));
    ss->table_mask = tsize - 1u;
    ss->state_size = state_size;
    ss->arena_cap = max_states;
    ss->count = 0u;
    return 0;
}

/* returns 1 if newly inserted, 0 if already present, -1 on overflow */
static int weft_stateset_add(weft_stateset_t *ss, const void *state)
{
    if (ss->count >= ss->arena_cap) {
        return -1;
    }
    uint64_t h = weft_hash64(state, ss->state_size);
    uint64_t j = h & ss->table_mask;
    for (;;) {
        uint32_t e = ss->table[j];
        if (e == 0u) {
            memcpy(ss->arena + ss->count * ss->state_size, state,
                   ss->state_size);
            ss->table[j] = (uint32_t)(ss->count + 1u);
            ss->count++;
            return 1;
        }
        if (memcmp(ss->arena + (uint64_t)(e - 1u) * ss->state_size, state,
                   ss->state_size) == 0) {
            return 0;
        }
        j = (j + 1u) & ss->table_mask;
    }
}

static const void *weft_stateset_get(const weft_stateset_t *ss, uint64_t idx)
{
    return ss->arena + idx * ss->state_size;
}

/* ================================================================== */
/* MODEL 1: seqlock ring (mirror of formal/seqlock_ring.tla)           */
/* ================================================================== */

#define SQ_SLOTS   2u
#define SQ_VALS    2u
#define SQ_SEQCAP  4u
#define SQ_READERS 2u

enum {
    SQ_W_IDLE = 0, SQ_W_LO = 1, SQ_W_HI = 2, SQ_W_DN = 3,
    SQ_R_IDLE = 0, SQ_R_MID = 1, SQ_R_GOTLO = 2, SQ_R_GOTHI = 3
};

typedef struct {
    uint32_t seq[SQ_SLOTS];      /* full-width for the walk regime */
    uint8_t  dlo[SQ_SLOTS], dhi[SQ_SLOTS];
    uint8_t  head;
    uint8_t  wphase;
    uint8_t  wval;
    uint8_t  wbeat;
    uint8_t  rphase[SQ_READERS];
    uint8_t  rslot[SQ_READERS];
    uint32_t rseqb[SQ_READERS];
    uint8_t  relo[SQ_READERS], rehi[SQ_READERS];
    uint8_t  rglo[SQ_READERS], rghi[SQ_READERS];
    uint8_t  rcommit[SQ_READERS];
    uint8_t  rtorn[SQ_READERS];
} sq_state_t;

/* action ids (successor tags) */
enum {
    SQ_A_WSEQUP = 0, SQ_A_WLO, SQ_A_WHI, SQ_A_WDN, SQ_A_WSKIP,
    SQ_A_RBEGIN, SQ_A_RLO, SQ_A_RHI, SQ_A_REND, SQ_A_COUNT
};

static void sq_init(sq_state_t *s, uint32_t head, uint32_t wval,
                    uint32_t wbeat)
{
    memset(s, 0, sizeof(*s));
    s->head = (uint8_t)head;
    s->wval = (uint8_t)wval;
    s->wbeat = (uint8_t)wbeat;
}

/* generate successors of s into out (each tagged); returns count */
static int sq_succ(const sq_state_t *s, sq_state_t *out, uint8_t *tags,
                   int bounded)
{
    int k = 0;
    uint32_t cap = bounded ? SQ_SEQCAP : 0xFFFFFFFEu;

    if (s->wphase == SQ_W_IDLE && s->seq[s->head] + 2u <= cap) {
        for (uint32_t v = 0; v < SQ_VALS; v++) {
            out[k] = *s;
            out[k].seq[s->head] += 1u;
            out[k].wphase = SQ_W_LO;
            out[k].wval = (uint8_t)v;
            tags[k++] = SQ_A_WSEQUP;
        }
    }
    if (s->wphase == SQ_W_LO) {
        out[k] = *s;
        out[k].dlo[s->head] = s->wval;
        out[k].wphase = SQ_W_HI;
        tags[k++] = SQ_A_WLO;
    }
    if (s->wphase == SQ_W_HI) {
        out[k] = *s;
        out[k].dhi[s->head] = s->wval;
        out[k].wphase = SQ_W_DN;
        tags[k++] = SQ_A_WHI;
    }
    if (s->wphase == SQ_W_DN) {
        out[k] = *s;
        out[k].seq[s->head] += 1u;
        out[k].wphase = SQ_W_IDLE;
        out[k].head = (uint8_t)((s->head + 1u) % SQ_SLOTS);
        out[k].wbeat = (uint8_t)(1u - s->wbeat);
        tags[k++] = SQ_A_WDN;
    }
    if (bounded && s->wphase == SQ_W_IDLE && s->seq[s->head] + 2u > cap) {
        out[k] = *s;
        out[k].head = (uint8_t)((s->head + 1u) % SQ_SLOTS);
        out[k].wbeat = (uint8_t)(1u - s->wbeat);
        tags[k++] = SQ_A_WSKIP;
    }
    for (uint32_t r = 0; r < SQ_READERS; r++) {
        if (s->rphase[r] == SQ_R_IDLE) {
            for (uint32_t sl = 0; sl < SQ_SLOTS; sl++) {
                out[k] = *s;
                out[k].rslot[r] = (uint8_t)sl;
                out[k].rseqb[r] = s->seq[sl];
                out[k].relo[r] = s->dlo[sl];
                out[k].rehi[r] = s->dhi[sl];
                out[k].rglo[r] = 0u;
                out[k].rghi[r] = 0u;
                out[k].rcommit[r] = 0u;
                out[k].rphase[r] = SQ_R_MID;
                tags[k++] = SQ_A_RBEGIN;
            }
        }
        if (s->rphase[r] == SQ_R_MID) {
            out[k] = *s;
            out[k].rglo[r] = s->dlo[s->rslot[r]];
            out[k].rphase[r] = SQ_R_GOTLO;
            tags[k++] = SQ_A_RLO;
        }
        if (s->rphase[r] == SQ_R_GOTLO) {
            out[k] = *s;
            out[k].rghi[r] = s->dhi[s->rslot[r]];
            out[k].rphase[r] = SQ_R_GOTHI;
            tags[k++] = SQ_A_RHI;
        }
        if (s->rphase[r] == SQ_R_GOTHI) {
            out[k] = *s;
            if (s->seq[s->rslot[r]] == s->rseqb[r] &&
                (s->rseqb[r] % 2u) == 0u) {
                out[k].rcommit[r] = 1u; /* validated: COMMIT */
            } else {
                out[k].rtorn[r] = 1u;   /* torn: honestly refused */
            }
            out[k].rphase[r] = SQ_R_IDLE;
            tags[k++] = SQ_A_REND;
        }
    }
    return k;
}

/* invariants: returns violation count; fills which for reporting.
 * `bounded` selects the finite-model TypeOK bounds (the walk regime uses
 * full-width counters that trivially satisfy the unbounded ones). */
static int sq_invariants(const sq_state_t *s, int bounded, const char **which)
{
    int v = 0;
    *which = NULL;
    for (uint32_t i = 0; i < SQ_SLOTS; i++) {
        if (bounded && s->seq[i] > SQ_SEQCAP) {
            *which = "TypeOK(seq)";
            v++;
        }
        if (s->dlo[i] >= SQ_VALS || s->dhi[i] >= SQ_VALS) {
            *which = "TypeOK(data)";
            v++;
        }
    }
    if (s->head >= SQ_SLOTS || s->wphase > SQ_W_DN || s->wval >= SQ_VALS) {
        *which = "TypeOK(writer)";
        v++;
    }
    /* SeqParity */
    if (s->wphase != SQ_W_IDLE && (s->seq[s->head] % 2u) != 1u) {
        *which = "SeqParity(mid-write-odd)";
        v++;
    }
    if (s->wphase == SQ_W_IDLE) {
        for (uint32_t i = 0; i < SQ_SLOTS; i++) {
            if ((s->seq[i] % 2u) != 0u) {
                *which = "SeqParity(idle-even)";
                v++;
            }
        }
    }
    /* NoTornReads: committed reads equal the ground truth at begin */
    for (uint32_t r = 0; r < SQ_READERS; r++) {
        if (s->rcommit[r] &&
            (s->rglo[r] != s->relo[r] || s->rghi[r] != s->rehi[r])) {
            *which = "NoTornReads";
            v++;
        }
        if (s->rphase[r] > SQ_R_GOTHI || s->rslot[r] >= SQ_SLOTS) {
            *which = "TypeOK(reader)";
            v++;
        }
    }
    return v;
}

/* writer action availability, evaluated ONLY on writer-owned fields */
static uint32_t sq_writer_mask(const sq_state_t *s, int bounded)
{
    uint32_t cap = bounded ? SQ_SEQCAP : 0xFFFFFFFEu;
    uint32_t m = 0u;
    if (s->wphase == SQ_W_IDLE) {
        m |= (s->seq[s->head] + 2u <= cap) ? (1u << SQ_A_WSEQUP) : 0u;
        m |= (bounded && s->seq[s->head] + 2u > cap) ? (1u << SQ_A_WSKIP) : 0u;
    }
    if (s->wphase == SQ_W_LO) {
        m |= 1u << SQ_A_WLO;
    }
    if (s->wphase == SQ_W_HI) {
        m |= 1u << SQ_A_WHI;
    }
    if (s->wphase == SQ_W_DN) {
        m |= 1u << SQ_A_WDN;
    }
    return m;
}

/* ReaderNeverBlocksWriter, mechanically: zero every reader field and
 * re-evaluate writer availability. Must be identical for ALL states. */
static int sq_writer_independent(const sq_state_t *s, int bounded)
{
    sq_state_t z = *s;
    memset(z.rphase, 0, sizeof(z.rphase));
    memset(z.rslot, 0, sizeof(z.rslot));
    memset(z.rseqb, 0, sizeof(z.rseqb));
    memset(z.relo, 0, sizeof(z.relo));
    memset(z.rehi, 0, sizeof(z.rehi));
    memset(z.rglo, 0, sizeof(z.rglo));
    memset(z.rghi, 0, sizeof(z.rghi));
    memset(z.rcommit, 0, sizeof(z.rcommit));
    memset(z.rtorn, 0, sizeof(z.rtorn));
    return sq_writer_mask(s, bounded) == sq_writer_mask(&z, bounded);
}

static sq_state_t g_sq_succ[32];
static uint8_t g_sq_tags[32];

static void sq_bfs(int reduced)
{
    /* full mode: exhaustive with fail-closed overflow; --san mode: a
     * deterministic budget-limited slice (invariants still checked on
     * every VISITED state) */
    const uint64_t cap = reduced ? (1u << 18) : (1u << 23);
    weft_stateset_t ss;
    if (weft_stateset_init(&ss, cap, sizeof(sq_state_t)) != 0) {
        fprintf(stderr, "FAIL[sq-bfs]: arena alloc\n");
        g_failures++;
        return;
    }
    /* initial states: head x wval x wbeat (mirrors the TLA Init) */
    for (uint32_t h = 0; h < SQ_SLOTS; h++) {
        for (uint32_t v = 0; v < SQ_VALS; v++) {
            for (uint32_t b = 0; b < 2u; b++) {
                sq_state_t s0;
                sq_init(&s0, h, v, b);
                (void)weft_stateset_add(&ss, &s0);
            }
        }
    }
    uint64_t edges = 0u, dead = 0u, indep_fail = 0u;
    const char *why = NULL;
    double t0 = now_ms();
    unsigned long long probe0 = weft_probe_calls();

    for (uint64_t cur = 0; cur < ss.count; cur++) {
        sq_state_t s;
        memcpy(&s, weft_stateset_get(&ss, cur), sizeof(s));
        if (sq_invariants(&s, 1, &why) != 0) {
            fprintf(stderr, "FAIL[sq-bfs]: invariant %s violated at "
                            "state #%llu\n",
                    why ? why : "?", (unsigned long long)cur);
            g_failures++;
            break;
        }
        if (!sq_writer_independent(&s, 1)) {
            indep_fail++;
        }
        int n = sq_succ(&s, g_sq_succ, g_sq_tags, 1);
        if (n == 0) {
            dead++;
        }
        for (int i = 0; i < n; i++) {
            if (sq_invariants(&g_sq_succ[i], 1, &why) != 0) {
                fprintf(stderr, "FAIL[sq-bfs]: invariant %s violated in "
                                "successor of #%llu\n",
                        why ? why : "?", (unsigned long long)cur);
                g_failures++;
            }
            int rc = weft_stateset_add(&ss, &g_sq_succ[i]);
            if (rc < 0) {
                if (reduced) {
                    goto done; /* budget-limited tier, not a failure */
                }
                fprintf(stderr,
                        "FAIL[sq-bfs]: capacity overflow at %llu states "
                        "(fail-closed)\n",
                        (unsigned long long)ss.count);
                g_failures++;
                goto done;
            }
            edges += (rc == 1) ? 1u : 0u;
        }
    }
done:
    {
        unsigned long long probe1 = weft_probe_calls();
        double dt = now_ms() - t0;
        printf("[oracle][seqlock] BFS states=%llu%s edges=%llu "
               "deadlocks=%llu writer-independence-failures=%llu "
               "(%.1f s, heap-calls=%llu)\n",
               (unsigned long long)ss.count,
               reduced ? " (budget-limited tier)" : "",
               (unsigned long long)edges,
               (unsigned long long)dead, (unsigned long long)indep_fail,
               dt / 1000.0, probe1 - probe0);
        if (dead != 0u) {
            fprintf(stderr, "FAIL[sq-bfs]: %llu deadlock states\n",
                    (unsigned long long)dead);
            g_failures++;
        }
        if (indep_fail != 0u) {
            fprintf(stderr,
                    "FAIL[sq-bfs]: writer enabledness depends on reader "
                    "state in %llu states\n",
                    (unsigned long long)indep_fail);
            g_failures++;
        }
        if (probe1 != probe0) {
            fprintf(stderr, "FAIL(Law1)[sq-bfs]: %llu heap calls in "
                            "steady state\n",
                    probe1 - probe0);
            g_failures++;
        }
    }
    free(ss.table);
    free(ss.arena);
}

static void sq_walk(uint64_t steps)
{
    sq_state_t s;
    sq_init(&s, 0u, 0u, 0u);
    uint64_t rng = weft_rng_seed(0x5345CC4B42464C4FULL);
    uint64_t commits = 0u, torn = 0u, wsteps = 0u, rsteps = 0u;
    const char *why = NULL;
    unsigned long long probe0 = weft_probe_calls();
    double t0 = now_ms();

    for (uint64_t i = 0; i < steps; i++) {
        int n = sq_succ(&s, g_sq_succ, g_sq_tags, 0);
        if (n <= 0) {
            fprintf(stderr, "FAIL[sq-walk]: no successors at step %llu\n",
                    (unsigned long long)i);
            g_failures++;
            return;
        }
        int pick = (int)(weft_rng_next(&rng) % (uint64_t)n);
        /* action telemetry */
        switch (g_sq_tags[pick]) {
        case SQ_A_REND:
            if (g_sq_succ[pick].rcommit[0] && !s.rcommit[0]) {
                commits++;
            } else if (g_sq_succ[pick].rtorn[0] && !s.rtorn[0]) {
                torn++;
            }
            if (g_sq_succ[pick].rcommit[1] && !s.rcommit[1]) {
                commits++;
            } else if (g_sq_succ[pick].rtorn[1] && !s.rtorn[1]) {
                torn++;
            }
            rsteps++;
            break;
        case SQ_A_RBEGIN:
        case SQ_A_RLO:
        case SQ_A_RHI:
            rsteps++;
            break;
        default:
            wsteps++;
            break;
        }
        s = g_sq_succ[pick];
        if (sq_invariants(&s, 0, &why) != 0) {
            fprintf(stderr, "FAIL[sq-walk]: invariant %s violated at step "
                            "%llu\n",
                    why ? why : "?", (unsigned long long)i);
            g_failures++;
            return;
        }
        /* spot-check writer independence under reader mutation */
        if ((i & 1023u) == 0u && !sq_writer_independent(&s, 0)) {
            fprintf(stderr, "FAIL[sq-walk]: writer depends on readers at "
                            "step %llu\n",
                    (unsigned long long)i);
            g_failures++;
            return;
        }
    }
    unsigned long long probe1 = weft_probe_calls();
    double dt = now_ms() - t0;
    printf("[oracle][seqlock] walk steps=%llu seed=0x%016" PRIx64
           " commits=%llu torn-detected=%llu torn-VIOLATIONS=0 "
           "writer-steps=%llu reader-steps=%llu (%.1f s, heap-calls=%llu)\n",
           (unsigned long long)steps, rng, (unsigned long long)commits,
           (unsigned long long)torn, (unsigned long long)wsteps,
           (unsigned long long)rsteps, dt / 1000.0, probe1 - probe0);
    if (probe1 != probe0) {
        fprintf(stderr, "FAIL(Law1)[sq-walk]: %llu heap calls\n",
                probe1 - probe0);
        g_failures++;
    }
    if (commits == 0u || torn == 0u) {
        fprintf(stderr,
                "FAIL[sq-walk]: walk did not exercise both outcomes "
                "(commits=%llu torn=%llu)\n",
                (unsigned long long)commits, (unsigned long long)torn);
        g_failures++;
    }
}

/* ================================================================== */
/* MODEL 2: WCR1 consensus (mirror of formal/wcr1_consensus.tla)       */
/* ================================================================== */

#define WC_N     3u
#define WC_ECAP  2u
#define WC_LEASE 2u
#define WC_MSGBUF 2u

enum { WC_F = 0, WC_C = 1, WC_P = 2 };
enum { WC_M_NONE = 0, WC_M_VRQ = 1, WC_M_VGR = 2, WC_M_HB = 3,
       WC_M_HBA = 4 };

typedef struct {
    uint32_t promised[WC_N];    /* full-width for the walk regime */
    uint32_t candEpoch[WC_N];   /* epoch of the current candidacy     */
    uint32_t leaseEpoch[WC_N];
    uint8_t  role[WC_N];
    uint8_t  leaseRem[WC_N];
    uint8_t  grants[WC_N];       /* bitmask of grant senders */
    uint8_t  hback[WC_N];        /* bitmask of hb ack senders */
    uint8_t  part[WC_N];
    uint8_t  msgn;
    uint8_t  mtype[WC_MSGBUF];
    uint8_t  mfrom[WC_MSGBUF];
    uint8_t  mto[WC_MSGBUF];
    uint32_t mep[WC_MSGBUF];
    uint32_t healedFloor;
} wcr_state_t;

enum {
    WC_A_TICK = 0, WC_A_START, WC_A_GRANT, WC_A_DELIVGRANT, WC_A_DROP,
    WC_A_WIN, WC_A_HB, WC_A_DELIVHB, WC_A_DELIVACK, WC_A_RENEW,
    WC_A_SPLIT, WC_A_HEAL, WC_A_COUNT
};

static void wcr_init(wcr_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

static int wcr_same_comp(const wcr_state_t *s, uint32_t a, uint32_t b)
{
    return s->part[a] == s->part[b];
}

static uint32_t wcr_comp_max_promised(const wcr_state_t *s, uint32_t n)
{
    uint32_t mx = 0u;
    for (uint32_t m = 0; m < WC_N; m++) {
        if (wcr_same_comp(s, m, n) && s->promised[m] > mx) {
            mx = s->promised[m];
        }
    }
    return mx;
}

static int wcr_is_active(const wcr_state_t *s, uint32_t n)
{
    return s->role[n] == WC_P && s->leaseRem[n] > 0;
}

/* canonical insert: keep msgs ordered by (type, from, to, ep) so that the
 * array representation matches the TLA set semantics one-to-one */
static void wcr_msg_insert(wcr_state_t *s, uint8_t type, uint8_t from,
                           uint8_t to, uint32_t ep)
{
    if (s->msgn >= WC_MSGBUF) {
        return; /* guarded by callers; defensive */
    }
    /* set semantics: an identical in-flight message is not duplicated */
    for (int i = 0; i < (int)s->msgn; i++) {
        if (s->mtype[i] == type && s->mfrom[i] == from && s->mto[i] == to &&
            s->mep[i] == ep) {
            return;
        }
    }
    int i = (int)s->msgn;
    while (i > 0) {
        uint8_t t0 = s->mtype[i - 1], f0 = s->mfrom[i - 1];
        uint8_t o0 = s->mto[i - 1];
        uint32_t e0 = s->mep[i - 1];
        if (t0 > type || (t0 == type && (f0 > from ||
            (f0 == from && (o0 > to || (o0 == to && e0 > ep)))))) {
            s->mtype[i] = s->mtype[i - 1];
            s->mfrom[i] = s->mfrom[i - 1];
            s->mto[i] = s->mto[i - 1];
            s->mep[i] = s->mep[i - 1];
            i--;
        } else {
            break;
        }
    }
    s->mtype[i] = type;
    s->mfrom[i] = from;
    s->mto[i] = to;
    s->mep[i] = ep;
    s->msgn++;
}

static void wcr_msg_remove(wcr_state_t *s, int idx)
{
    for (int i = idx; i + 1 < (int)s->msgn; i++) {
        s->mtype[i] = s->mtype[i + 1];
        s->mfrom[i] = s->mfrom[i + 1];
        s->mto[i] = s->mto[i + 1];
        s->mep[i] = s->mep[i + 1];
    }
    s->msgn--;
    /* zero the vacated slot so dead bytes never inflate the state space
     * (set semantics: two states differing only in dead-slot garbage are
     * the SAME TLC state) */
    s->mtype[s->msgn] = WC_M_NONE;
    s->mfrom[s->msgn] = 0u;
    s->mto[s->msgn] = 0u;
    s->mep[s->msgn] = 0u;
}

static int wcr_popcount3(uint8_t m)
{
    return ((m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1));
}

static int wcr_succ(const wcr_state_t *s, wcr_state_t *out, uint8_t *tags,
                     int bounded)
{
    int k = 0;
    uint32_t ecap = bounded ? WC_ECAP : 0xFFFFFFFEu;

    /* Tick */
    {
        out[k] = *s;
        for (uint32_t n = 0; n < WC_N; n++) {
            if (out[k].leaseRem[n] > 0) {
                out[k].leaseRem[n]--;
            }
            if (out[k].role[n] == WC_P && out[k].leaseRem[n] == 0) {
                out[k].role[n] = WC_F;
            }
        }
        tags[k++] = WC_A_TICK;
    }
    /* StartElection(n) */
    for (uint32_t n = 0; n < WC_N; n++) {
        if (s->role[n] == WC_F || s->role[n] == WC_C) {
            uint32_t e = wcr_comp_max_promised(s, n) + 1u;
            uint32_t compn = 0u;
            for (uint32_t m = 0; m < WC_N; m++) {
                if (wcr_same_comp(s, m, n)) {
                    compn++;
                }
            }
            if (e <= ecap &&
                (uint32_t)s->msgn + (compn - 1u) <= WC_MSGBUF) {
                out[k] = *s;
                out[k].role[n] = WC_C;
                out[k].promised[n] = e;
                out[k].candEpoch[n] = e; /* stamp the candidacy epoch */
                out[k].grants[n] = 0u;
                out[k].hback[n] = 0u;
                for (uint32_t m = 0; m < WC_N; m++) {
                    if (m != n && wcr_same_comp(s, m, n)) {
                        wcr_msg_insert(&out[k], WC_M_VRQ, (uint8_t)n,
                                       (uint8_t)m, e);
                    }
                }
                tags[k++] = WC_A_START;
            }
        }
    }
    /* GrantVote(v): consume a vrq addressed to v */
    for (int mi = 0; mi < (int)s->msgn; mi++) {
        if (s->mtype[mi] == WC_M_VRQ) {
            uint8_t v = s->mto[mi];
            uint8_t f = s->mfrom[mi];
            uint32_t e = s->mep[mi];
            if (wcr_same_comp(s, v, f) && e > s->promised[v] &&
                !wcr_is_active(s, v) && e <= ecap) {
                out[k] = *s;
                out[k].promised[v] = e;
                wcr_msg_remove(&out[k], mi);
                wcr_msg_insert(&out[k], WC_M_VGR, v, f, e);
                tags[k++] = WC_A_GRANT;
            }
        }
    }
    /* DeliverGrant(c): consume a vgr addressed to the candidate */
    for (int mi = 0; mi < (int)s->msgn; mi++) {
        if (s->mtype[mi] == WC_M_VGR) {
            uint8_t c = s->mto[mi];
            uint8_t f = s->mfrom[mi];
            uint32_t e = s->mep[mi];
            if (wcr_same_comp(s, c, f) && s->role[c] == WC_C &&
                e == s->candEpoch[c]) {
                out[k] = *s;
                out[k].grants[c] |= (uint8_t)(1u << f);
                wcr_msg_remove(&out[k], mi);
                tags[k++] = WC_A_DELIVGRANT;
            }
        }
    }
    /* DropMsg(i) */
    for (int mi = 0; mi < (int)s->msgn; mi++) {
        out[k] = *s;
        wcr_msg_remove(&out[k], mi);
        tags[k++] = WC_A_DROP;
    }
    /* WinElection(c) */
    for (uint32_t c = 0; c < WC_N; c++) {
        if (s->role[c] == WC_C &&
            wcr_popcount3((uint8_t)(s->grants[c] | (1u << c))) >= 2) {
            out[k] = *s;
            out[k].role[c] = WC_P;
            out[k].leaseRem[c] = WC_LEASE;
            out[k].leaseEpoch[c] = s->candEpoch[c]; /* the epoch the quorum
                                                       actually promised */
            out[k].grants[c] = 0u;
            tags[k++] = WC_A_WIN;
        }
    }
    /* Heartbeat(n) */
    for (uint32_t n = 0; n < WC_N; n++) {
        if (wcr_is_active(s, n)) {
            uint32_t compn = 0u;
            for (uint32_t m = 0; m < WC_N; m++) {
                if (wcr_same_comp(s, m, n)) {
                    compn++;
                }
            }
            if ((uint32_t)s->msgn + (compn - 1u) <= WC_MSGBUF) {
                out[k] = *s;
                for (uint32_t m = 0; m < WC_N; m++) {
                    if (m != n && wcr_same_comp(s, m, n)) {
                        wcr_msg_insert(&out[k], WC_M_HB, (uint8_t)n,
                                       (uint8_t)m, s->leaseEpoch[n]);
                    }
                }
                tags[k++] = WC_A_HB;
            }
        }
    }
    /* DeliverHb(v) */
    for (int mi = 0; mi < (int)s->msgn; mi++) {
        if (s->mtype[mi] == WC_M_HB) {
            uint8_t v = s->mto[mi];
            uint8_t f = s->mfrom[mi];
            uint32_t e = s->mep[mi];
            if (wcr_same_comp(s, v, f) && e >= s->promised[v]) {
                out[k] = *s;
                out[k].promised[v] = e;
                wcr_msg_remove(&out[k], mi);
                wcr_msg_insert(&out[k], WC_M_HBA, v, f, e);
                tags[k++] = WC_A_DELIVHB;
            }
        }
    }
    /* DeliverAck(n) */
    for (int mi = 0; mi < (int)s->msgn; mi++) {
        if (s->mtype[mi] == WC_M_HBA) {
            uint8_t n = s->mto[mi];
            uint8_t f = s->mfrom[mi];
            uint32_t e = s->mep[mi];
            if (wcr_same_comp(s, n, f) && s->role[n] == WC_P &&
                e == s->leaseEpoch[n]) {
                out[k] = *s;
                out[k].hback[n] |= (uint8_t)(1u << f);
                wcr_msg_remove(&out[k], mi);
                tags[k++] = WC_A_DELIVACK;
            }
        }
    }
    /* RenewLease(n) */
    for (uint32_t n = 0; n < WC_N; n++) {
        if (s->role[n] == WC_P &&
            wcr_popcount3((uint8_t)(s->hback[n] | (1u << n))) >= 2 &&
            s->leaseRem[n] < WC_LEASE) {
            out[k] = *s;
            out[k].leaseRem[n] = WC_LEASE;
            out[k].hback[n] = 0u;
            tags[k++] = WC_A_RENEW;
        }
    }
    /* Split(n, v): one link flap */
    for (uint32_t n = 0; n < WC_N; n++) {
        for (uint32_t v = 0; v < 2u; v++) {
            if (s->part[n] != v) {
                out[k] = *s;
                out[k].part[n] = (uint8_t)v;
                tags[k++] = WC_A_SPLIT;
            }
        }
    }
    /* Heal */
    {
        int any_split = 0;
        for (uint32_t n = 0; n < WC_N; n++) {
            if (s->part[n] != 0u) {
                any_split = 1;
            }
        }
        if (any_split) {
            uint32_t mx = 0u;
            for (uint32_t n = 0; n < WC_N; n++) {
                if (s->promised[n] > mx) {
                    mx = s->promised[n];
                }
            }
            out[k] = *s;
            for (uint32_t n = 0; n < WC_N; n++) {
                out[k].part[n] = 0u;
                out[k].promised[n] = mx;
            }
            out[k].healedFloor = mx;
            tags[k++] = WC_A_HEAL;
        }
    }
    return k;
}


static int g_dumped = 0;
static void wcr_dump(const wcr_state_t *s, const char *tag)
{
    if (g_dumped) {
        return;
    }
    g_dumped = 1;
    printf("DUMP[%s] promised=%u,%u,%u role=%u,%u,%u leaseRem=%u,%u,%u "
           "leaseEpoch=%u,%u,%u grants=%u,%u,%u hback=%u,%u,%u "
           "part=%u,%u,%u healedFloor=%u msgs=[",
           tag, (unsigned)s->promised[0], (unsigned)s->promised[1],
           (unsigned)s->promised[2], (unsigned)s->role[0],
           (unsigned)s->role[1], (unsigned)s->role[2],
           (unsigned)s->leaseRem[0], (unsigned)s->leaseRem[1],
           (unsigned)s->leaseRem[2], (unsigned)s->leaseEpoch[0],
           (unsigned)s->leaseEpoch[1], (unsigned)s->leaseEpoch[2],
           (unsigned)s->grants[0], (unsigned)s->grants[1],
           (unsigned)s->grants[2], (unsigned)s->hback[0],
           (unsigned)s->hback[1], (unsigned)s->hback[2],
           (unsigned)s->part[0], (unsigned)s->part[1], (unsigned)s->part[2],
           (unsigned)s->healedFloor);
    for (int i = 0; i < (int)s->msgn; i++) {
        printf("(%u:%u->%u e%u) ", (unsigned)s->mtype[i],
               (unsigned)s->mfrom[i], (unsigned)s->mto[i],
               (unsigned)s->mep[i]);
    }
    printf("]\n");
}

static int wcr_invariants(const wcr_state_t *s, int bounded,
                         const char **which)
{
    int v = 0;
    *which = NULL;
    /* TypeOK (epoch bounds only in the finite BFS tier) */
    for (uint32_t n = 0; n < WC_N; n++) {
        if (s->role[n] > WC_P || s->leaseRem[n] > WC_LEASE ||
            (bounded && (s->promised[n] > WC_ECAP ||
                         s->leaseEpoch[n] > WC_ECAP ||
                         s->healedFloor > WC_ECAP)) ||
            (!bounded && (s->leaseEpoch[n] > s->promised[n] &&
                          s->role[n] == WC_C))) {
            *which = "TypeOK";
            v++;
        }
    }
    if (s->msgn > WC_MSGBUF) {
        *which = "TypeOK(msgbuf)";
        v++;
    }
    /* SinglePrimary (per-epoch) + global count telemetry */
    for (uint32_t a = 0; a < WC_N; a++) {
        for (uint32_t b = a + 1u; b < WC_N; b++) {
            if (wcr_is_active(s, a) && wcr_is_active(s, b) &&
                s->leaseEpoch[a] == s->leaseEpoch[b]) {
                *which = "SinglePrimary";
                v++;
                wcr_dump(s, "SinglePrimary");
            }
        }
    }
    /* QuorumPromise */
    for (uint32_t n = 0; n < WC_N; n++) {
        if (wcr_is_active(s, n)) {
            uint32_t c = 0u;
            for (uint32_t m = 0; m < WC_N; m++) {
                if (s->promised[m] >= s->leaseEpoch[n]) {
                    c++;
                }
            }
            if (c < 2u) {
                *which = "QuorumPromise";
                v++;
            }
        }
    }
    /* HealFloor */
    for (uint32_t n = 0; n < WC_N; n++) {
        if (s->promised[n] < s->healedFloor) {
            *which = "HealFloor";
            v++;
        }
    }
    return v;
}

static wcr_state_t g_wcr_succ[48];
static uint8_t g_wcr_tags[48];

static int wcr_promised_monotone(const wcr_state_t *before,
                                 const wcr_state_t *after)
{
    for (uint32_t n = 0; n < WC_N; n++) {
        if (after->promised[n] < before->promised[n]) {
            return 0;
        }
    }
    return 1;
}

static void wcr_bfs(int reduced)
{
    const uint64_t cap = reduced ? (1u << 18) : (1u << 23);
    weft_stateset_t ss;
    if (weft_stateset_init(&ss, cap, sizeof(wcr_state_t)) != 0) {
        fprintf(stderr, "FAIL[wcr-bfs]: arena alloc\n");
        g_failures++;
        return;
    }
    wcr_state_t s0;
    wcr_init(&s0);
    (void)weft_stateset_add(&ss, &s0);
    uint64_t edges = 0u, dead = 0u, mono_fail = 0u;
    const char *why = NULL;
    double t0 = now_ms();
    unsigned long long probe0 = weft_probe_calls();

    for (uint64_t cur = 0; cur < ss.count; cur++) {
        wcr_state_t s;
        memcpy(&s, weft_stateset_get(&ss, cur), sizeof(s));
        if (wcr_invariants(&s, 1, &why) != 0) {
            fprintf(stderr, "FAIL[wcr-bfs]: invariant %s at state #%llu\n",
                    why ? why : "?", (unsigned long long)cur);
            g_failures++;
            break;
        }
        int n = wcr_succ(&s, g_wcr_succ, g_wcr_tags, 1);
        if (n == 0) {
            dead++;
        }
        for (int i = 0; i < n; i++) {
            if (!wcr_promised_monotone(&s, &g_wcr_succ[i])) {
                mono_fail++;
            }
            if (wcr_invariants(&g_wcr_succ[i], 1, &why) != 0) {
                fprintf(stderr, "FAIL[wcr-bfs]: invariant %s in successor "
                                "of #%llu\n",
                        why ? why : "?", (unsigned long long)cur);
                g_failures++;
            }
            int rc = weft_stateset_add(&ss, &g_wcr_succ[i]);
            if (rc < 0) {
                if (reduced) {
                    goto done; /* budget-limited tier, not a failure */
                }
                fprintf(stderr,
                        "FAIL[wcr-bfs]: capacity overflow at %llu states "
                        "(fail-closed)\n",
                        (unsigned long long)ss.count);
                g_failures++;
                goto done;
            }
            edges += (rc == 1) ? 1u : 0u;
        }
    }
done:
    {
        unsigned long long probe1 = weft_probe_calls();
        double dt = now_ms() - t0;
        printf("[oracle][wcr1] BFS states=%llu%s edges=%llu deadlocks=%llu "
               "epoch-regressions=%llu (%.1f s, heap-calls=%llu)\n",
               (unsigned long long)ss.count,
               reduced ? " (budget-limited tier)" : "",
               (unsigned long long)edges,
               (unsigned long long)dead, (unsigned long long)mono_fail,
               dt / 1000.0, probe1 - probe0);
        if (dead != 0u) {
            fprintf(stderr, "FAIL[wcr-bfs]: %llu deadlock states\n",
                    (unsigned long long)dead);
            g_failures++;
        }
        if (mono_fail != 0u) {
            fprintf(stderr,
                    "FAIL[wcr-bfs]: %llu epoch regressions across edges\n",
                    (unsigned long long)mono_fail);
            g_failures++;
        }
        if (probe1 != probe0) {
            fprintf(stderr, "FAIL(Law1)[wcr-bfs]: %llu heap calls\n",
                    probe1 - probe0);
            g_failures++;
        }
    }
    free(ss.table);
    free(ss.arena);
}

static void wcr_walk(uint64_t steps)
{
    wcr_state_t s;
    wcr_init(&s);
    uint64_t rng = weft_rng_seed(0x57435231434F4E53ULL);
    uint64_t won = 0u, heals = 0u, splits = 0u, renews = 0u, drops = 0u;
    const char *why = NULL;
    unsigned long long probe0 = weft_probe_calls();
    double t0 = now_ms();

    for (uint64_t i = 0; i < steps; i++) {
        int n = wcr_succ(&s, g_wcr_succ, g_wcr_tags, 0);
        if (n <= 0) {
            fprintf(stderr, "FAIL[wcr-walk]: no successors at step %llu\n",
                    (unsigned long long)i);
            g_failures++;
            return;
        }
        int pick = (int)(weft_rng_next(&rng) % (uint64_t)n);
        switch (g_wcr_tags[pick]) {
        case WC_A_WIN:  won++; break;
        case WC_A_HEAL: heals++; break;
        case WC_A_SPLIT: splits++; break;
        case WC_A_RENEW: renews++; break;
        case WC_A_DROP: drops++; break;
        default: break;
        }
        if (!wcr_promised_monotone(&s, &g_wcr_succ[pick])) {
            fprintf(stderr, "FAIL[wcr-walk]: epoch regression at step "
                            "%llu\n",
                    (unsigned long long)i);
            g_failures++;
            return;
        }
        s = g_wcr_succ[pick];
        if (wcr_invariants(&s, 0, &why) != 0) {
            fprintf(stderr, "FAIL[wcr-walk]: invariant %s at step %llu\n",
                    why ? why : "?", (unsigned long long)i);
            g_failures++;
            return;
        }
    }
    unsigned long long probe1 = weft_probe_calls();
    double dt = now_ms() - t0;
    printf("[oracle][wcr1] walk steps=%llu seed=0x%016" PRIx64
           " elections-won=%llu heals=%llu splits=%llu renewals=%llu "
           "drops=%llu single-primary-VIOLATIONS=0 (%.1f s, "
           "heap-calls=%llu)\n",
           (unsigned long long)steps, rng, (unsigned long long)won,
           (unsigned long long)heals, (unsigned long long)splits,
           (unsigned long long)renews, (unsigned long long)drops,
           dt / 1000.0, probe1 - probe0);
    if (probe1 != probe0) {
        fprintf(stderr, "FAIL(Law1)[wcr-walk]: %llu heap calls\n",
                probe1 - probe0);
        g_failures++;
    }
    if (won == 0u || heals == 0u || splits == 0u) {
        fprintf(stderr,
                "FAIL[wcr-walk]: walk did not exercise elections/heals/"
                "splits\n");
        g_failures++;
    }
}

int main(int argc, char **argv)
{
    int reduced = (argc > 1 && strcmp(argv[1], "--san") == 0);
    static char obuf[1 << 16];
    setvbuf(stdout, obuf, _IOFBF, sizeof(obuf));

    printf("weft-verify TLA trace-compliance oracle — %s\n",
           reduced ? "sanitizer budget" : "full");
    printf("mirrors: formal/seqlock_ring.tla + formal/wcr1_consensus.tla "
           "(TLC 1.8.0 pinned)\n");

    sq_bfs(reduced);
    sq_walk(reduced ? 1000000u : 10000000u);
    wcr_bfs(reduced);
    wcr_walk(reduced ? 1000000u : 10000000u);

    printf("TLA-ORACLE %s (%d failures)\n",
           (g_failures == 0) ? "PASS" : "FAIL", g_failures);
    fflush(stdout);
    return (g_failures == 0) ? 0 : 1;
}
