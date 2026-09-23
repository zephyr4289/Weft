#include <stdlib.h>

/* clean.c — clean C fixture: ZERO findings expected.
   Stack allocation (alloca) is allowed in hot paths by the rule matrix. */

__attribute__((weft_hot))
static void on_frame(const unsigned char *pkt, struct ctx *ctx) {
    unsigned long acc = 0;
    for (int i = 0; i < 64; i++) {
        acc += (unsigned long)pkt[i];
    }
    ctx->acc = acc;
    void *stack_buf = alloca(32); /* stack allocation is allowed */
    (void)stack_buf;
}

static void cold_path(void) {
    void *p = malloc(64); /* NOT hot: legal */
    (void)p;
}
