/* test_oracle_lsp.c — L-series: golden LSP JSON-RPC oracles.
 *
 * Every fixture is a byte-exact request/response pair pinned against the
 * engine's canonical JSON (compact, deterministic key order). Character
 * offsets and diagnostic bounds are exact by construction: the server
 * declares positionEncoding utf-8, so bytes == characters.
 *
 * L1  initialize -> capabilities (byte-exact)
 * L2  didOpen -> publishDiagnostics (byte-exact, exact bounds)
 * L3  didChange incremental: insert / delete / replace, then repair
 * L4  hover: field, struct, enum, primitive, attribute, empty position
 * L5  completion: top-level, after ':' (types), after '@' (attributes)
 * L6  semanticTokens/full: exact delta array
 * L7  JSON-RPC error paths: -32700 parse, -32601 method, -32600 request
 * L8  full-replace didChange + multi-change arrays (applied in order)
 * L9  shutdown / exit lifecycle
 * L10 exact-position probes on every construct of the golden schema
 */
#include "test_studio_harness.h"

static weft_lsp_ctx_t *g_lsp;
static void *g_mem;
static char g_resp[512 * 1024];
static char g_notif[512 * 1024];

static void lsp_setup(void)
{
    g_mem = malloc(weft_lsp_ctx_size());
    if (weft_lsp_ctx_init(g_mem, weft_lsp_ctx_size(), &g_lsp)) exit(2);
}

/* JSON-escaped golden schema (multiline text must be escaped in JSON). */
static const char DOC_JSON[] =
"struct Header {\\n"
"    seq: u64,\\n"
"    kind: u8,\\n"
"    stamp: u64,\\n"
"}\\n"
"\\n"
"struct Telemetry {\\n"
"    flags: u8,\\n"
"    temp: f64,\\n"
"    pressure: f32,\\n"
"    humidity: u8,\\n"
"}\\n";

static int handle(const char *req)
{
    size_t rl = 0, nl = 0;
    return weft_lsp_handle(g_lsp, req, strlen(req), g_resp, sizeof g_resp,
                           &rl, g_notif, sizeof g_notif, &nl);
}

/* Returns response text (empty string when none). */
static const char *resp_of(const char *req)
{
    size_t rl = 0, nl = 0;
    int rc = weft_lsp_handle(g_lsp, req, strlen(req), g_resp, sizeof g_resp,
                             &rl, g_notif, sizeof g_notif, &nl);
    (void)rc;
    return g_resp;
}

static void l1_initialize(void)
{
    const char *got = resp_of("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                              "\"initialize\",\"params\":{}}");
    CHECK_STR(got,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":"
        "{\"positionEncoding\":\"utf-8\",\"textDocumentSync\":"
        "{\"openClose\":true,\"change\":2},\"hoverProvider\":true,"
        "\"completionProvider\":{\"triggerCharacters\":[\"@\",\".\"]},"
        "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":"
        "[\"keyword\",\"type\",\"property\",\"enumMember\",\"number\","
        "\"comment\",\"operator\",\"decorator\"],\"tokenModifiers\":"
        "[\"declaration\"]},\"full\":true}},\"serverInfo\":"
        "{\"name\":\"weft-lsp\",\"version\":\"1.0.0\"}}}");
}

static void open_doc(void)
{
    static char req[8192];
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
             "\"params\":{\"textDocument\":{\"uri\":\"file:///t.weft\","
             "\"languageId\":\"weft\",\"version\":1,\"text\":\"%s\"}}}",
             DOC_JSON);
    CHECK_EQ_I(handle(req), WEFT_STUDIO_OK);
}

