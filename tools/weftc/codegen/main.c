// main.c — weftc-codegen CLI driver (Pillar 1 codegen front door).
//
//   weftc-codegen --ir schema.weft.json --target all --out gen/
//
// The CLI is deliberately tiny: load IR (verify every layout law), then hand
// the verified IR to the requested backend(s). Exit codes:
//   0 ok (warnings allowed — they are printed to stderr)
//   1 usage
//   2 IR load/verification error
//   3 I/O error

#define _POSIX_C_SOURCE 200809L

#include "weftc_codegen.h"

static void usage(FILE* f)
{
    fputs(
        "weftc-codegen " WEFTC_CODEGEN_VERSION " — native code generators for Project weftc\n"
        "\n"
        "usage: weftc-codegen --ir <file.weft.json> [options]\n"
        "\n"
        "required:\n"
        "  --ir FILE          weft-ir JSON v1 file (Engineer 1's compiler output;\n"
        "                     see README §IR for the contract)\n"
        "\n"
        "options:\n"
        "  --target T         c | rust | wgsl | glsl | all   (default: all)\n"
        "  --out DIR          output directory (default: .)\n"
        "  --core-name NAME   shared core stem (default: weft_projection_core)\n"
        "  --rust-traits LIST comma list from {bytemuck, zerocopy}\n"
        "                     (emits feature-gated impls; default: none)\n"
        "  --verify-only      verify IR layout laws, emit nothing\n"
        "  --version          print version\n"
        "  --help             this text\n",
        f);
}

int main(int argc, char** argv)
{
    const char* ir_path = NULL;
    const char* target = "all";
    const char* out_dir = ".";
    const char* core_name = "weft_projection_core";
    const char* rust_traits = NULL;
    bool verify_only = false;

    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--ir") == 0 && k + 1 < argc) {
            ir_path = argv[++k];
        } else if (strcmp(argv[k], "--target") == 0 && k + 1 < argc) {
            target = argv[++k];
        } else if (strcmp(argv[k], "--out") == 0 && k + 1 < argc) {
            out_dir = argv[++k];
        } else if (strcmp(argv[k], "--core-name") == 0 && k + 1 < argc) {
            core_name = argv[++k];
        } else if (strcmp(argv[k], "--rust-traits") == 0 && k + 1 < argc) {
            rust_traits = argv[++k];
        } else if (strcmp(argv[k], "--verify-only") == 0) {
            verify_only = true;
        } else if (strcmp(argv[k], "--version") == 0) {
            printf("weftc-codegen %s\n", WEFTC_CODEGEN_VERSION);
            return 0;
        } else if (strcmp(argv[k], "--help") == 0 || strcmp(argv[k], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "weftc-codegen: unknown argument '%s'\n", argv[k]);
            usage(stderr);
            return 1;
        }
    }
    if (!ir_path) {
        fprintf(stderr, "weftc-codegen: --ir is required\n");
        usage(stderr);
        return 1;
    }

    char err[512];
    weft_ir* ir = weft_ir_load(ir_path, err);
    if (!ir) {
        fprintf(stderr, "weftc-codegen: %s\n", err);
        return 2;
    }

    if (verify_only) {
        printf("weftc-codegen: %s: %u struct(s) verified (module '%s')\n",
               ir_path, ir->nstructs, ir->module);
        for (uint32_t k = 0; k < ir->nstructs; k++) {
            weft_struct* s = &ir->ss[k];
            printf("  %-20s %-18s %4u bytes  align %-3u %s\n",
                   s->name, s->c_name, s->size, s->align,
                   s->root ? s->schema_id_str : "(embedded)");
        }
        return 0;
    }

    bool want_c = strcmp(target, "c") == 0 || strcmp(target, "all") == 0;
    bool want_rs = strcmp(target, "rust") == 0 || strcmp(target, "rs") == 0 || strcmp(target, "all") == 0;
    bool want_wgsl = strcmp(target, "wgsl") == 0 || strcmp(target, "all") == 0;
    bool want_glsl = strcmp(target, "glsl") == 0 || strcmp(target, "all") == 0;
    if (!want_c && !want_rs && !want_wgsl && !want_glsl) {
        fprintf(stderr, "weftc-codegen: unknown target '%s' (c|rust|wgsl|glsl|all)\n", target);
        return 1;
    }
    if (strcmp(target, "rs") == 0) {
        target = "rust";
    }

    weft_emit_opts o;
    memset(&o, 0, sizeof(o));
    o.out_dir = out_dir;
    o.core_name = core_name;
    if (rust_traits) {
        // strict list parse
        char buf[128];
        if (strlen(rust_traits) >= sizeof(buf)) {
            fprintf(stderr, "weftc-codegen: --rust-traits list too long\n");
            return 1;
        }
        strcpy(buf, rust_traits);
        char* save = NULL;
        for (char* tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
            if (strcmp(tok, "bytemuck") == 0) {
                o.rust_bytemuck = true;
            } else if (strcmp(tok, "zerocopy") == 0) {
                o.rust_zerocopy = true;
            } else {
                fprintf(stderr, "weftc-codegen: unknown rust trait '%s' (bytemuck|zerocopy)\n", tok);
                return 1;
            }
        }
    }

    if (weft_mkdir_p(out_dir) != 0) {
        fprintf(stderr, "weftc-codegen: cannot create output dir '%s'\n", out_dir);
        return 3;
    }

    int rc = 0;
    if (want_c && weft_emit_c(ir, &o) != 0) rc = 3;
    if (want_rs && weft_emit_rust(ir, &o) != 0) rc = 3;
    if (want_wgsl && weft_emit_wgsl(ir, &o) != 0) rc = 3;
    if (want_glsl && weft_emit_glsl(ir, &o) != 0) rc = 3;
    return rc;
}
