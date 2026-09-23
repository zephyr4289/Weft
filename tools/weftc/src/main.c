/* main.c — the weftc command line (RFC-0017 §7).
 *
 *   weftc check   FILE.weft      validate: parse + layout + diagnostics
 *   weftc inspect FILE.weft      validate + ASCII memory map on stdout
 *   weftc compile FILE [flags]  emit artifacts (binary IR + verify header;
 *                                JSON IR via --dump-ir / --emit-json)
 *
 * Exit codes: 0 = clean, 1 = diagnostics reported, 2 = usage / fatal.
 * Diagnostics always go to stderr; artifacts and summaries to stdout —
 * scripts can pipe one and grep the other (house convention).
 */
#define _POSIX_C_SOURCE 200809L
#include "weftc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { CMD_NONE, CMD_CHECK, CMD_INSPECT, CMD_COMPILE } Cmd;

static void usage(FILE *out)
{
    fputs(
"weftc " WEFTC_VERSION " — universal zero-serialization schema compiler\n"
"(RFC-0017; .weft -> bit-exact frozen memory layouts, 0ns decode)\n"
"\n"
"usage:\n"
"  weftc check   <file.weft>              validate (parse + layout)\n"
"  weftc inspect <file.weft>              validate + ASCII memory map\n"
"  weftc compile <file.weft> [flags]      emit artifacts\n"
"\n"
"compile flags:\n"
"  -o, --out PREFIX    artifact prefix (default: input path minus .weft)\n"
"                        writes PREFIX.weftir (binary IR) and\n"
"                        PREFIX.verify.h (C11 _Static_assert pin)\n"
"      --dump-ir       JSON IR to stdout\n"
"      --emit-json F   JSON IR to file F ('-' = stdout)\n"
"      --emit-ir F     binary IR to file F ('-' = stdout)\n"
"      --emit-header F verify header to file F ('-' = stdout)\n"
"      --color=WHEN    auto | always | never   (default: auto)\n"
"      --werror        treat warnings as errors\n"
"      --version       print version and exit\n"
"      --help          this help\n"
"\n"
"exit codes: 0 clean, 1 diagnostics, 2 usage/fatal\n",
        out);
}

