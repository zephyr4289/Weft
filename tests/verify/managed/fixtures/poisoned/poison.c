#include <stdlib.h>

/* poison.c — poisoned C fixture. Every commented rule id MUST fire. */

__attribute__((weft_hot))
static void on_frame(const unsigned char *pkt, struct ctx *ctx) {
    void *p = malloc(64);                        /* expect: WV-C-001 */
    struct hdr *q = calloc(1, 128);              /* expect: WV-C-002 */
    ctx->scratch = realloc(ctx->scratch, 256);   /* expect: WV-C-003 */
    char *dup = strdup("tick");                  /* expect: WV-C-005 */
    (void)p; (void)q; (void)dup;
    ctx->acc = (unsigned long)pkt[0];
}

static void cold_path(void) {
    void *p = malloc(64); /* NOT hot: no finding */
    (void)p;
}
