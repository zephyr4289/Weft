// test_spectrum_registry.c — registry rules + admission gates (Pillar 5, D-52).
//
// PROVES: builtin table shape; runtime registration contract (EINVAL/EABI/
// ESTATE post-create); builtin permanence; the WEFT_VENDOR_MOCK admission
// gate (WEFT_CTX_FLAG_ALLOW_MOCK); engine-mask exclusion fail-closed; and
// the heap-lock lifecycle across multiple contexts.

#include "../../../core/c/spectrum/drivers/weft_backend.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

// Builtin driver symbols (declared here for the frozen-order assertion;
// backend_registry.c owns the authoritative table)
extern const weft_backend_ops_t weft_driver_qualcomm_ops;
extern const weft_backend_ops_t weft_cpu_simd_ops;

// ---------------------------------------------------------------------------
// A fake runtime backend (Engineer-3 style SDK fallback) for the seam tests
// ---------------------------------------------------------------------------

static weft_backend_status_t fake_probe(weft_backend_caps_t* caps) {
    memset(caps, 0, sizeof(*caps));
    caps->abi_version = WEFT_BACKEND_ABI_VERSION;
    caps->vendor_id = WEFT_VENDOR_MOCK;
    caps->engine_class = WEFT_ENGINE_GPU;
    caps->op_affinity = WEFT_OP_AFFINITY(WEFT_OP_NORMALIZE_F32);
    caps->score = 600;
    strncpy(caps->impl_name, "fake-sdk", sizeof(caps->impl_name) - 1);
    return WEFT_BACKEND_OK;
}
static weft_backend_status_t fake_init(void* self,
                                       const weft_backend_init_cfg_t* cfg) {
    (void)self;
    (void)cfg;
    return WEFT_BACKEND_OK;
}
static weft_backend_status_t fake_execute(void* self, const weft_op_desc_t* op,
                                          weft_dispatch_result_t* out) {
    (void)self;
    (void)op;
    (void)out;
    return WEFT_BACKEND_OK;   // never actually dispatched in this battery
}
static weft_backend_status_t fake_submit(void* self, const weft_op_desc_t* ops,
                                        uint32_t count) {
    (void)self; (void)ops; (void)count;
    return WEFT_BACKEND_OK;
}
static weft_backend_status_t fake_sync(void* self, uint64_t s, uint64_t t) {
    (void)self; (void)s; (void)t;
    return WEFT_BACKEND_OK;
}
static void fake_shutdown(void* self) { (void)self; }

static const weft_backend_ops_t g_fake_ops = {
    .name = "fake-sdk-backend",
    .abi_version = WEFT_BACKEND_ABI_VERSION,
    .vendor_id = WEFT_VENDOR_MOCK,
    .engine_class = WEFT_ENGINE_GPU,
    .state_bytes = 64,
    .reserved0 = 0,
    .probe = fake_probe,
    .init = fake_init,
    .execute = fake_execute,
    .submit = fake_submit,
    .sync = fake_sync,
    .shutdown = fake_shutdown,
};

static void test_registry_rules(void) {
    uint32_t count = 0;
    const weft_backend_ops_t* const* ops = weft_backend_registry_ops(&count);
    CHECK(ops != NULL && count == 6, "6 builtin backends, got %u", count);
    CHECK(ops[0] == &weft_driver_qualcomm_ops && ops[5] == &weft_cpu_simd_ops,
          "frozen builtin order (qualcomm first, cpu terminal)");

    CHECK(weft_backend_register(NULL) == WEFT_BACKEND_EINVAL, "NULL register");
    CHECK(weft_backend_unregister(NULL) == WEFT_BACKEND_EINVAL, "NULL unregister");
    CHECK(weft_backend_unregister(&g_fake_ops) == WEFT_BACKEND_EINVAL,
          "unregister unknown");

    // ABI mismatch: fail-closed.
    weft_backend_ops_t bad_abi = g_fake_ops;
    bad_abi.abi_version = 99;
    CHECK(weft_backend_register(&bad_abi) == WEFT_BACKEND_EABI, "ABI mismatch");

    // Builtin permanence.
    CHECK(weft_backend_unregister(&weft_driver_qualcomm_ops) ==
          WEFT_BACKEND_ESTATE, "builtins are permanent");

    // Valid registration; duplicates are fail-closed (ESTATE), then
    // idempotent unregister rules.
    CHECK(weft_backend_register(&g_fake_ops) == WEFT_BACKEND_OK, "register");
    CHECK(weft_backend_register(&g_fake_ops) == WEFT_BACKEND_ESTATE,
          "duplicate registration refused (dedup, fail-closed)");
    CHECK(weft_backend_unregister(&g_fake_ops) == WEFT_BACKEND_OK, "unregister");
    CHECK(weft_backend_unregister(&g_fake_ops) == WEFT_BACKEND_EINVAL,
          "double unregister");

    // Live context: tables immutable (frozen header contract).
    weft_backend_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(&cfg);
    CHECK(ctx != NULL, "ctx");
    CHECK(weft_backend_register(&g_fake_ops) == WEFT_BACKEND_ESTATE,
          "post-create registration refused");
    CHECK(weft_backend_heap_locked() == 1, "heap lock engaged");
    weft_backend_ctx_destroy(ctx);
    CHECK(weft_backend_register(&g_fake_ops) == WEFT_BACKEND_OK,
          "registration legal again after destroy");
    CHECK(weft_backend_unregister(&g_fake_ops) == WEFT_BACKEND_OK, "cleanup");
}

