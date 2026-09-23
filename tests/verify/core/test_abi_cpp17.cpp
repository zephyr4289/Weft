/* ---------------------------------------------------------------------------
 * test_abi_cpp17.cpp — Weft Pillar 8: ABI freeze & C++17 compatibility
 *
 * GATE COVERAGE: G6 (mandate §2.D.4): "ABI freeze & C++17 compatibility".
 *
 * This translation unit includes BOTH frozen headers from C++17 under
 * extern "C", mirrors every layout static assert the C side pins, links
 * against the C objects and calls the exported symbols through the C++
 * compiler's ABI view. Any drift between the two compilers' views of the
 * contract fails this test at compile time (static asserts) or run time
 * (value checks).
 * ------------------------------------------------------------------------- */

#include "weft_verify.h"
#include "weftc_lint_alloc.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>

/* ------------------------------------------------------------------ */
/* mirrored frozen-layout proofs (C++17 side of the contract)          */
/* ------------------------------------------------------------------ */

static_assert(sizeof(weft_verify_status_t) == 4,
              "frozen ABI (C++17 mirror): weft_verify_status_t must be 4 "
              "bytes");
static_assert(WEFT_VERIFY_ABI_VERSION == 0x0100,
              "frozen ABI (C++17 mirror): version packing must be 0x0100");
static_assert(WEFT_VERIFY_OK == 0,
              "frozen ABI (C++17 mirror): WEFT_VERIFY_OK must be 0");

static_assert(sizeof(weft_lint_node_t) == 24,
              "frozen ABI (C++17 mirror): weft_lint_node_t must be 24 bytes");
static_assert(sizeof(weft_lint_diag_t) == 444,
              "frozen ABI (C++17 mirror): weft_lint_diag_t must be 444 bytes");
static_assert(sizeof(weft_lint_func_t) == 32,
              "frozen ABI (C++17 mirror): weft_lint_func_t must be 32 bytes");

static_assert(offsetof(weft_lint_diag_t, file) == 0, "frozen diag layout");
static_assert(offsetof(weft_lint_diag_t, line) == 64, "frozen diag layout");
static_assert(offsetof(weft_lint_diag_t, col) == 68, "frozen diag layout");
static_assert(offsetof(weft_lint_diag_t, severity) == 72,
              "frozen diag layout");
static_assert(offsetof(weft_lint_diag_t, rule) == 73, "frozen diag layout");
static_assert(offsetof(weft_lint_diag_t, message) == 89,
              "frozen diag layout");
static_assert(offsetof(weft_lint_diag_t, remediation) == 249,
              "frozen diag layout");

static_assert(sizeof(((weft_lint_node_t *)0)->kind) == 4,
              "frozen node field size");
static_assert(WEFT_LINT_TK_IDENT == 1, "frozen token kinds");
static_assert(WEFT_LINT_TK_STRING == 3, "frozen token kinds");
static_assert(WEFT_LINT_TK_PREPROC == 7, "frozen token kinds");
static_assert(WEFT_LINT_LANG_RUST == 3, "frozen lang ids");
static_assert(WEFT_LINT_LANG_DART == 6, "frozen lang ids");
static_assert(WEFT_LINT_ERROR == 2, "frozen severities");
static_assert(WEFTC_LINT_ABI_VERSION == 0x0100,
              "frozen lint ABI version");

/* the C-side layout asserts live in weft_verify.h / weftc_lint_alloc.h
 * and weft_verify_bounds.c; the C++ TU re-derives them independently. */

int main()
{
    int failures = 0;

    /* ---- verify ABI surface ---- */
    if (weft_verify_abi_version() != 0x0100u) {
        std::fprintf(stderr, "FAIL: verify abi version 0x%x\n",
                     (unsigned)weft_verify_abi_version());
        failures++;
    }
    if (weft_verify_abi_selfcheck() != WEFT_VERIFY_OK) {
        std::fprintf(stderr, "FAIL: verify selfcheck\n");
        failures++;
    }
    if (weft_verify_ring_index(7, 8) != WEFT_VERIFY_OK ||
        weft_verify_ring_index(8, 8) != WEFT_VERIFY_EBOUNDS) {
        std::fprintf(stderr, "FAIL: ring_index twins\n");
        failures++;
    }
    if (weft_verify_ring_span(4, 8, 8) != WEFT_VERIFY_EOVERFLOW ||
        weft_verify_vlf_offset(60, 8, 64) != WEFT_VERIFY_EOVERFLOW ||
        weft_verify_dma_stride(32, 64, 10, 640) != WEFT_VERIFY_ESTRIDE ||
        weft_verify_alignment(65, 64) != WEFT_VERIFY_EMISALIGN) {
        std::fprintf(stderr, "FAIL: bounds twins\n");
        failures++;
    }
    if (weft_verify_violation_count() != 0u) {
        std::fprintf(stderr, "FAIL: violation ledger not clean at start\n");
        failures++;
    }

    /* ---- lint ABI surface ---- */
    if (weftc_lint_abi_version() != 0x0100u) {
        std::fprintf(stderr, "FAIL: lint abi version 0x%x\n",
                     (unsigned)weftc_lint_abi_version());
        failures++;
    }
    if (std::strstr(weftc_lint_version(), "weftc-lint 1.0.0") == nullptr) {
        std::fprintf(stderr, "FAIL: lint version string '%s'\n",
                     weftc_lint_version());
        failures++;
    }

    /* ---- struct round-trip through the C++ view ---- */
    weft_lint_diag_t d;
    std::memset(&d, 0, sizeof(d));
    std::memcpy(d.file, "corpus.cpp", 10);
    d.line = 12;
    d.col = 34;
    d.severity = WEFT_LINT_ERROR;
    std::memcpy(d.rule, "alloc-new", 9);
    if (d.line != 12u || d.col != 34u || d.severity != 2u ||
        d.file[10] != '\0' || d.rule[9] != '\0') {
        std::fprintf(stderr, "FAIL: diag struct round-trip\n");
        failures++;
    }

    std::printf("ABI-CPP17: sizeof(diag)=%zu sizeof(node)=%zu "
                "sizeof(func)=%zu verify_abi=0x%04x lint_abi=0x%04x\n",
                sizeof(weft_lint_diag_t), sizeof(weft_lint_node_t),
                sizeof(weft_lint_func_t), (unsigned)weft_verify_abi_version(),
                (unsigned)weftc_lint_abi_version());
    std::printf("ABI-CPP17 %s (%d failures)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