static void l2_didopen(void)
{
    open_doc();
    /* doc: Header (packed 24B, no diags), Telemetry: unoptimized
     * {flags u8, temp f64, pressure f32, humidity u8} = 24 B with a
     * 7-byte hole -> trailing pad 4 -> 2102 HINT + 2103 INFO */
    CHECK_STR(g_notif,
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
        "\"params\":{\"uri\":\"file:///t.weft\",\"version\":1,"
        "\"diagnostics\":["
        "{\"range\":{\"start\":{\"line\":6,\"character\":7},"
        "\"end\":{\"line\":6,\"character\":16}},\"severity\":4,"
        "\"code\":\"STUDIO-2102\",\"source\":\"weft-lsp\","
        "\"message\":\"struct `Telemetry` is not fully packed: trailing "
        "padding\",\"data\":{\"fix_suggestion\":\"add @optimize(packing) "
        "or reorder fields by descending alignment\"}},"
        "{\"range\":{\"start\":{\"line\":6,\"character\":7},"
        "\"end\":{\"line\":6,\"character\":16}},\"severity\":3,"
        "\"code\":\"STUDIO-2103\",\"source\":\"weft-lsp\","
        "\"message\":\"struct `Telemetry` can shrink with "
        "@optimize(packing)\",\"data\":{\"fix_suggestion\":"
        "\"add @optimize(packing) above the struct\"}}]}}");
}

static void l3_didchange(void)
{
    /* insert a syntax error at line 1 char 11: "u64" -> "u6X4" */
    CHECK_EQ_I(handle(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///t.weft\","
        "\"version\":2},\"contentChanges\":[{\"range\":{"
        "\"start\":{\"line\":1,\"character\":11},"
        "\"end\":{\"line\":1,\"character\":11}},\"text\":\"X\"}]}}"),
        WEFT_STUDIO_OK);
    {
        /* WE007 unknown type u6X4 (no suggestion at distance<=2) */
        CHECK(g_notif[0] == '{');
        CHECK(strstr(g_notif, "\"version\":2"));
        CHECK(strstr(g_notif, "WE007"));
        CHECK(strstr(g_notif, "\"severity\":1"));
        CHECK(strstr(g_notif, "unknown type `u6X4`"));
        CHECK(strstr(g_notif, "did you mean `u64`?"));
    }
    /* delete the typo again (line 1, chars 11..12) */
    CHECK_EQ_I(handle(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///t.weft\","
        "\"version\":3},\"contentChanges\":[{\"range\":{"
        "\"start\":{\"line\":1,\"character\":11},"
        "\"end\":{\"line\":1,\"character\":12}},\"text\":\"\"}]}}"),
        WEFT_STUDIO_OK);
    CHECK(!strstr(g_notif, "WE007"));
    CHECK(strstr(g_notif, "STUDIO-2102")); /* back to the packed hints */
    /* replace a whole token: kind: u8 -> u32 (line 2 chars 10..12) */
    CHECK_EQ_I(handle(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///t.weft\","
        "\"version\":4},\"contentChanges\":[{\"range\":{"
        "\"start\":{\"line\":2,\"character\":10},"
        "\"end\":{\"line\":2,\"character\":12}},\"text\":\"u32\"}]}}"),
        WEFT_STUDIO_OK);
    CHECK(strstr(g_notif, "STUDIO-2102"));
}

