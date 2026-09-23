/* ---------------------------------------------------------------------------
 * weftc_lint_main.c — Weft Pillar 8: `weftc --lint-alloc` command-line driver
 *
 * TERRITORY: tools/weftc/lint/ (Pillar 8 directive, Engineer 1).
 *
 * Usage:
 *   weftc-lint [--lint-alloc] [--lang=c|cpp|rust|ts|swift|dart|auto]
 *              [--format=gcc|json] [--bench=N] [--rounds=N]
 *              [--no-strict] [--version] FILE...
 *
 * Exit codes (fail-closed):
 *   0  no diagnostics (or bench-only run)
 *   1  diagnostics found (default strict behaviour)
 *   2  usage / I/O error
 * ------------------------------------------------------------------------- */

#include "weftc_lint_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_prog = "weftc-lint";

static void usage(FILE *out)
{
    fprintf(out,
            "usage: %s [options] FILE...\n"
            "  --lint-alloc        explicit enable (default behaviour)\n"
            "  --lang=L            c|cpp|rust|ts|swift|dart|auto (default "
            "auto)\n"
            "  --format=F          gcc|json (default gcc)\n"
            "  --bench=N           run the SLA benchmark on a synthetic AST\n"
            "                       of >= N nodes (default 150000)\n"
            "  --rounds=N          benchmark rounds (default 5)\n"
            "  --no-strict         exit 0 even when diagnostics are found\n"
            "  --version           print engine version and ABI\n",
            k_prog);
}

static int parse_lang(const char *s, weft_lint_lang_t *out)
{
    if (strcmp(s, "c") == 0) {
        *out = WEFT_LINT_LANG_C;
    } else if (strcmp(s, "cpp") == 0 || strcmp(s, "c++") == 0) {
        *out = WEFT_LINT_LANG_CPP;
    } else if (strcmp(s, "rust") == 0 || strcmp(s, "rs") == 0) {
        *out = WEFT_LINT_LANG_RUST;
    } else if (strcmp(s, "ts") == 0 || strcmp(s, "typescript") == 0) {
        *out = WEFT_LINT_LANG_TYPESCRIPT;
    } else if (strcmp(s, "swift") == 0) {
        *out = WEFT_LINT_LANG_SWIFT;
    } else if (strcmp(s, "dart") == 0) {
        *out = WEFT_LINT_LANG_DART;
    } else if (strcmp(s, "auto") == 0) {
        *out = WEFT_LINT_LANG_AUTO;
    } else {
        return -1;
    }
    return 0;
}

static const char *sev_name(uint8_t sev)
{
    switch (sev) {
    case WEFT_LINT_ERROR:
        return "error";
    case WEFT_LINT_WARNING:
        return "warning";
    default:
        return "note";
    }
}

static void json_escape(const char *s, char *dst, size_t cap)
{
    size_t o = 0u;
    for (size_t i = 0u; s[i] != '\0' && o + 6u < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20u) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", (unsigned)c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static int read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "%s: cannot open '%s'\n", k_prog, path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1u);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return -1;
    }
    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

