// fanout_chaos_runner.c — RFC 0011 chaos engine CLI (C reference oracle).
//
// Modes:
//   stepped <steps> <slots> <words> <readers> <frames> <chaosRate> <seed> [out.json]
//       The deterministic scheduler. Pure function of the config; the
//       verdict JSON is byte-identical to every port's stepped engine for
//       the same config (run_chaos_parity.sh diffs C vs TS vs JVM vs Dart).
//   free <frames> <slots> <words> <readers> <chaosRate> <seed> [out.json]
//       Real threads over the production ring, seed-deterministic fault
//       sequence. The 10,000,000-frame tier lives here (nightly).
//   selftest
//       PRNG replay + known-good tiny run. Exit 0 required before any long
//       run (the chaos shard gates on it).
//
// Exit codes: 0 = PASS, 1 = protocol violation, 2 = contract misuse.
// The verdict JSON always lands in out.json (default: stdout when omitted).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fanout_chaos.h"

static int write_json(const char* path, const char* json) {
    if (!path) return 0;
    FILE* f = fopen(path, "w");
    if (!f) { perror("chaos-runner: open out"); return -1; }
    fputs(json, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

static int run_stepped(int argc, char** argv) {
    if (argc < 8) {
        fprintf(stderr, "stepped <steps> <slots> <words> <readers> <frames> <chaosRate> <seed> [out.json]\n");
        return 2;
    }
    weft_chaos_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.steps      = (uint32_t)strtoul(argv[2], NULL, 10);
    cfg.slots      = (uint32_t)strtoul(argv[3], NULL, 10);
    cfg.words      = (uint32_t)strtoul(argv[4], NULL, 10);
    cfg.readers    = (uint32_t)strtoul(argv[5], NULL, 10);
    cfg.frames     = (uint32_t)strtoul(argv[6], NULL, 10);
    cfg.chaos_rate = (uint32_t)strtoul(argv[7], NULL, 10);
    cfg.seed       = (uint32_t)strtoul(argv[8], NULL, 10);
    const char* out = argc > 9 ? argv[9] : NULL;

    weft_chaos_verdict_t v;
    int rc = weft_chaos_run_stepped(&cfg, &v);
    char* json = (char*)malloc(4096);
    if (weft_chaos_verdict_json(&cfg, &v, json, 4096) < 0) {
        fprintf(stderr, "chaos-runner: verdict JSON overflow\n");
        free(json);
        return 2;
    }
    printf("%s\n", json);
    if (write_json(out, json) != 0) { free(json); return 2; }
    free(json);
    fprintf(stderr, "chaos-stepped: %s (steps_executed=%llu, injections p/s/t/r = %llu/%llu/%llu/%llu)\n",
            v.pass ? "PASS" : "FAIL",
            (unsigned long long)v.ledger.steps_executed,
            (unsigned long long)v.ledger.injected[0],
            (unsigned long long)v.ledger.injected[1],
            (unsigned long long)v.ledger.injected[2],
            (unsigned long long)v.ledger.injected[3]);
    return rc;
}

static int run_free(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "free <frames> <slots> <words> <readers> <chaosRate> <seed> [out.json]\n");
        return 2;
    }
    weft_chaos_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.steps      = 0; // free mode has no scheduler budget
    cfg.frames     = (uint32_t)strtoul(argv[2], NULL, 10);
    cfg.slots      = (uint32_t)strtoul(argv[3], NULL, 10);
    cfg.words      = (uint32_t)strtoul(argv[4], NULL, 10);
    cfg.readers    = (uint32_t)strtoul(argv[5], NULL, 10);
    cfg.chaos_rate = (uint32_t)strtoul(argv[6], NULL, 10);
    cfg.seed       = (uint32_t)strtoul(argv[7], NULL, 10);
    const char* out = argc > 8 ? argv[8] : NULL;

    weft_chaos_verdict_t v;
    int rc = weft_chaos_run_free(&cfg, &v);
    char* json = (char*)malloc(4096);
    if (weft_chaos_verdict_json(&cfg, &v, json, 4096) < 0) {
        fprintf(stderr, "chaos-runner: verdict JSON overflow\n");
        free(json);
        return 2;
    }
    printf("%s\n", json);
    if (write_json(out, json) != 0) { free(json); return 2; }
    free(json);
    fprintf(stderr, "chaos-free: %s (%.2fs, injections p/s/t/r = %llu/%llu/%llu/%llu)\n",
            v.pass ? "PASS" : "FAIL",
            (double)v.elapsed_ns / 1e9,
            (unsigned long long)v.ledger.injected[0],
            (unsigned long long)v.ledger.injected[1],
            (unsigned long long)v.ledger.injected[2],
            (unsigned long long)v.ledger.injected[3]);
    return rc;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "weft chaos runner (RFC 0011) — usage:\n"
                "  stepped <steps> <slots> <words> <readers> <frames> <chaosRate> <seed> [out.json]\n"
                "  free <frames> <slots> <words> <readers> <chaosRate> <seed> [out.json]\n"
                "  selftest\n");
        return 2;
    }
    if (strcmp(argv[1], "stepped") == 0) return run_stepped(argc, argv);
    if (strcmp(argv[1], "free") == 0)    return run_free(argc, argv);
    if (strcmp(argv[1], "selftest") == 0) {
        int rc = weft_chaos_selftest();
        fprintf(stderr, "chaos-selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
        return rc;
    }
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