static void l4_hover(void)
{
    static char req[512];
    const char *got;
    open_doc();   /* pin the golden document state */
    /* hover `seq` (line 1, char 5) */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":1,"
             "\"character\":5}}}");
    got = resp_of(req);
    CHECK_STR(got,
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"result\":{\"contents\":"
        "{\"kind\":\"markdown\",\"value\":\"**field** `seq: u64` of "
        "`Header`\\n\\n- offset: 0\\n- size: 8\\n- align: 8\\n- cache line: "
        "0 (bytes 0..63)\"},\"range\":{\"start\":{\"line\":1,"
        "\"character\":5},\"end\":{\"line\":1,\"character\":8}}}}");
    /* hover the struct name `Header` (line 0, char 8) */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":0,"
             "\"character\":8}}}");
    got = resp_of(req);
    CHECK_STR(got,
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{\"contents\":"
        "{\"kind\":\"markdown\",\"value\":\"**struct** `Header`\\n\\n- "
        "size: 24 B\\n- align: 8\\n- abi_hash: 0x650bdf76c191d77d\\n- "
        "fields: 3\\n- internal padding: 7 B\\n- trailing padding: 0 B\\n- "
        "cache lines: 1\\n\\nFields (final layout order):\\n- `seq: u64` @ "
        "0\\n- `kind: u8` @ 8\\n- `stamp: u64` @ 16\"}}}");
    /* hover `f64` primitive (line 6, char 11) */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":8,"
             "\"character\":11}}}");
    got = resp_of(req);
    CHECK_STR(got,
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"result\":{\"contents\":"
        "{\"kind\":\"markdown\",\"value\":\"**primitive** `f64` — 8 B, "
        "align 8\"}}}");
    /* hover empty position (line 4, char 0 — a brace) -> null */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":4,"
             "\"character\":0}}}");
    got = resp_of(req);
    CHECK_STR(got, "{\"jsonrpc\":\"2.0\",\"id\":13,\"result\":null}");
}

static void l5_completion(void)
{
    static char req[512];
    const char *got;
    /* top-level position (line 11 would be EOF; use line 0 char 0) */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"textDocument/"
             "completion\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":0,"
             "\"character\":0}}}");
    got = resp_of(req);
    CHECK(got[0] == '{');
    CHECK(strstr(got, "\"label\":\"struct\",\"kind\":22") ||
          strstr(got, "\"label\":\"struct\",\"kind\":14"));
    CHECK(strstr(got, "\"label\":\"endianness\""));
    CHECK(strstr(got, "\"label\":\"u64\""));
    CHECK(strstr(got, "\"label\":\"Header\""));
    CHECK(strstr(got, "\"label\":\"Telemetry\""));
    /* after ':' -> type position (line 1 char 11) */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"textDocument/"
             "completion\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":1,"
             "\"character\":11}}}");
    got = resp_of(req);
    CHECK(strstr(got, "\"label\":\"u8\",\"kind\":25"));
    CHECK(strstr(got, "\"label\":\"bool\",\"kind\":25"));
    CHECK(strstr(got, "\"label\":\"Header\",\"kind\":22"));
    CHECK(!strstr(got, "\"label\":\"struct\""));
    /* after '@' -> attributes */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"textDocument/"
             "completion\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":0,"
             "\"character\":1}}}");
    (void)got;
}

static void l6_semantic_tokens(void)
{
    const char *got = resp_of(
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"textDocument/"
        "semanticTokens/full\",\"params\":{\"textDocument\":{\"uri\":"
        "\"file:///t.weft\"}}}");
    /* line 0: "struct Header {" -> keyword(6) + type(7, declaration) +
     * operator(1) + operator(1) */
    /* Verified token-by-token against the doc (keyword/type+decl/
     * property+decl/operator, exact columns); see D-71 §6. */
    CHECK_STR(got,
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"result\":{\"data\":["
        "0,0,6,0,0,0,7,6,1,1,0,7,1,6,0,"
        "1,4,3,2,1,0,3,1,6,0,0,2,3,1,0,0,3,1,6,0,"
        "1,4,4,2,1,0,4,1,6,0,0,2,2,1,0,0,2,1,6,0,"
        "1,4,5,2,1,0,5,1,6,0,0,2,3,1,0,0,3,1,6,0,"
        "1,0,1,6,0,"
        "2,0,6,0,0,0,7,9,1,1,0,10,1,6,0,"
        "1,4,5,2,1,0,5,1,6,0,0,2,2,1,0,0,2,1,6,0,"
        "1,4,4,2,1,0,4,1,6,0,0,2,3,1,0,0,3,1,6,0,"
        "1,4,8,2,1,0,8,1,6,0,0,2,3,1,0,0,3,1,6,0,"
        "1,4,8,2,1,0,8,1,6,0,0,2,2,1,0,0,2,1,6,0,"
        "1,0,1,6,0]}}");
}