int main(int argc, char **argv)
{
    weft_lint_lang_t lang = WEFT_LINT_LANG_AUTO;
    int fmt_json = 0;
    int strict = 1;
    int do_bench = 0;
    uint32_t bench_nodes = 150000u;
    uint32_t bench_rounds = 5u;
    int nfiles = 0;
    uint32_t total_diags = 0u;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--lint-alloc") == 0 || strcmp(a, "--lint") == 0) {
            continue; /* default behaviour */
        }
        if (strcmp(a, "--version") == 0) {
            printf("%s: %s (ABI 0x%04x)\n", k_prog, weftc_lint_version(),
                   (unsigned)weftc_lint_abi_version());
            return 0;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strncmp(a, "--lang=", 7) == 0) {
            if (parse_lang(a + 7, &lang) != 0) {
                fprintf(stderr, "%s: unknown language '%s'\n", k_prog, a + 7);
                return 2;
            }
            continue;
        }
        if (strncmp(a, "--format=", 9) == 0) {
            if (strcmp(a + 9, "gcc") == 0) {
                fmt_json = 0;
            } else if (strcmp(a + 9, "json") == 0) {
                fmt_json = 1;
            } else {
                fprintf(stderr, "%s: unknown format '%s'\n", k_prog, a + 9);
                return 2;
            }
            continue;
        }
        if (strncmp(a, "--bench=", 8) == 0) {
            do_bench = 1;
            bench_nodes = (uint32_t)strtoul(a + 8, NULL, 10);
            if (bench_nodes == 0u) {
                bench_nodes = 150000u;
            }
            continue;
        }
        if (strncmp(a, "--rounds=", 9) == 0) {
            bench_rounds = (uint32_t)strtoul(a + 9, NULL, 10);
            if (bench_rounds == 0u) {
                bench_rounds = 5u;
            }
            continue;
        }
        if (strcmp(a, "--no-strict") == 0) {
            strict = 0;
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "%s: unknown option '%s'\n", k_prog, a);
            usage(stderr);
            return 2;
        }
        argv[nfiles++] = argv[i];
    }

    if (do_bench) {
        double best = 0.0, avg = 0.0;
        uint32_t nodes = 0u, diags = 0u;
        int rc = weftc_lint_benchmark(bench_nodes, bench_rounds, &best, &avg,
                                      &nodes, &diags);
        if (rc != 0) {
            fprintf(stderr, "%s: benchmark failed (%d)\n", k_prog, rc);
            return 2;
        }
        printf("bench: nodes=%u rounds=%u best=%.3fms avg=%.3fms "
               "diags=%u (%.1f ns/node)\n",
               (unsigned)nodes, (unsigned)bench_rounds, best, avg,
               (unsigned)diags, (nodes > 0u) ? (best * 1000000.0 /
                                                (double)nodes)
                                             : 0.0);
        printf("SLA: >100,000 AST nodes in <15 ms -> %s\n",
               (nodes >= 100000u && best < 15.0) ? "PASS" : "FAIL");
        if (nodes < 100000u || best >= 15.0) {
            return 1;
        }
        if (nfiles == 0) {
            return 0;
        }
    }

    if (nfiles == 0 && !do_bench) {
        usage(stderr);
        return 2;
    }

    if (fmt_json) {
        printf("[");
    }

    for (int fi = 0; fi < nfiles; fi++) {
        char *src = NULL;
        size_t srclen = 0u;
        if (read_file(argv[fi], &src, &srclen) != 0) {
            if (fmt_json) {
                printf("]");
            }
            return 2;
        }
        weft_lint_ast_t *ast = NULL;
        int prc = weftc_lint_parse(src, srclen, argv[fi], lang,
                                   weft_lint_default_alloc, NULL, &ast);
        free(src);
        if (prc < 0 || ast == NULL) {
            fprintf(stderr, "%s: parse failed for '%s' (%d)\n", k_prog,
                    argv[fi], prc);
            return 2;
        }
        weft_lint_diag_t *diags = malloc(sizeof(weft_lint_diag_t) * 4096u);
        if (diags == NULL) {
            weftc_lint_free_ast(ast, weft_lint_default_alloc, NULL);
            return 2;
        }
        uint32_t written = 0u, found = 0u;
        (void)weftc_lint_scan_ast(ast, diags, 4096u, &written, &found);
        total_diags += found;

        if (fmt_json) {
            for (uint32_t d = 0; d < written; d++) {
                char em[360], er[430];
                json_escape(diags[d].message, em, sizeof(em));
                json_escape(diags[d].remediation, er, sizeof(er));
                printf("%s\n  {\"file\":\"%s\",\"line\":%u,\"col\":%u,"
                       "\"severity\":\"%s\",\"rule\":\"%s\",\n   "
                       "\"message\":\"%s\",\n   \"remediation\":\"%s\"}",
                       (d > 0u || fi > 0) ? "," : "", diags[d].file,
                       (unsigned)diags[d].line, (unsigned)diags[d].col,
                       sev_name(diags[d].severity), diags[d].rule, em, er);
            }
        } else {
            for (uint32_t d = 0; d < written; d++) {
                printf("%s:%u:%u: %s: %s [%s]\n", diags[d].file,
                       (unsigned)diags[d].line, (unsigned)diags[d].col,
                       sev_name(diags[d].severity), diags[d].message,
                       diags[d].rule);
                printf("%s:%u:%u: note: %s\n", diags[d].file,
                       (unsigned)diags[d].line, (unsigned)diags[d].col,
                       diags[d].remediation);
            }
        }
        if (found > written) {
            fprintf(stderr,
                    "%s: note: %u further diagnostic(s) truncated for "
                    "'%s'\n",
                    k_prog, (unsigned)(found - written), argv[fi]);
        }
        free(diags);
        weftc_lint_free_ast(ast, weft_lint_default_alloc, NULL);
    }

    if (fmt_json) {
        printf("\n]\n");
    }

    if (strict && total_diags > 0u) {
        printf("%s: %u diagnostic(s) across %d file(s)\n", k_prog,
               (unsigned)total_diags, nfiles);
        return 1;
    }
    return 0;
}