static void test_mock_admission(void) {
    CHECK(weft_backend_register(&g_fake_ops) == WEFT_BACKEND_OK, "register mock");

    // Without ALLOW_MOCK: the mock-vendor entry is refused at admission.
    weft_backend_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(&cfg);
    weft_backend_info_t info[WEFT_BACKEND_MAX_BACKENDS];
    uint32_t n = weft_backend_table_info(ctx, info, WEFT_BACKEND_MAX_BACKENDS);
    int found_dead = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (info[i].vendor_id == WEFT_VENDOR_MOCK) {
            found_dead = 1;
            CHECK(info[i].state == 0, "mock refused without the flag");
        }
    }
    CHECK(found_dead, "mock entry visible (honestly dead)");
    weft_backend_ctx_destroy(ctx);

    // With ALLOW_MOCK: admitted.
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = WEFT_CTX_FLAG_ALLOW_MOCK;
    ctx = weft_backend_ctx_create(&cfg);
    n = weft_backend_table_info(ctx, info, WEFT_BACKEND_MAX_BACKENDS);
    int found_live = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (info[i].vendor_id == WEFT_VENDOR_MOCK) {
            found_live = 1;
            CHECK(info[i].state == 1, "mock admitted with the flag");
        }
    }
    CHECK(found_live, "mock entry live with ALLOW_MOCK");
    weft_backend_ctx_destroy(ctx);
    CHECK(weft_backend_unregister(&g_fake_ops) == WEFT_BACKEND_OK, "cleanup");
}

static void test_engine_mask(void) {
    // NPU-only mask: the terminal CPU engine is EXCLUDED — a checksum
    // dispatch must fail closed (no executor), never silently run wide.
    weft_backend_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.engine_mask = WEFT_ENGINE_MASK_NPU;
    weft_backend_ctx_t* ctx = weft_backend_ctx_create(&cfg);
    CHECK(ctx != NULL, "ctx (NPU mask)");
    weft_backend_info_t info[WEFT_BACKEND_MAX_BACKENDS];
    const uint32_t n = weft_backend_table_info(ctx, info, WEFT_BACKEND_MAX_BACKENDS);
    for (uint32_t i = 0; i < n; i++) {
        if (info[i].state == 1) {
            CHECK(info[i].engine_class == WEFT_ENGINE_NPU,
                  "only NPU rows admitted live (got class %u)",
                  info[i].engine_class);
        }
    }
    static unsigned char buf[64] __attribute__((aligned(64)));
    weft_op_desc_t op;
    memset(&op, 0, sizeof(op));
    op.kind = WEFT_OP_SEQLOCK_CHECKSUM;
    op.u0 = 3;
    op.bufs[0].data = buf;
    op.bufs[0].bytes = 64;
    op.bufs[0].dtype = WEFT_BACKEND_DTYPE_U32;
    weft_dispatch_result_t res;
    memset(&res, 0, sizeof(res));
    const weft_backend_status_t st = weft_backend_dispatch(ctx, &op, &res);
    CHECK(st == WEFT_BACKEND_ESTATE || st == WEFT_BACKEND_EREFUSED,
          "checksum with NPU-only mask fails closed (got %d)", (int)st);
    weft_backend_ctx_destroy(ctx);

    // Two live contexts: heap lock spans both lifetimes.
    weft_backend_ctx_t* a = weft_backend_ctx_create(NULL);
    weft_backend_ctx_t* b = weft_backend_ctx_create(NULL);
    CHECK(a != NULL && b != NULL, "two ctxs");
    CHECK(weft_backend_heap_locked() == 1, "lock held across both");
    weft_backend_ctx_destroy(a);
    CHECK(weft_backend_heap_locked() == 1, "lock still held (b alive)");
    weft_backend_ctx_destroy(b);
    CHECK(weft_backend_heap_locked() == 0, "lock released after last");
}

int main(void) {
    test_registry_rules();
    test_mock_admission();
    test_engine_mask();
    if (g_failures != 0) {
        printf("spectrum-registry: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("spectrum-registry: admission+registry rules PASS\n");
    return 0;
}