static int write_buf_to_file(const char *path, const uint8_t *data,
                             size_t len)
{
    if (strcmp(path, "-") == 0) {
        fwrite(data, 1, len, stdout);
        return fflush(stdout) == 0 && !ferror(stdout) ? 0 : -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    return 0;
}

static int emit_json_file(const WeftUnit *u, const char *path)
{
    if (strcmp(path, "-") == 0)
        return weft_dump_json(u, stdout);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int r = weft_dump_json(u, f);
    if (fclose(f) != 0) r = -1;
    return r;
}

static int emit_header_file(const WeftUnit *u, const char *path)
{
    if (strcmp(path, "-") == 0)
        return weft_emit_verify_header(u, stdout);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int r = weft_emit_verify_header(u, f);
    if (fclose(f) != 0) r = -1;
    return r;
}

/* Strip a trailing ".weft" (or any final '.' component) for -o defaults. */
static char *stem_of(const char *path, Arena *ar)
{
    size_t n = strlen(path);
    const char *dot = strrchr(path, '.');
    size_t cut = (dot && dot != path) ? (size_t)(dot - path) : n;
    char *s = arena_alloc(ar, cut + 1, 1);
    memcpy(s, path, cut);
    s[cut] = '\0';
    return s;
}

int main(int argc, char **argv)
{
    Cmd cmd = CMD_NONE;
    const char *file = NULL;
    const char *out_prefix = NULL;
    const char *json_path = NULL, *ir_path = NULL, *hdr_path = NULL;
    int dump_ir = 0, werror = 0;
    int color_mode = 0; /* 0 auto, 1 always, -1 never */

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (cmd == CMD_NONE) {
            if (strcmp(a, "check") == 0)   { cmd = CMD_CHECK;   continue; }
            if (strcmp(a, "inspect") == 0) { cmd = CMD_INSPECT; continue; }
            if (strcmp(a, "compile") == 0) { cmd = CMD_COMPILE; continue; }
            if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
                printf("weftc %s (ir %u)\n", WEFTC_VERSION,
                       (unsigned)WEFTC_IR_VERSION);
                return 0;
            }
            if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
                usage(stdout);
                return 0;
            }
            fprintf(stderr, "weftc: error: unknown command `%s`\n\n", a);
            usage(stderr);
            return 2;
        }
        if (strcmp(a, "--werror") == 0) { werror = 1; continue; }
        if (strcmp(a, "--dump-ir") == 0) { dump_ir = 1; json_path = "-"; continue; }
        if (strcmp(a, "--color=auto") == 0)   { color_mode = 0;  continue; }
        if (strcmp(a, "--color=always") == 0) { color_mode = 1;  continue; }
        if (strcmp(a, "--color=never") == 0)  { color_mode = -1; continue; }
        if (strcmp(a, "--color") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "auto") == 0) color_mode = 0;
            else if (strcmp(argv[i], "always") == 0) color_mode = 1;
            else if (strcmp(argv[i], "never") == 0) color_mode = -1;
            else {
                fprintf(stderr, "weftc: error: --color expects "
                                "auto|always|never\n");
                return 2;
            }
            continue;
        }
        if ((strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0)) {
            if (i + 1 >= argc) {
                fprintf(stderr, "weftc: error: %s needs a value\n", a);
                return 2;
            }
            out_prefix = argv[++i];
            continue;
        }
        if (strcmp(a, "--emit-json") == 0 || strcmp(a, "--emit-ir") == 0 ||
            strcmp(a, "--emit-header") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "weftc: error: %s needs a value\n", a);
                return 2;
            }
            const char *v = argv[++i];
            if (strcmp(a, "--emit-json") == 0)   json_path = v;
            if (strcmp(a, "--emit-ir") == 0)     ir_path = v;
            if (strcmp(a, "--emit-header") == 0) hdr_path = v;
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "weftc: error: unknown flag `%s`\n\n", a);
            usage(stderr);
            return 2;
        }
        if (file) {
            fprintf(stderr, "weftc: error: multiple input files "
                            "(`%s` and `%s`) — one schema per invocation\n",
                    file, a);
            return 2;
        }
        file = a;
    }

    if (cmd == CMD_NONE) {
        usage(stderr);
        return 2;
    }
    if (!file) {
        fprintf(stderr, "weftc: error: no input file\n\n");
        usage(stderr);
        return 2;
    }

    int color = color_mode == 1 ? 1 : 0;
    if (color_mode == 0) {
        const char *nc = getenv("NO_COLOR");
        color = (nc && *nc) ? 0 : isatty(fileno(stderr));
    }
    unsigned opts = (color ? WEFT_OPT_COLOR : 0u) | (werror ? WEFT_OPT_WERROR : 0u);

    WeftUnit *u = weft_compile_file(file, opts);
    diag_render_all(&u->diags, stderr);
    size_t errs = weft_error_count(u);

    if (errs > 0) {
        weft_unit_free(u);
        return 1;
    }

    int rc = 0;
    switch (cmd) {
    case CMD_CHECK:
        printf("weftc: %s: %zu decl%s OK (%zu error%s, %zu warning%s)\n",
               file, u->ndecls, u->ndecls == 1 ? "" : "s",
               u->diags.n_errors, u->diags.n_errors == 1 ? "" : "s",
               u->diags.n_warnings, u->diags.n_warnings == 1 ? "" : "s");
        break;

    case CMD_INSPECT:
        weft_inspect(u, stdout);
        break;

    case CMD_COMPILE: {
        Arena *ar = arena_create();
        if (!out_prefix) out_prefix = stem_of(file, ar);
        const char *jpath = json_path;
        const char *ipath = ir_path;
        const char *hpath = hdr_path;
        if (!jpath && !ipath && !hpath && !dump_ir) {
            /* default artifact pair: binary IR + verify header */
            size_t n = strlen(out_prefix);
            ipath = arena_alloc(ar, n + 16, 1);
            snprintf((char *)ipath, n + 16, "%s.weftir", out_prefix);
            hpath = arena_alloc(ar, n + 16, 1);
            snprintf((char *)hpath, n + 16, "%s.verify.h", out_prefix);
        }
        if (jpath && emit_json_file(u, jpath) != 0) {
            fprintf(stderr, "weftc: error: cannot write JSON IR to `%s`\n",
                    jpath);
            rc = 2;
        }
        if (rc == 0 && ipath) {
            ByteBuf bin;
            bb_init(&bin, ar);
            if (weft_dump_binary(u, &bin) != 0 ||
                write_buf_to_file(ipath, bin.data, bin.len) != 0) {
                fprintf(stderr, "weftc: error: cannot write binary IR to "
                                "`%s`\n", ipath);
                rc = 2;
            }
        }
        if (rc == 0 && hpath && emit_header_file(u, hpath) != 0) {
            fprintf(stderr, "weftc: error: cannot write verify header to "
                            "`%s`\n", hpath);
            rc = 2;
        }
        if (rc == 0)
            printf("weftc: %s: %zu decl%s compiled — schema_id ",
                   file, u->ndecls, u->ndecls == 1 ? "" : "s"),
            printf("0x%016llx", (unsigned long long)u->schema_id),
            printf("\n");
        arena_destroy(ar);
        break;
    }

    default:
        break;
    }

    weft_unit_free(u);
    return rc;
}
