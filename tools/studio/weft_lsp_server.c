/* weft_lsp_server.c — Weft Studio headless LSP server over stdio.
 *
 * Tool layer (NOT the engine): all stdio lives here, pumped through the
 * engine's callback seam — the engine itself stays syscall-free and
 * wasm32-portable. Speaks Content-Length framed JSON-RPC 2.0 on
 * stdin/stdout; diagnostics notifications interleave with responses.
 *
 * usage: weft-lsp-server < in.lsp  > out.lsp
 */
#define _POSIX_C_SOURCE 199309L
#include "weft_studio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long io_read(void *user, char *buf, unsigned long len)
{
    FILE *f = (FILE *)user;
    size_t got = fread(buf, 1, len, f);
    (void)f;
    return (long)got;
}

static long io_write(void *user, const char *buf, unsigned long len)
{
    FILE *f = (FILE *)user;
    size_t put = fwrite(buf, 1, len, f);
    if (put == len) fflush(f);
    return (long)put;
}

int main(void)
{
    void *mem = malloc(weft_lsp_ctx_size());
    weft_lsp_ctx_t *ctx;
    int rc;
    if (!mem) return 2;
    if (weft_lsp_ctx_init(mem, weft_lsp_ctx_size(), &ctx)) return 2;
    rc = weft_lsp_serve(ctx, io_read, (void *)stdin,
                     io_write, (void *)stdout);
    return rc == WEFT_STUDIO_OK ? 0 : 1;
}