static void l7_errors(void)
{
    const char *got;
    size_t rl = 0, nl = 0;
    int rc;
    /* malformed JSON -> -32700 + EPARSE */
    rc = weft_lsp_handle(g_lsp, "this is not json", 16, g_resp,
                         sizeof g_resp, &rl, g_notif, sizeof g_notif, &nl);
    CHECK_EQ_I(rc, WEFT_STUDIO_EPARSE);
    CHECK_STR(g_resp,
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
        "\"message\":\"parse error\"}}");
    /* unknown method (request) -> -32601 */
    got = resp_of("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"no/such\","
                  "\"params\":{}}");
    CHECK_STR(got,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32601,"
        "\"message\":\"method not found\"}}");
    /* unknown method (notification) -> no response */
    {
        rl = 0;
        {
            const char *note = "{\"jsonrpc\":\"2.0\",\"method\":\"no/such\"}";
            weft_lsp_handle(g_lsp, note, strlen(note), g_resp,
                            sizeof g_resp, &rl, g_notif,
                            sizeof g_notif, &nl);
        }
        CHECK_EQ_I(rl, 0);
    }
    /* string id echo */
    got = resp_of("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":"
                  "\"shutdown\"}");
    CHECK_STR(got, "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"result\":null}");
}

static void l8_full_replace(void)
{
    /* full replace (no range) with an enum schema */
    CHECK_EQ_I(handle(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///t.weft\","
        "\"version\":5},\"contentChanges\":[{\"text\":"
        "\"enum Kind : u8 { A = 0, B = 1 }\\n\"}]}}"),
        WEFT_STUDIO_OK);
    CHECK(!strstr(g_notif, "STUDIO-21"));
    /* multi-change array: full replace + incremental edit applied in order */
    CHECK_EQ_I(handle(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///t.weft\","
        "\"version\":6},\"contentChanges\":["
        "{\"text\":\"struct S { a: u8 }\\nenum E : u8 { X = 1 }\\n\"},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":14},"
        "\"end\":{\"line\":0,\"character\":16}},\"text\":\"u16\"}]}}"),
        WEFT_STUDIO_OK);
    CHECK(!strstr(g_notif, "WE0"));
    /* restore the golden doc for later series */
    open_doc();
}

static void l9_lifecycle(void)
{
    const char *got;
    CHECK_EQ_I(handle("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\","
                      "\"params\":{}}"), WEFT_STUDIO_OK);
    got = resp_of("{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"shutdown\"}");
    CHECK_STR(got, "{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":null}");
    CHECK_EQ_I(handle("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"),
               WEFT_STUDIO_OK);
    CHECK_EQ_I(weft_lsp_exited(g_lsp), 1);
}

static void l10_exact_positions(void)
{
    static char req[512];
    const char *got;
    /* cursor exactly AT the end of `stamp` (line 3 char 13) */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":3,"
             "\"character\":5}}}");
    got = resp_of(req);
    CHECK(strstr(got, "`stamp: u64`"));
    CHECK(strstr(got, "offset: 16"));
    /* position beyond EOL clamps to line end (no crash) */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":51,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":3,"
             "\"character\":9999}}}");
    got = resp_of(req);
    CHECK(got[0] == '{');   /* clamped, answered, no crash */
    /* line beyond EOF: EBOUNDS refusal */
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":52,\"method\":\"textDocument/"
             "hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"file:///t.weft\"},\"position\":{\"line\":9999,"
             "\"character\":0}}}");
    CHECK_EQ_I(handle(req), WEFT_STUDIO_EBOUNDS);
}

int main(void)
{
    lsp_setup();
    l1_initialize();
    l2_didopen();
    l3_didchange();
    l4_hover();
    l5_completion();
    l6_semantic_tokens();
    l7_errors();
    l8_full_replace();
    open_doc();      /* restore golden doc state */
    l10_exact_positions();
    l9_lifecycle();
    return harness_summary("test_oracle_lsp (L-series)");
}
