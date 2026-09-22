/* test_alloc_probe.c — A-series: Law 1 evidence via malloc interposition.
 *
 * 0 allocation events over 200,000 steady-state cycles (100k LSP document
 * cycles + 100k .weftrec replay cycles), proven by interposing
 * malloc/calloc/realloc/free through dlsym(RTLD_NEXT):
 *
 *   - the engines' contexts are allocated ONCE during setup (Law 1 scopes
 *     setup out of the steady state);
 *   - dlsym's own bootstrap allocations land in a static arena and are
 *     never counted (guarded by in_hook);
 *   - stdout is given a static buffer and warmed before arming so the
 *     first printf cannot allocate inside the measured window;
 *   - a mallinfo2 delta over the same window is the secondary evidence
 *     (this is the check the ASan leg runs, where interposition is
 *     preempted by the sanitizer's own interceptors).
 */
#define _GNU_SOURCE
#include "test_studio_harness.h"
#include <dlfcn.h>
#include <malloc.h>

/* ---- interposition -------------------------------------------------- */
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void  (*real_free)(void *);

static unsigned long long g_events = 0;
static int g_in_hook = 0;
static int g_armed = 0;

/* bootstrap arena: dlsym allocates on first use */
static char g_boot[65536] __attribute__((aligned(16)));
static size_t g_boot_used = 0;

static void *boot_alloc(size_t n)
{
    void *p;
    if (n == 0) n = 1;
    if (g_boot_used + n + 16u > sizeof g_boot) return NULL;
    p = g_boot + g_boot_used;
    g_boot_used += (n + 15u) & ~(size_t)15u;
    return p;
}
static int is_boot_ptr(const void *p)
{
    return (const char *)p >= g_boot &&
           (const char *)p < g_boot + sizeof g_boot;
}

/* ISO C forbids object-to-function-pointer conversion (dlsym returns
 * void*), so the pointer is moved with memcpy — the POSIX-blessed
 * workaround that also satisfies -pedantic (Pillar-6 precedent). */
static void sym_load(void **fp, const char *name)
{
    void *sym = dlsym(RTLD_NEXT, name);
    memcpy(fp, &sym, sizeof sym);
}

static void resolve_real(void)
{
    if (real_malloc) return;
    if (g_in_hook) return;
    g_in_hook = 1;
    sym_load((void **)&real_malloc, "malloc");
    sym_load((void **)&real_calloc, "calloc");
    sym_load((void **)&real_realloc, "realloc");
    sym_load((void **)&real_free, "free");
    g_in_hook = 0;
}

void *malloc(size_t n)
{
    if (!real_malloc) {
        void *p;
        resolve_real();
        if (!real_malloc) return boot_alloc(n);
        p = real_malloc(n);
        return p;
    }
    if (g_armed && !g_in_hook) g_events++;
    return real_malloc(n);
}

void *calloc(size_t a, size_t b)
{
    if (!real_calloc) {
        void *p;
        resolve_real();
        if (!real_calloc) {
            p = boot_alloc(a * b);
            if (p) memset(p, 0, a * b);
            return p;
        }
        return real_calloc(a, b);
    }
    if (g_armed && !g_in_hook) g_events++;
    return real_calloc(a, b);
}

void *realloc(void *old, size_t n)
{
    if (is_boot_ptr(old)) {
        void *p = malloc(n);        /* counted if armed */
        if (p && old) memcpy(p, old, n);
        return p;
    }
    if (!real_realloc) {
        resolve_real();
        if (!real_realloc) return boot_alloc(n);
    }
    if (g_armed && !g_in_hook) g_events++;
    return real_realloc(old, n);
}

void free(void *p)
{
    if (!p || is_boot_ptr(p)) return;
    if (!real_free) { resolve_real(); }
    if (!real_free) return;
    real_free(p);
}

/* ---- probe bodies ---------------------------------------------------- */
static const char A_DOC[] =
"struct Probe {\\n"
"    a: u64,\\n"
"    b: u32,\\n"
"    c: [u8; 12],\\n"
"}\\n";

static char g_a_req[4096];
static char g_a_resp[256 * 1024];
static char g_a_notif[256 * 1024];

static void a_lsp_cycle(weft_lsp_ctx_t *L, int i)
{
    size_t rl = 0, nl = 0;
    char req[1024];
    /* alternate incremental edits on line 1 */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
             "\"params\":{\"textDocument\":{\"uri\":\"file:///a.weft\","
             "\"version\":%d},\"contentChanges\":[{\"range\":{"
             "\"start\":{\"line\":1,\"character\":12},"
             "\"end\":{\"line\":1,\"character\":13}},\"text\":\"%d\"}]}}",
             100 + i, i % 10);
    (void)weft_lsp_handle(L, req, strlen(req), g_a_resp, sizeof g_a_resp,
                          &rl, g_a_notif, sizeof g_a_notif, &nl);
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///a.weft\"},\"position\":{\"line\":1,"
             "\"character\":5}}}", i);
    (void)weft_lsp_handle(L, req, strlen(req), g_a_resp, sizeof g_a_resp,
                          &rl, g_a_notif, sizeof g_a_notif, &nl);
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/"
             "completion\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///a.weft\"},\"position\":{\"line\":2,"
             "\"character\":10}}}", i);
    (void)weft_lsp_handle(L, req, strlen(req), g_a_resp, sizeof g_a_resp,
                          &rl, g_a_notif, sizeof g_a_notif, &nl);
    strcpy(req, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/"
                "semanticTokens/full\",\"params\":{\"textDocument\":"
                "{\"uri\":\"file:///a.weft\"}}}");
    (void)weft_lsp_handle(L, req, strlen(req), g_a_resp, sizeof g_a_resp,
                          &rl, g_a_notif, sizeof g_a_notif, &nl);
}

static uint8_t g_a_buf[1 << 16] __attribute__((aligned(64)));
static weftrec_index_entry_t g_a_idx[64] __attribute__((aligned(8)));
static uint8_t g_a_pl[64];
static uint8_t g_a_scratch[128];

static void a_rec_cycle(void)
{
    weftrec_builder_t b;
    weftrec_reader_t r;
    weftrec_walker_t w;
    weftrec_frame_view_t v;
    uint64_t len = 0;
    int i, rc;
    weftrec_builder_init(&b, g_a_buf, sizeof g_a_buf, g_a_idx, 64);
    for (i = 0; i < 16; i++) {
        g_a_pl[0] = (uint8_t)i;
        weftrec_builder_append(&b, 1000 + (uint64_t)i * 100, 0, g_a_pl, 64,
                               (i & 1) ? WEFTREC_CODEC_DZV
                                       : WEFTREC_CODEC_RAW);
    }
    weftrec_builder_finish(&b, &len);
    if (weftrec_reader_open(&r, g_a_buf, len) == WEFT_STUDIO_OK) {
        weftrec_frame_view_t sv;
        (void)weftrec_seek_timestamp(&r, 1000 + 500, &sv);
        weftrec_walker_init(&w, &r, 0, g_a_scratch, sizeof g_a_scratch);
        while ((rc = weftrec_frame_next(&w, &v)) == WEFT_STUDIO_OK) {}
        (void)rc;
    }
}

int main(void)
{
    void *lsp_mem;
    weft_lsp_ctx_t *L;
    static char outbuf[4096];
    struct mallinfo2 before, after;
    int i;
    uint64_t t0;

    /* warm stdio with a static buffer BEFORE arming */
    setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);
    printf("alloc probe: warming\n");
    fflush(stdout);

    /* setup (outside Law 1's steady state) */
    lsp_mem = malloc(weft_lsp_ctx_size());
    if (!lsp_mem) return 2;
    if (weft_lsp_ctx_init(lsp_mem, weft_lsp_ctx_size(), &L)) return 2;
    snprintf(g_a_req, sizeof g_a_req,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
             "\"params\":{\"textDocument\":{\"uri\":\"file:///a.weft\","
             "\"languageId\":\"weft\",\"version\":1,\"text\":\"%s\"}}}",
             A_DOC);
    {
        size_t rl = 0, nl = 0;
        weft_lsp_handle(L, g_a_req, strlen(g_a_req), g_a_resp,
                        sizeof g_a_resp, &rl, g_a_notif, sizeof g_a_notif,
                        &nl);
    }
    /* warm-up cycle of both surfaces (lazy paths settle) */
    a_lsp_cycle(L, 0);
    a_rec_cycle();
    fflush(stdout);
    resolve_real();

    before = mallinfo2();
    g_armed = 1;
    t0 = harness_now_ns();
    for (i = 0; i < 100000; i++)
        a_lsp_cycle(L, i);
    for (i = 0; i < 100000; i++)
        a_rec_cycle();
    g_armed = 0;
    {
        uint64_t dt = harness_now_ns() - t0;
        printf("  200000 cycles in %llu ms\n",
               (unsigned long long)(dt / 1000000u));
    }
    after = mallinfo2();

    printf("  interposition events: %llu (LAW 1: must be 0)\n", g_events);
    printf("  mallinfo2 arena delta: %lld B, uordblks delta: %lld B\n",
           (long long)(after.arena - before.arena),
           (long long)(after.uordblks - before.uordblks));
    g_checks++;
    if (g_events != 0) {
        g_failures++;
        fprintf(stderr, "FAIL A1: %llu allocation events in the steady "
                "state\n", g_events);
    }
    g_checks++;
    if (after.uordblks != before.uordblks) {
        g_failures++;
        fprintf(stderr, "FAIL A2: mallinfo2 uordblks changed by %lld\n",
                (long long)(after.uordblks - before.uordblks));
    }
    fflush(stdout);
    return harness_summary("test_alloc_probe (A-series)");
}
