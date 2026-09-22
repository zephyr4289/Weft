/* weft_lsp.c — the Weft Studio headless Language Server Protocol engine.
 *
 * JSON-RPC 2.0 over caller-provided memory buffers (Law 1): the engine
 * itself performs zero syscalls — hosts pump bytes through the read/write
 * callback seam (native stdio, wasm32 shims, or in-memory tests), so the
 * same code serves the desktop studio and the browser.
 *
 * Protocol surface:
 *   initialize / initialized / shutdown / exit
 *   textDocument/didOpen, didChange (incremental sync), didClose
 *   textDocument/publishDiagnostics  (server notification)
 *   textDocument/hover                (byte-exact layout tooltips)
 *   textDocument/completion           (keywords, primitives, decls, tags)
 *   textDocument/semanticTokens/full  (editor token classification)
 *
 * Position encoding: utf-8 (declared in capabilities) — byte offsets are
 * character offsets, exact by construction for the ASCII grammar.
 *
 * Document sync strategy: recompile-on-change (a full compile is ~tens of
 * microseconds — far below the 1 ms budget), which keeps didChange
 * incremental purely a text-editing concern and the engine stateless
 * between requests (no incremental AST patching to get wrong).
 */
#include "weft_studio_internal.h"

#define WEFT_LSP_MAX_DOC    (256u * 1024u)
#define WEFT_LSP_MAX_URI    1024u
#define WEFT_LSP_MAX_JNODES 8192u
#define WEFT_LSP_MAX_JSCRATCH (256u * 1024u)

/* ------------------------------------------------------------------ */
/* Minimal JSON DOM (in-place, bounded, zero allocation)               */
/* ------------------------------------------------------------------ */
enum {
    JV_NULL = 0, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ
};

typedef struct jv_t {
    uint8_t  kind;
    uint8_t  bool_v;
    uint16_t reserved;
    uint32_t key_off, key_len;      /* OBJ member key (decoded)           */
    uint32_t str_off, str_len;      /* decoded string (scratch pool)      */
    uint32_t num_i64;               /* number fits i64 flag               */
    uint32_t first_child, next_sibling;
    uint32_t raw_off, raw_len;      /* raw span in the request buffer     */
    uint64_t num;
} jv_t;

typedef struct {
    const char *base;
    size_t      len;
    uint32_t    pos;
    jv_t       *nodes;
    uint32_t    nnodes, cap;
    char       *scratch;
    uint32_t    scratch_n, scratch_cap;
    int         err;
} JParser;

static uint32_t jp_node(JParser *j)
{
    if (j->nnodes >= j->cap) { j->err = 1; return UINT32_MAX; }
    {
        jv_t *n = &j->nodes[j->nnodes];
        memset(n, 0, sizeof *n);
        n->first_child = UINT32_MAX;
        n->next_sibling = UINT32_MAX;
        return j->nnodes++;
    }
}

static void jp_ws(JParser *j)
{
    while (j->pos < j->len) {
        char c = j->base[j->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->pos++;
        else break;
    }
}

static int jp_hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void jp_scratch_utf8(JParser *j, uint32_t cp)
{
    if (j->scratch_n + 4u > j->scratch_cap) { j->err = 1; return; }
    if (cp < 0x80u) {
        j->scratch[j->scratch_n++] = (char)cp;
    } else if (cp < 0x800u) {
        j->scratch[j->scratch_n++] = (char)(0xC0u | (cp >> 6));
        j->scratch[j->scratch_n++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        j->scratch[j->scratch_n++] = (char)(0xE0u | (cp >> 12));
        j->scratch[j->scratch_n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        j->scratch[j->scratch_n++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        j->scratch[j->scratch_n++] = (char)(0xF0u | (cp >> 18));
        j->scratch[j->scratch_n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        j->scratch[j->scratch_n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        j->scratch[j->scratch_n++] = (char)(0x80u | (cp & 0x3Fu));
    }
}

/* Parses a JSON string at j->pos (must be '"'); decoded bytes land in the
 * scratch pool; returns node index or UINT32_MAX on error. */
static uint32_t jp_string(JParser *j)
{
    uint32_t node = jp_node(j);
    uint32_t start;
    if (node == UINT32_MAX) return UINT32_MAX;
    if (j->pos >= j->len || j->base[j->pos] != '"') { j->err = 1; return UINT32_MAX; }
    j->pos++;
    start = j->scratch_n;
    while (j->pos < j->len) {
        char c = j->base[j->pos];
        if (c == '"') {
            j->pos++;
            j->nodes[node].kind = JV_STR;
            j->nodes[node].str_off = start;
            j->nodes[node].str_len = j->scratch_n - start;
            return node;
        }
        if ((uint8_t)c < 0x20u) { j->err = 1; return UINT32_MAX; }
        if (c == '\\') {
            j->pos++;
            if (j->pos >= j->len) { j->err = 1; return UINT32_MAX; }
            {
                char e = j->base[j->pos];
                j->pos++;
                switch (e) {
                case '"': case '\\': case '/':
                    if (j->scratch_n < j->scratch_cap)
                        j->scratch[j->scratch_n++] = e;
                    break;
                case 'b': if (j->scratch_n < j->scratch_cap) j->scratch[j->scratch_n++] = '\b'; break;
                case 'f': if (j->scratch_n < j->scratch_cap) j->scratch[j->scratch_n++] = '\f'; break;
                case 'n': if (j->scratch_n < j->scratch_cap) j->scratch[j->scratch_n++] = '\n'; break;
                case 'r': if (j->scratch_n < j->scratch_cap) j->scratch[j->scratch_n++] = '\r'; break;
                case 't': if (j->scratch_n < j->scratch_cap) j->scratch[j->scratch_n++] = '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (j->pos + 4u > j->len || !jp_hex4(j->base + j->pos, &cp)) {
                        j->err = 1;
                        return UINT32_MAX;
                    }
                    j->pos += 4;
                    if (cp >= 0xD800u && cp <= 0xDBFFu &&
                        j->pos + 6u <= j->len &&
                        j->base[j->pos] == '\\' && j->base[j->pos + 1u] == 'u') {
                        uint32_t lo;
                        if (jp_hex4(j->base + j->pos + 2u, &lo) &&
                            lo >= 0xDC00u && lo <= 0xDFFFu) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            j->pos += 6;
                        }
                    }
                    jp_scratch_utf8(j, cp);
                    break;
                }
                default:
                    j->err = 1;
                    return UINT32_MAX;
                }
            }
            continue;
        }
        if (j->scratch_n < j->scratch_cap)
            j->scratch[j->scratch_n++] = c;
        else
            j->err = 1;
        j->pos++;
    }
    j->err = 1;
    return UINT32_MAX;
}

static uint32_t jp_value(JParser *j, uint32_t depth);

static uint32_t jp_value_wrapped(JParser *j, uint32_t depth)
{
    uint32_t start = j->pos;
    uint32_t n = jp_value(j, depth);
    if (n != UINT32_MAX) {
        j->nodes[n].raw_off = start;
        j->nodes[n].raw_len = j->pos - start;
    }
    return n;
}

static uint32_t jp_container(JParser *j, uint32_t depth, int is_obj)
{
    uint32_t node = jp_node(j);
    uint32_t prev = UINT32_MAX;
    char open = is_obj ? '{' : '[';
    char close = is_obj ? '}' : ']';
    if (node == UINT32_MAX) return UINT32_MAX;
    j->nodes[node].kind = is_obj ? JV_OBJ : JV_ARR;
    j->nodes[node].first_child = UINT32_MAX;
    if (j->pos >= j->len || j->base[j->pos] != open) { j->err = 1; return UINT32_MAX; }
    j->pos++;
    jp_ws(j);
    if (j->pos < j->len && j->base[j->pos] == close) {
        j->pos++;
        return node;
    }
    if (depth > 32) { j->err = 1; return UINT32_MAX; }
    for (;;) {
        uint32_t child;
        if (is_obj) {
            uint32_t key;
            jp_ws(j);
            key = jp_string(j);
            if (key == UINT32_MAX) return UINT32_MAX;
            jp_ws(j);
            if (j->pos >= j->len || j->base[j->pos] != ':') { j->err = 1; return UINT32_MAX; }
            j->pos++;
            jp_ws(j);
            child = jp_value_wrapped(j, depth + 1u);
            if (child == UINT32_MAX) return UINT32_MAX;
            /* member chain: key node (carries the key text) whose
             * next_sibling is the value node; pairs walk two-at-a-time */
            j->nodes[key].key_off = j->nodes[key].str_off;
            j->nodes[key].key_len = j->nodes[key].str_len;
            if (prev == UINT32_MAX) j->nodes[node].first_child = key;
            else j->nodes[prev].next_sibling = key;
            j->nodes[key].next_sibling = child;
            prev = child;
        } else {
            jp_ws(j);
            child = jp_value_wrapped(j, depth + 1u);
            if (child == UINT32_MAX) return UINT32_MAX;
            if (prev == UINT32_MAX) j->nodes[node].first_child = child;
            else j->nodes[prev].next_sibling = child;
            prev = child;
        }
        jp_ws(j);
        if (j->pos >= j->len) { j->err = 1; return UINT32_MAX; }
        if (j->base[j->pos] == ',') { j->pos++; continue; }
        if (j->base[j->pos] == close) { j->pos++; return node; }
        j->err = 1;
        return UINT32_MAX;
    }
}

static uint32_t jp_value(JParser *j, uint32_t depth)
{
    jp_ws(j);
    if (j->pos >= j->len) { j->err = 1; return UINT32_MAX; }
    {
        char c = j->base[j->pos];
        if (c == '{') return jp_container(j, depth, 1);
        if (c == '[') return jp_container(j, depth, 0);
        if (c == '"') return jp_string(j);
        if (c == 't') {
            if (j->pos + 4u <= j->len && memcmp(j->base + j->pos, "true", 4) == 0) {
                uint32_t n = jp_node(j);
                if (n == UINT32_MAX) return UINT32_MAX;
                j->nodes[n].kind = JV_BOOL;
                j->nodes[n].bool_v = 1;
                j->pos += 4;
                return n;
            }
            j->err = 1;
            return UINT32_MAX;
        }
        if (c == 'f') {
            if (j->pos + 5u <= j->len && memcmp(j->base + j->pos, "false", 5) == 0) {
                uint32_t n = jp_node(j);
                if (n == UINT32_MAX) return UINT32_MAX;
                j->nodes[n].kind = JV_BOOL;
                j->pos += 5;
                return n;
            }
            j->err = 1;
            return UINT32_MAX;
        }
        if (c == 'n') {
            if (j->pos + 4u <= j->len && memcmp(j->base + j->pos, "null", 4) == 0) {
                uint32_t n = jp_node(j);
                if (n == UINT32_MAX) return UINT32_MAX;
                j->nodes[n].kind = JV_NULL;
                j->pos += 4;
                return n;
            }
            j->err = 1;
            return UINT32_MAX;
        }
        /* number */
        {
            uint32_t n = jp_node(j);
            uint32_t start = j->pos;
            uint64_t num = 0;
            int neg = 0;
            if (n == UINT32_MAX) return UINT32_MAX;
            if (c == '-') { neg = 1; j->pos++; }
            if (j->pos >= j->len || j->base[j->pos] < '0' || j->base[j->pos] > '9') {
                j->err = 1;
                return UINT32_MAX;
            }
            while (j->pos < j->len) {
                char d = j->base[j->pos];
                if (d >= '0' && d <= '9') {
                    num = num * 10u + (uint64_t)(d - '0');
                    j->pos++;
                } else if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
                    j->pos++; /* accepted, ignored (no float params in LSP use) */
                } else {
                    break;
                }
            }
            j->nodes[n].kind = JV_NUM;
            j->nodes[n].num = neg ? (uint64_t)(-(int64_t)num) : num;
            j->nodes[n].num_i64 = 1;
            (void)start;
            return n;
        }
    }
}

/* ------------------------------------------------------------------ */
/* JSON writer                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *buf;
    size_t cap, n;
    int    of;
} JW;

static void jw_mem(JW *w, const char *s, size_t len)
{
    if ((uint64_t)w->n + len > w->cap) { w->of = 1; return; }
    if (len) memcpy(w->buf + w->n, s, len);
    w->n += len;
}
static void jw_str(JW *w, const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    jw_mem(w, s, len);
}
static void jw_ch(JW *w, char c) { jw_mem(w, &c, 1); }
static void jw_u64(JW *w, uint64_t v)
{
    char tmp[24];
    uint32_t i = 0, j;
    if (v == 0) { jw_ch(w, '0'); return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    for (j = i; j > 0; j--) jw_ch(w, tmp[j - 1]);
}
static void jw_hex64(JW *w, uint64_t v)
{
    static const char hexd[] = "0123456789abcdef";
    char tmp[16];
    uint32_t i = 0, j;
    jw_str(w, "0x");
    if (v == 0) { jw_ch(w, '0'); return; }
    while (v) { tmp[i++] = hexd[v & 0xF]; v >>= 4; }
    for (j = i; j > 0; j--) jw_ch(w, tmp[j - 1]);
}

/* JSON string value (escaped, quoted). */
static void jw_qstr(JW *w, const char *s, size_t len)
{
    size_t i;
    jw_ch(w, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': jw_str(w, "\\\""); break;
        case '\\': jw_str(w, "\\\\"); break;
        case '\n': jw_str(w, "\\n"); break;
        case '\r': jw_str(w, "\\r"); break;
        case '\t': jw_str(w, "\\t"); break;
        case '\b': jw_str(w, "\\b"); break;
        case '\f': jw_str(w, "\\f"); break;
        default:
            if (c < 0x20u) {
                static const char hexd[] = "0123456789abcdef";
                jw_str(w, "\\u00");
                jw_ch(w, hexd[(c >> 4) & 0xF]);
                jw_ch(w, hexd[c & 0xF]);
            } else {
                jw_ch(w, (char)c);
            }
            break;
        }
    }
    jw_ch(w, '"');
}

/* ------------------------------------------------------------------ */
/* LSP context                                                         */
/* ------------------------------------------------------------------ */
struct weft_lsp_ctx_t {
    /* document state */
    char        doc[WEFT_LSP_MAX_DOC];
    uint32_t    doc_len;
    int64_t     doc_version;
    char        uri[WEFT_LSP_MAX_URI];
    uint32_t    uri_len;
    uint8_t     doc_open;
    uint8_t     initialized;
    uint8_t     shutdown_req;
    uint8_t     exited;
    uint32_t    reserved0;

    /* JSON machinery (per handle call) */
    jv_t        jnodes[WEFT_LSP_MAX_JNODES];
    uint32_t    njnodes;
    char        jscratch[WEFT_LSP_MAX_JSCRATCH];
    uint32_t    jscratch_n;

    /* id echo span (raw request bytes) */
    const char *raw;
    size_t      raw_len;
    uint32_t    id_off, id_len;
    uint8_t     has_id;

    /* the embedded compiler context (storage follows this struct) */
    weftc_ctx_t *comp;
};

/* DOM queries -------------------------------------------------------- */

/* OBJ member lookup: walks key/value pairs; returns the VALUE node. */
static uint32_t jv_obj_get(weft_lsp_ctx_t *L, uint32_t obj, const char *key)
{
    uint32_t it;
    size_t klen = 0;
    if (obj == UINT32_MAX || obj >= L->njnodes) return UINT32_MAX;
    while (key[klen]) klen++;
    it = L->jnodes[obj].first_child;
    while (it != UINT32_MAX && it < L->njnodes) {
        uint32_t val = L->jnodes[it].next_sibling;
        if (L->jnodes[it].key_len == klen &&
            memcmp(L->jscratch + L->jnodes[it].key_off, key, klen) == 0)
            return val < L->njnodes ? val : UINT32_MAX;
        it = (val < L->njnodes) ? L->jnodes[val].next_sibling : UINT32_MAX;
    }
    return UINT32_MAX;
}

/* ARR element at index i (value node chain). */
static uint32_t jv_arr_at(weft_lsp_ctx_t *L, uint32_t arr, uint32_t i)
{
    uint32_t it, k;
    if (arr == UINT32_MAX || arr >= L->njnodes) return UINT32_MAX;
    it = L->jnodes[arr].first_child;
    for (k = 0; it != UINT32_MAX && it < L->njnodes && k <= i; k++) {
        if (k == i) return it;
        it = L->jnodes[it].next_sibling;
    }
    return UINT32_MAX;
}

static const char *jv_str_ptr(weft_lsp_ctx_t *L, uint32_t node)
{
    if (node == UINT32_MAX || node >= L->njnodes) return NULL;
    if (L->jnodes[node].kind != JV_STR) return NULL;
    return L->jscratch + L->jnodes[node].str_off;
}
static uint32_t jv_str_len(weft_lsp_ctx_t *L, uint32_t node)
{
    if (node == UINT32_MAX || node >= L->njnodes) return 0;
    return L->jnodes[node].str_len;
}
static uint64_t jv_num(weft_lsp_ctx_t *L, uint32_t node)
{
    if (node == UINT32_MAX || node >= L->njnodes) return 0;
    return L->jnodes[node].num;
}

/* ------------------------------------------------------------------ */
/* Document sync                                                       */
/* ------------------------------------------------------------------ */

/* (line, character) -> byte offset (utf-8: bytes == chars). */
static int doc_pos_to_off(weft_lsp_ctx_t *L, uint64_t line, uint64_t ch,
                          uint32_t *off_out)
{
    uint32_t i = 0, ln = 0;
    const char *d = L->doc;
    uint32_t len = L->doc_len;
    while (ln < line && i < len) {
        if (d[i] == '\n') ln++;
        i++;
    }
    if (ln < line) { *off_out = len; return WEFT_STUDIO_EBOUNDS; }
    while (ch > 0 && i < len && d[i] != '\n') { i++; ch--; }
    *off_out = i;
    return WEFT_STUDIO_OK;
}

static void lsp_set_doc(weft_lsp_ctx_t *L, const char *text, uint32_t len)
{
    if (len > WEFT_LSP_MAX_DOC) len = WEFT_LSP_MAX_DOC; /* caller checks */
    if (len) memcpy(L->doc, text, len);
    L->doc_len = len;
}

static int lsp_apply_change(weft_lsp_ctx_t *L, uint32_t change)
{
    uint32_t text_node = jv_obj_get(L, change, "text");
    uint32_t range = jv_obj_get(L, change, "range");
    const char *text;
    uint32_t text_len;
    if (text_node == UINT32_MAX) return WEFT_STUDIO_EPARSE;
    text = jv_str_ptr(L, text_node);
    text_len = jv_str_len(L, text_node);
    if (!text) return WEFT_STUDIO_EPARSE;
    if (range == UINT32_MAX) {
        /* full replace */
        if (text_len > WEFT_LSP_MAX_DOC) return WEFT_STUDIO_EBOUNDS;
        lsp_set_doc(L, text, text_len);
        return WEFT_STUDIO_OK;
    }
    /* incremental */
    {
        uint32_t rs = jv_obj_get(L, range, "start");
        uint32_t re = jv_obj_get(L, range, "end");
        uint32_t start_off = 0, end_off = 0;
        uint64_t sl = 0, sc = 0, el = 0, ec = 0;
        int rc;
        if (rs != UINT32_MAX) {
            uint32_t n = jv_obj_get(L, rs, "line");
            uint32_t c = jv_obj_get(L, rs, "character");
            if (n != UINT32_MAX) sl = jv_num(L, n);
            if (c != UINT32_MAX) sc = jv_num(L, c);
        }
        if (re != UINT32_MAX) {
            uint32_t n = jv_obj_get(L, re, "line");
            uint32_t c = jv_obj_get(L, re, "character");
            if (n != UINT32_MAX) el = jv_num(L, n);
            if (c != UINT32_MAX) ec = jv_num(L, c);
        }
        rc = doc_pos_to_off(L, sl, sc, &start_off);
        if (rc) return rc;
        rc = doc_pos_to_off(L, el, ec, &end_off);
        if (rc) return rc;
        if (end_off < start_off) { uint32_t t = start_off; start_off = end_off; end_off = t; }
        if ((uint64_t)L->doc_len - (uint64_t)(end_off - start_off) +
            (uint64_t)text_len > WEFT_LSP_MAX_DOC)
            return WEFT_STUDIO_EBOUNDS;
        if (end_off < L->doc_len)
            memmove(L->doc + start_off + text_len, L->doc + end_off,
                    L->doc_len - end_off);
        if (text_len) memcpy(L->doc + start_off, text, text_len);
        L->doc_len = L->doc_len - (end_off - start_off) + text_len;
        return WEFT_STUDIO_OK;
    }
}

static void lsp_recompile(weft_lsp_ctx_t *L)
{
    weftc_compile(L->comp, L->doc, L->doc_len);
}

/* ------------------------------------------------------------------ */
/* Diagnostics notification                                            */
/* ------------------------------------------------------------------ */
static void emit_diag_json(JW *w, weft_lsp_ctx_t *L, const weft_diag_t *d)
{
    jw_str(w, "{\"range\":{\"start\":{\"line\":");
    jw_u64(w, d->start_line);
    jw_str(w, ",\"character\":");
    jw_u64(w, d->start_col);
    jw_str(w, "},\"end\":{\"line\":");
    jw_u64(w, d->end_line);
    jw_str(w, ",\"character\":");
    jw_u64(w, d->end_col);
    jw_str(w, "}},\"severity\":");
    jw_u64(w, d->severity);
    jw_str(w, ",\"code\":\"");
    jw_str(w, weft_diag_code_name(d->code));
    jw_str(w, "\",\"source\":\"weft-lsp\",\"message\":");
    if (d->message) {
        jw_qstr(w, d->message, strlen(d->message));
    } else {
        jw_str(w, "\"\"");
    }
    if (d->fix_suggestion) {
        jw_str(w, ",\"data\":{\"fix_suggestion\":");
        jw_qstr(w, d->fix_suggestion, strlen(d->fix_suggestion));
        jw_ch(w, '}');
    }
    jw_ch(w, '}');
    (void)L;
}

static int emit_publish_diagnostics(weft_lsp_ctx_t *L, char *notif,
                                    size_t cap, size_t *notif_len)
{
    JW w;
    uint32_t i;
    w.buf = notif; w.cap = cap; w.n = 0; w.of = 0;
    jw_str(&w, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
              "publishDiagnostics\",\"params\":{\"uri\":");
    jw_qstr(&w, L->uri, L->uri_len);
    jw_str(&w, ",\"version\":");
    jw_u64(&w, (uint64_t)L->doc_version);
    jw_str(&w, ",\"diagnostics\":[");
    for (i = 0; i < weftc_diag_count(L->comp); i++) {
        if (i) jw_ch(&w, ',');
        emit_diag_json(&w, L, weftc_diag_at(L->comp, i));
    }
    jw_str(&w, "]}}");
    if (w.of) return WEFT_STUDIO_EBOUNDS;
    notif[w.n] = '\0';
    *notif_len = w.n;
    return WEFT_STUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Hover                                                               */
/* ------------------------------------------------------------------ */

/* Finds the token containing byte offset off (or the one ending exactly
 * at off, e.g. cursor right after an identifier). */
static int32_t tok_at_off(weftc_ctx_t *c, uint32_t off)
{
    uint32_t i;
    int32_t best = -1;
    for (i = 0; i < c->ntoks; i++) {
        const weft_tok_t *t = &c->toks[i];
        if (t->kind == TOK_EOF) break;
        if (t->off <= off && off < t->off + (t->len ? t->len : 1u))
            return (int32_t)i;
        if (t->off + t->len == off) best = (int32_t)i;
    }
    return best;
}

static int src_ident_eq(weftc_ctx_t *c, const weft_tok_t *t, const char *name)
{
    uint32_t i;
    if (t->kind != TOK_IDENT) return 0;
    if (t->off + t->len > c->src_len) return 0;
    for (i = 0; name[i]; i++) {
        if (i >= t->len) return 0;
        if (name[i] != c->src[t->off + i]) return 0;
    }
    return i == t->len;
}

static void hover_text_field(JW *w, weftc_ctx_t *c, uint32_t di,
                             const weft_field_layout_t *F)
{
    jw_str(w, "**field** `");
    jw_str(w, F->name);
    jw_str(w, ": ");
    jw_str(w, F->type_str);
    jw_str(w, "` of `");
    jw_str(w, c->layouts[di].name);
    jw_str(w, "`\\n\\n- offset: ");
    jw_u64(w, F->offset);
    jw_str(w, "\\n- size: ");
    jw_u64(w, F->size);
    jw_str(w, "\\n- align: ");
    jw_u64(w, F->align);
    jw_str(w, "\\n- cache line: ");
    jw_u64(w, F->cache_line_idx);
    jw_str(w, " (bytes ");
    jw_u64(w, (uint64_t)F->cache_line_idx * 64u);
    jw_str(w, "..");
    jw_u64(w, (uint64_t)(F->cache_line_idx + 1u) * 64u - 1u);
    jw_str(w, ")");
    if (F->padding_after) {
        jw_str(w, "\\n- padding after: ");
        jw_u64(w, F->padding_after);
        jw_str(w, " B");
    }
    if (F->flags & WEFT_FLF_CROSSES_CL64)
        jw_str(w, "\\n- warning: crosses a 64 B cache-line boundary");
    if (F->flags & WEFT_FLF_CROSSES_CL128)
        jw_str(w, "\\n- warning: crosses a 128 B cache-line boundary");
    if (F->flags & WEFT_FLF_FALSE_SHARING)
        jw_str(w, "\\n- false-sharing tripwire: writable scalar on two lines");
}

/* Emits the response envelope prefix; the handler appends the result
 * body, then lsp_end_resp closes the object. */
static void lsp_begin_resp(weft_lsp_ctx_t *L, JW *w)
{
    jw_str(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (L->has_id && L->id_len && L->id_off + L->id_len <= L->raw_len)
        jw_mem(w, L->raw + L->id_off, L->id_len);
    else
        jw_str(w, "null");
    jw_str(w, ",\"result\":");
}

static int lsp_end_resp(JW *w, char *resp, size_t *resp_len)
{
    jw_ch(w, '}');
    if (w->of) return WEFT_STUDIO_EBOUNDS;
    resp[w->n] = '\0';
    *resp_len = w->n;
    return WEFT_STUDIO_OK;
}

static int emit_hover(weft_lsp_ctx_t *L, uint64_t line, uint64_t ch,
                      char *resp, size_t cap, size_t *resp_len)
{
    weftc_ctx_t *c = L->comp;
    JW w;
    uint32_t off = 0;
    int32_t ti;
    int rc;
    rc = doc_pos_to_off(L, line, ch, &off);
    if (rc) return rc;
    ti = tok_at_off(c, off);
    w.buf = resp; w.cap = cap; w.n = 0; w.of = 0;
    lsp_begin_resp(L, &w);
    if (ti < 0 || (uint32_t)ti >= c->ntoks) {
        jw_str(&w, "null");
    } else {
        const weft_tok_t *t = &c->toks[ti];
        int emitted = 0;
        /* field name? */
        {
            uint32_t di;
            for (di = 0; di < c->nlayouts && !emitted; di++) {
                const weftc_decl_t *d = &c->decls[di];
                uint32_t s;
                if (d->kind != DCL_STRUCT) continue;
                for (s = 0; s < d->nfields; s++) {
                    const weftc_field_t *f = &c->fields[d->first_field + s];
                    if (t->off == f->span_start &&
                        t->len == f->name_len) {
                        /* find the planned reflection entry */
                        uint32_t ps;
                        for (ps = 0; ps < d->nplanned; ps++) {
                            if (c->field_order[d->first_field + ps] ==
                                d->first_field + s) {
                                const weft_field_layout_t *F =
                                    &c->flayouts[d->first_field + ps];
                                jw_str(&w, "{\"contents\":{\"kind\":\"markdown\","
                                          "\"value\":\"");
                                hover_text_field(&w, c, di, F);
                                jw_str(&w, "\"},\"range\":{\"start\":{\"line\":");
                                jw_u64(&w, line);
                                jw_str(&w, ",\"character\":");
                                jw_u64(&w, ch);
                                jw_str(&w, "},\"end\":{\"line\":");
                                jw_u64(&w, line);
                                jw_str(&w, ",\"character\":");
                                jw_u64(&w, (uint64_t)ch + t->len);
                                jw_str(&w, "}}}");  /* end, range, body */
                                emitted = 1;
                                break;
                            }
                        }
                        if (!emitted) {
                            /* bad field (unresolved type): minimal hover */
                            jw_str(&w, "{\"contents\":{\"kind\":\"markdown\","
                                      "\"value\":\"**field** `");
                            jw_str(&w, c->name_pool + f->name_pool);
                            jw_str(&w, "` (type unresolved)\"}");
                            emitted = 1;
                        }
                        break;
                    }
                }
            }
        }
        /* decl name? */
        if (!emitted && t->kind == TOK_IDENT) {
            uint32_t di;
            for (di = 0; di < c->ndecls; di++) {
                const weftc_decl_t *d = &c->decls[di];
                if (t->off == d->span_start + 0 && 0) {}
                if (t->off == d->name_off && t->len == d->name_len) {
                    const weft_struct_layout_t *Lay = &c->layouts[di];
                    jw_str(&w, "{\"contents\":{\"kind\":\"markdown\",\"value\":\"**");
                    jw_str(&w, d->kind == DCL_STRUCT ? "struct"
                             : d->kind == DCL_ENUM ? "enum" : "bitflags");
                    jw_str(&w, "** `");
                    jw_str(&w, c->name_pool + d->name_pool);
                    jw_str(&w, "`\\n\\n- size: ");
                    jw_u64(&w, Lay->size);
                    jw_str(&w, " B\\n- align: ");
                    jw_u64(&w, Lay->align);
                    jw_str(&w, "\\n- abi_hash: ");
                    jw_hex64(&w, Lay->abi_hash);
                    if (d->kind == DCL_STRUCT) {
                        uint32_t s;
                        jw_str(&w, "\\n- fields: ");
                        jw_u64(&w, Lay->field_count);
                        jw_str(&w, "\\n- internal padding: ");
                        jw_u64(&w, Lay->internal_pad);
                        jw_str(&w, " B\\n- trailing padding: ");
                        jw_u64(&w, Lay->trailing_pad);
                        jw_str(&w, " B\\n- cache lines: ");
                        jw_u64(&w, Lay->cache_line_span);
                        if (Lay->optimize_hint) {
                            jw_str(&w, "\\n- @optimize(packing) saves ");
                            jw_u64(&w, Lay->optimize_hint);
                            jw_str(&w, " B");
                        }
                        jw_str(&w, "\\n\\nFields (final layout order):");
                        for (s = 0; s < d->nplanned; s++) {
                            const weftc_field_t *f =
                                &c->fields[c->field_order[d->first_field + s]];
                            jw_str(&w, "\\n- `");
                            jw_str(&w, c->name_pool + f->name_pool);
                            jw_str(&w, ": ");
                            jw_str(&w, c->name_pool + f->tystr_pool);
                            jw_str(&w, "` @ ");
                            jw_u64(&w, f->offset);
                        }
                    } else {
                        uint32_t s;
                        jw_str(&w, "\\n- backing: ");
                        jw_str(&w, weft_prim_names[d->backing]);
                        jw_str(&w, "\\n- variants: ");
                        for (s = 0; s < d->nvariants; s++) {
                            const weftc_variant_t *v =
                                &c->variants[d->first_variant + s];
                            if (s) jw_str(&w, ", ");
                            jw_str(&w, c->name_pool + v->name_pool);
                            jw_str(&w, " = ");
                            {
                                int64_t val = v->value;
                                if (val < 0) {
                                    jw_ch(&w, '-');
                                    jw_u64(&w, (uint64_t)(-(val + 1)) + 1u);
                                } else {
                                    jw_u64(&w, (uint64_t)val);
                                }
                            }
                        }
                    }
                    jw_str(&w, "\"}}");
                    emitted = 1;
                    break;
                }
            }
        }
        /* primitive type? */
        if (!emitted && t->kind == TOK_IDENT) {
            int k;
            for (k = 1; k <= 12; k++) {
                if (src_ident_eq(c, t, weft_prim_names[k])) {
                    jw_str(&w, "{\"contents\":{\"kind\":\"markdown\",\"value\":\""
                              "**primitive** `");
                    jw_str(&w, weft_prim_names[k]);
                    jw_str(&w, "` — ");
                    jw_u64(&w, weft_prim_sizes[k]);
                    jw_str(&w, " B, align ");
                    jw_u64(&w, weft_prim_aligns[k]);
                    jw_str(&w, "\"}}");
                    emitted = 1;
                    break;
                }
            }
        }
        /* attribute? */
        if (!emitted && t->kind == TOK_IDENT) {
            static const char *const attrs[4] = { "align", "simd", "packed",
                                                  "optimize" };
            int k;
            for (k = 0; k < 4; k++) {
                if (src_ident_eq(c, t, attrs[k])) {
                    jw_str(&w, "{\"contents\":{\"kind\":\"markdown\",\"value\":\""
                              "**attribute** `@");
                    jw_str(&w, attrs[k]);
                    jw_str(&w, "` — RFC-0017 layout attribute\"}}");
                    emitted = 1;
                    break;
                }
            }
        }
        if (!emitted) {
            jw_str(&w, "null");
        }
    }
    return lsp_end_resp(&w, resp, resp_len);
}

/* ------------------------------------------------------------------ */
/* Completion                                                          */
/* ------------------------------------------------------------------ */
static void jw_item(JW *w, const char *label, int kind, const char *detail)
{
    jw_str(w, "{\"label\":");
    jw_qstr(w, label, strlen(label));
    jw_str(w, ",\"kind\":");
    jw_u64(w, (uint64_t)kind);
    jw_str(w, ",\"detail\":");
    jw_qstr(w, detail, strlen(detail));
    jw_ch(w, '}');
}

static int emit_completion(weft_lsp_ctx_t *L, uint64_t line, uint64_t ch,
                           char *resp, size_t cap, size_t *resp_len)
{
    weftc_ctx_t *c = L->comp;
    JW w;
    uint32_t off = 0;
    int32_t ti;
    int mode = 0;   /* 0 = general, 1 = type position (after ':') */
    int rc;
    static const char *const kw[] = {
        "endianness", "little", "big", "struct", "enum", "bitflags",
        "span", "str", "packing"
    };
    static const char *const attr_kw[] = { "@align", "@simd", "@packed",
                                           "@optimize" };
    int i;
    rc = doc_pos_to_off(L, line, ch, &off);
    if (rc) return rc;
    /* context: ':' or '@' on the same line before the cursor, outside
     * comments (cheap heuristic, deterministic) */
    if (off > 0 && off <= L->doc_len) {
        uint32_t k = off;
        while (k > 0 && L->doc[k - 1] != '\n') {
            char pc = L->doc[k - 1];
            if (pc == ':') { mode = 1; break; }
            if (pc == '@') { mode = 2; break; }
            k--;
        }
    }
    w.buf = resp; w.cap = cap; w.n = 0; w.of = 0;
    lsp_begin_resp(L, &w);
    jw_str(&w, "{\"isIncomplete\":false,\"items\":[");
    ti = 0; (void)ti;
    if (mode == 2) {
        for (i = 0; i < 4; i++) {
            if (i) jw_ch(&w, ',');
            jw_item(&w, attr_kw[i], 14, "RFC-0017 layout attribute");
        }
    } else if (mode == 1) {
        for (i = 1; i <= 12; i++) {
            if (i > 1) jw_ch(&w, ',');
            jw_item(&w, weft_prim_names[i], 25, "primitive type");
        }
        {
            uint32_t di;
            for (di = 0; di < c->ndecls; di++) {
                const weftc_decl_t *d = &c->decls[di];
                if (d->kind == DCL_STRUCT) {
                    jw_ch(&w, ',');
                    jw_item(&w, c->name_pool + d->name_pool, 22,
                            "struct (in-memory layout)");
                }
            }
            for (di = 0; di < c->ndecls; di++) {
                const weftc_decl_t *d = &c->decls[di];
                if (d->kind != DCL_STRUCT) {
                    jw_ch(&w, ',');
                    jw_item(&w, c->name_pool + d->name_pool, 13,
                            d->kind == DCL_ENUM ? "enum" : "bitflags");
                }
            }
        }
    } else {
        for (i = 0; i < 9; i++) {
            if (i) jw_ch(&w, ',');
            jw_item(&w, kw[i], 14, "keyword");
        }
        for (i = 0; i < 4; i++) {
            jw_ch(&w, ',');
            jw_item(&w, attr_kw[i], 14, "layout attribute");
        }
        for (i = 1; i <= 12; i++) {
            jw_ch(&w, ',');
            jw_item(&w, weft_prim_names[i], 25, "primitive type");
        }
        {
            uint32_t di;
            for (di = 0; di < c->ndecls; di++) {
                const weftc_decl_t *d = &c->decls[di];
                jw_ch(&w, ',');
                jw_item(&w, c->name_pool + d->name_pool,
                        d->kind == DCL_STRUCT ? 22 : 13,
                        d->kind == DCL_STRUCT ? "struct"
                        : d->kind == DCL_ENUM ? "enum" : "bitflags");
            }
            for (di = 0; di < c->ndecls; di++) {
                const weftc_decl_t *d = &c->decls[di];
                if (d->kind == DCL_STRUCT) continue;
                {
                    uint32_t s;
                    for (s = 0; s < d->nvariants; s++) {
                        const weftc_variant_t *v =
                            &c->variants[d->first_variant + s];
                        jw_ch(&w, ',');
                        jw_item(&w, c->name_pool + v->name_pool, 20,
                                "enum tag");
                    }
                }
            }
        }
    }
    jw_str(&w, "]}");
    return lsp_end_resp(&w, resp, resp_len);
}

/* ------------------------------------------------------------------ */
/* Semantic tokens                                                     */
/* ------------------------------------------------------------------ */

/* Legend (frozen): index MUST match the capabilities announcement. */
static const char *const SEM_LEGEND[] = {
    "keyword", "type", "property", "enumMember", "number", "comment",
    "operator", "decorator"
};
#define SEM_NTYPES 8
#define SEM_MOD_DECLARATION 0x1u

static int sem_classify(weft_lsp_ctx_t *L, uint32_t ti,
                        uint32_t *type, uint32_t *mods)
{
    weftc_ctx_t *c = L->comp;
    const weft_tok_t *t = &c->toks[ti];
    *mods = 0;
    switch (t->kind) {
    case TOK_COMMENT_LINE:
    case TOK_COMMENT_BLOCK:
        *type = 5;
        return 1;
    case TOK_INT:
        *type = 4;
        return 1;
    case TOK_PUNCT:
    case TOK_INVALID:
        *type = 6;
        return 1;
    case TOK_EOF:
        return 0;
    case TOK_IDENT:
        break;
    default:
        return 0;
    }
    /* identifier classification */
    {
        static const char *const kws[] = {
            "endianness", "little", "big", "struct", "enum", "bitflags",
            "span", "str", "packing"
        };
        static const char *const attrs[4] = { "align", "simd", "packed",
                                              "optimize" };
        uint32_t di, i;
        for (i = 0; i < 9; i++) {
            if (src_ident_eq(c, t, kws[i])) { *type = 0; return 1; }
        }
        {
            int k;
            for (k = 0; k < 4; k++) {
                if (src_ident_eq(c, t, attrs[k])) { *type = 7; return 1; }
            }
        }
        for (i = 1; i <= 12; i++) {
            if (src_ident_eq(c, t, weft_prim_names[i])) {
                *type = 1;
                return 1;
            }
        }
        /* declaration names */
        for (di = 0; di < c->ndecls; di++) {
            const weftc_decl_t *d = &c->decls[di];
            if (t->off == d->name_off && t->len == d->name_len) {
                *type = 1;
                *mods = SEM_MOD_DECLARATION;
                return 1;
            }
        }
        /* field names (declaration site) */
        for (di = 0; di < c->ndecls; di++) {
            const weftc_decl_t *d = &c->decls[di];
            uint32_t s;
            if (d->kind != DCL_STRUCT) continue;
            for (s = 0; s < d->nfields; s++) {
                const weftc_field_t *f = &c->fields[d->first_field + s];
                if (t->off == f->span_start && t->len == f->name_len) {
                    *type = 2;
                    *mods = SEM_MOD_DECLARATION;
                    return 1;
                }
            }
        }
        /* variant names */
        for (di = 0; di < c->ndecls; di++) {
            const weftc_decl_t *d = &c->decls[di];
            uint32_t s;
            if (d->kind == DCL_STRUCT) continue;
            for (s = 0; s < d->nvariants; s++) {
                const weftc_variant_t *v = &c->variants[d->first_variant + s];
                if (t->off == v->span_start && t->len == v->name_len) {
                    *type = 3;
                    *mods = SEM_MOD_DECLARATION;
                    return 1;
                }
            }
        }
        /* named type references + non-declaration identifiers */
        *type = 1;
        return 1;
    }
}

static int emit_semantic_tokens(weft_lsp_ctx_t *L, char *resp, size_t cap,
                                size_t *resp_len)
{
    weftc_ctx_t *c = L->comp;
    JW w;
    uint32_t ti;
    uint32_t prev_line = 0, prev_col = 0;
    int first = 1;
    w.buf = resp; w.cap = cap; w.n = 0; w.of = 0;
    lsp_begin_resp(L, &w);
    jw_str(&w, "{\"data\":[");
    for (ti = 0; ti < c->ntoks; ti++) {
        const weft_tok_t *t = &c->toks[ti];
        uint32_t type = 0, mods = 0;
        if (t->kind == TOK_EOF) break;
        if (!sem_classify(L, ti, &type, &mods)) continue;
        if (t->len == 0) continue;
        /* multi-line block comments split per line */
        if (t->kind == TOK_COMMENT_BLOCK) {
            uint32_t k = 0;
            uint32_t seg_start = 0;
            uint32_t line = t->line - 1u, col = t->col - 1u;
            uint32_t cur_line = line, cur_col = col;
            for (k = 0; k <= t->len; k++) {
                char ch = (k < t->len) ? c->src[t->off + k] : '\0';
                if (ch == '\n' || k == t->len) {
                    uint32_t seg_len = k - seg_start;
                    if (seg_len) {
                        uint32_t dl = cur_line - prev_line;
                        uint32_t dc = (dl == 0) ? cur_col - prev_col
                                        : cur_col;
                        if (!first) jw_ch(&w, ',');
                        jw_u64(&w, dl);
                        jw_ch(&w, ',');
                        jw_u64(&w, dc);
                        jw_ch(&w, ',');
                        jw_u64(&w, seg_len);
                        jw_ch(&w, ',');
                        jw_u64(&w, 5);
                        jw_ch(&w, ',');
                        jw_u64(&w, 0);
                        first = 0;
                        prev_line = cur_line;
                        prev_col = cur_col;
                    }
                    cur_line++;
                    cur_col = 0;
                    seg_start = k + 1u;
                }
            }
            continue;
        }
        {
            uint32_t tline = t->line - 1u;
            uint32_t tcol = t->col - 1u;
            uint32_t dl = tline - prev_line;
            uint32_t dc = (dl == 0) ? tcol - prev_col : tcol;
            if (!first) jw_ch(&w, ',');
            jw_u64(&w, dl);
            jw_ch(&w, ',');
            jw_u64(&w, dc);
            jw_ch(&w, ',');
            jw_u64(&w, t->len);
            jw_ch(&w, ',');
            jw_u64(&w, type);
            jw_ch(&w, ',');
            jw_u64(&w, mods);
            first = 0;
            prev_line = tline;
            prev_col = tcol;
        }
    }
    jw_str(&w, "]}");
    return lsp_end_resp(&w, resp, resp_len);
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */
static void lsp_emit_error(weft_lsp_ctx_t *L, JW *w, int code,
                           const char *msg)
{
    jw_str(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (L->has_id && L->id_len && L->id_off + L->id_len <= L->raw_len)
        jw_mem(w, L->raw + L->id_off, L->id_len);
    else
        jw_str(w, "null");
    jw_str(w, ",\"error\":{\"code\":");
    if (code < 0) {
        jw_ch(w, '-');
        jw_u64(w, (uint64_t)(-(int64_t)code));
    } else {
        jw_u64(w, (uint64_t)code);
    }
    jw_str(w, ",\"message\":");
    jw_qstr(w, msg, strlen(msg));
    jw_str(w, "}}");
}

static int emit_initialize(weft_lsp_ctx_t *L, char *resp, size_t cap,
                           size_t *resp_len)
{
    JW w;
    w.buf = resp; w.cap = cap; w.n = 0; w.of = 0;
    lsp_begin_resp(L, &w);
    jw_str(&w, "{\"capabilities\":{\"positionEncoding\":\"utf-8\","
              "\"textDocumentSync\":{\"openClose\":true,\"change\":2},"
              "\"hoverProvider\":true,"
              "\"completionProvider\":{\"triggerCharacters\":[\"@\",\".\"]},"
              "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[");
    {
        int i;
        for (i = 0; i < SEM_NTYPES; i++) {
            if (i) jw_ch(&w, ',');
            jw_qstr(&w, SEM_LEGEND[i], strlen(SEM_LEGEND[i]));
        }
    }
    jw_str(&w, "],\"tokenModifiers\":[\"declaration\"]},\"full\":true}},"
              "\"serverInfo\":{\"name\":\"weft-lsp\",\"version\":\"1.0.0\"}}");
    return lsp_end_resp(&w, resp, resp_len);
}

int weft_lsp_handle(weft_lsp_ctx_t *ctx, const char *req, size_t req_len,
                    char *resp, size_t resp_cap, size_t *resp_len,
                    char *notif, size_t notif_cap, size_t *notif_len)
{
    JParser jp;
    uint32_t root, method, params, id;
    const char *mname;
    uint32_t mlen;
    if (resp_len) *resp_len = 0;
    if (notif_len) *notif_len = 0;
    if (!ctx || !req) return WEFT_STUDIO_EBOUNDS;
    ctx->njnodes = 0;
    ctx->jscratch_n = 0;
    ctx->has_id = 0;
    ctx->id_off = ctx->id_len = 0;
    ctx->raw = req;
    ctx->raw_len = req_len;

    jp.base = req;
    jp.len = req_len;
    jp.pos = 0;
    jp.nodes = ctx->jnodes;
    jp.nnodes = 0;
    jp.cap = WEFT_LSP_MAX_JNODES;
    jp.scratch = ctx->jscratch;
    jp.scratch_n = 0;
    jp.scratch_cap = WEFT_LSP_MAX_JSCRATCH;
    jp.err = 0;
    root = jp_value_wrapped(&jp, 0);
    if (jp.err || root == UINT32_MAX ||
        ctx->jnodes[root].kind != JV_OBJ) {
        JW w;
        w.buf = resp; w.cap = resp_cap; w.n = 0; w.of = 0;
        lsp_emit_error(ctx, &w, -32700, "parse error");
        if (w.of) return WEFT_STUDIO_EBOUNDS;
        if (resp) resp[w.n] = '\0';
        if (resp_len) *resp_len = w.n;
        return WEFT_STUDIO_EPARSE;
    }
    ctx->njnodes = jp.nnodes;

    id = jv_obj_get(ctx, root, "id");
    if (id != UINT32_MAX) {
        ctx->has_id = 1;
        ctx->id_off = ctx->jnodes[id].raw_off;
        ctx->id_len = ctx->jnodes[id].raw_len;
    }
    method = jv_obj_get(ctx, root, "method");
    params = jv_obj_get(ctx, root, "params");
    mname = jv_str_ptr(ctx, method);
    mlen = jv_str_len(ctx, method);

    if (!mname) {
        /* no method: invalid request when it carries an id */
        if (ctx->has_id) {
            JW w;
            w.buf = resp; w.cap = resp_cap; w.n = 0; w.of = 0;
            lsp_emit_error(ctx, &w, -32600, "invalid request");
            if (w.of) return WEFT_STUDIO_EBOUNDS;
            if (resp) resp[w.n] = '\0';
            if (resp_len) *resp_len = w.n;
        }
        return WEFT_STUDIO_OK;
    }

    if (mlen == 10 && memcmp(mname, "initialize", 10) == 0) {
        ctx->initialized = 1;
        return emit_initialize(ctx, resp, resp_cap, resp_len);
    }
    if (mlen == 11 && memcmp(mname, "initialized", 11) == 0) {
        return WEFT_STUDIO_OK; /* notification: no response */
    }
    if (mlen == 8 && memcmp(mname, "shutdown", 8) == 0) {
        ctx->shutdown_req = 1;
        {
            JW w;
            int rc;
            w.buf = resp; w.cap = resp_cap; w.n = 0; w.of = 0;
            lsp_begin_resp(ctx, &w);
            jw_str(&w, "null");
            rc = lsp_end_resp(&w, resp, resp_len);
            return rc;
        }
    }
    if (mlen == 4 && memcmp(mname, "exit", 4) == 0) {
        ctx->exited = 1;
        return WEFT_STUDIO_OK;
    }
    if (mlen == 20 && memcmp(mname, "textDocument/didOpen", 20) == 0) {
        uint32_t td = jv_obj_get(ctx, params, "textDocument");
        uint32_t uri = jv_obj_get(ctx, td, "uri");
        uint32_t text = jv_obj_get(ctx, td, "text");
        uint32_t ver = jv_obj_get(ctx, td, "version");
        const char *u = jv_str_ptr(ctx, uri);
        const char *t = jv_str_ptr(ctx, text);
        if (!u || !t) return WEFT_STUDIO_EPARSE;
        if (jv_str_len(ctx, uri) >= WEFT_LSP_MAX_URI) return WEFT_STUDIO_EBOUNDS;
        if (jv_str_len(ctx, text) > WEFT_LSP_MAX_DOC) return WEFT_STUDIO_EBOUNDS;
        memcpy(ctx->uri, u, jv_str_len(ctx, uri));
        ctx->uri_len = jv_str_len(ctx, uri);
        ctx->doc_version = (ver != UINT32_MAX)
                           ? (int64_t)jv_num(ctx, ver) : 0;
        lsp_set_doc(ctx, t, jv_str_len(ctx, text));
        ctx->doc_open = 1;
        lsp_recompile(ctx);
        return emit_publish_diagnostics(ctx, notif, notif_cap, notif_len);
    }
    if (mlen == 22 && memcmp(mname, "textDocument/didChange", 22) == 0) {
        uint32_t td = jv_obj_get(ctx, params, "textDocument");
        uint32_t ver = jv_obj_get(ctx, td, "version");
        uint32_t changes = jv_obj_get(ctx, params, "contentChanges");
        uint32_t ci = 0;
        if (changes == UINT32_MAX) return WEFT_STUDIO_EPARSE;
        ctx->doc_version = (ver != UINT32_MAX)
                           ? (int64_t)jv_num(ctx, ver) : ctx->doc_version;
        for (;;) {
            uint32_t change = jv_arr_at(ctx, changes, ci);
            int rc;
            if (change == UINT32_MAX) break;
            rc = lsp_apply_change(ctx, change);
            if (rc) return rc;
            ci++;
        }
        lsp_recompile(ctx);
        return emit_publish_diagnostics(ctx, notif, notif_cap, notif_len);
    }
    if (mlen == 21 && memcmp(mname, "textDocument/didClose", 21) == 0) {
        ctx->doc_open = 0;
        ctx->doc_len = 0;
        lsp_recompile(ctx);
        return emit_publish_diagnostics(ctx, notif, notif_cap, notif_len);
    }
    if (mlen == 18 && memcmp(mname, "textDocument/hover", 18) == 0) {
        uint32_t pos = jv_obj_get(ctx, params, "position");
        uint64_t line = 0, ch = 0;
        if (pos != UINT32_MAX) {
            uint32_t n = jv_obj_get(ctx, pos, "line");
            uint32_t c = jv_obj_get(ctx, pos, "character");
            if (n != UINT32_MAX) line = jv_num(ctx, n);
            if (c != UINT32_MAX) ch = jv_num(ctx, c);
        }
        return emit_hover(ctx, line, ch, resp, resp_cap, resp_len);
    }
    if (mlen == 23 && memcmp(mname, "textDocument/completion", 23) == 0) {
        uint32_t pos = jv_obj_get(ctx, params, "position");
        uint64_t line = 0, ch = 0;
        if (pos != UINT32_MAX) {
            uint32_t n = jv_obj_get(ctx, pos, "line");
            uint32_t c = jv_obj_get(ctx, pos, "character");
            if (n != UINT32_MAX) line = jv_num(ctx, n);
            if (c != UINT32_MAX) ch = jv_num(ctx, c);
        }
        return emit_completion(ctx, line, ch, resp, resp_cap, resp_len);
    }
    if (mlen == 32 &&
        memcmp(mname, "textDocument/semanticTokens/full", 32) == 0) {
        return emit_semantic_tokens(ctx, resp, resp_cap, resp_len);
    }
    /* unknown method */
    if (ctx->has_id) {
        JW w;
        w.buf = resp; w.cap = resp_cap; w.n = 0; w.of = 0;
        lsp_emit_error(ctx, &w, -32601, "method not found");
        if (w.of) return WEFT_STUDIO_EBOUNDS;
        if (resp) resp[w.n] = '\0';
        if (resp_len) *resp_len = w.n;
    }
    return WEFT_STUDIO_OK; /* notifications are silently ignored */
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
size_t weft_lsp_ctx_size(void)
{
    return sizeof(weft_lsp_ctx_t) + weftc_ctx_size();
}

int weft_lsp_ctx_init(void *mem, size_t size, weft_lsp_ctx_t **out)
{
    weft_lsp_ctx_t *L;
    char *comp_mem;
    if (!mem || !out) return WEFT_STUDIO_EBOUNDS;
    if (size < weft_lsp_ctx_size()) return WEFT_STUDIO_EBOUNDS;
    if (((uintptr_t)mem & 15u) != 0u) return WEFT_STUDIO_EALIGN;
    L = (weft_lsp_ctx_t *)mem;
    memset(L, 0, sizeof *L);
    comp_mem = (char *)mem + sizeof(weft_lsp_ctx_t);
    {
        int rc = weftc_ctx_init(comp_mem, weftc_ctx_size(), &L->comp);
        if (rc) return rc;
    }
    *out = L;
    return WEFT_STUDIO_OK;
}

int weft_lsp_exited(const weft_lsp_ctx_t *ctx)
{
    return (ctx && ctx->exited) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* stdio transport (Content-Length framing over the callback seam)     */
/* ------------------------------------------------------------------ */
typedef struct {
    weft_lsp_read_fn  rd;
    void             *rd_user;
    weft_lsp_write_fn wr;
    void             *wr_user;
    int               any_write_failed;
} LspIo;

/* Reads exactly one framed message; returns 0 at clean EOF, 1 on
 * message, -1 on malformed transport. */
static int lsp_read_frame(LspIo *io, char *buf, size_t cap, size_t *msg_len)
{
    uint64_t content_len = 0;
    int have_len = 0;
    for (;;) {
        char line[128];
        uint32_t n = 0;
        /* read a header line byte-by-byte */
        for (;;) {
            char ch;
            long got = io->rd(io->rd_user, &ch, 1);
            if (got <= 0) return 0;            /* EOF */
            if (ch == '\n') break;
            if (ch == '\r') continue;
            if (n < sizeof line - 1u) line[n++] = ch;
        }
        line[n] = '\0';
        if (n == 0 && have_len) break;        /* blank line after headers */
        if (n > 15 && memcmp(line, "Content-Length:", 15) == 0) {
            uint32_t k = 15;
            while (line[k] == ' ') k++;
            content_len = 0;
            while (line[k] >= '0' && line[k] <= '9') {
                content_len = content_len * 10u +
                              (uint64_t)(line[k] - '0');
                k++;
            }
            have_len = 1;
        }
        if (!have_len && n == 0) return -1;   /* headers never started */
    }
    if (!have_len) return -1;
    if (content_len > cap) return -1;
    {
        size_t got = 0;
        while (got < content_len) {
            long r = io->rd(io->rd_user, buf + got, (unsigned long)(content_len - got));
            if (r <= 0) return 0;
            got += (size_t)r;
        }
        *msg_len = got;
    }
    return 1;
}

static int lsp_write_frame(LspIo *io, const char *msg, size_t len)
{
    char hdr[40];
    size_t hl = 0;
    int i;
    uint64_t v = len;
    char digits[24];
    int nd = 0;
    if (v == 0) digits[nd++] = '0';
    while (v) { digits[nd++] = (char)('0' + (v % 10)); v /= 10; }
    hl = 0;
    {
        const char *pre = "Content-Length: ";
        while (*pre) hdr[hl++] = *pre++;
    }
    for (i = nd; i > 0; i--) hdr[hl++] = digits[i - 1];
    hdr[hl++] = '\r';
    hdr[hl++] = '\n';
    hdr[hl++] = '\r';
    hdr[hl++] = '\n';
    if (io->wr(io->wr_user, hdr, (unsigned long)hl) != (long)hl) {
        io->any_write_failed = 1;
        return WEFT_STUDIO_EBOUNDS;
    }
    if (len && io->wr(io->wr_user, msg, (unsigned long)len) != (long)len) {
        io->any_write_failed = 1;
        return WEFT_STUDIO_EBOUNDS;
    }
    return WEFT_STUDIO_OK;
}

int weft_lsp_serve(weft_lsp_ctx_t *ctx,
                   weft_lsp_read_fn read_fn, void *read_user,
                   weft_lsp_write_fn write_fn, void *write_user)
{
    static char msg[WEFT_LSP_MAX_DOC];
    static char resp[WEFT_LSP_MAX_DOC];
    static char notif[WEFT_LSP_MAX_DOC];
    LspIo io;
    if (!ctx || !read_fn || !write_fn) return WEFT_STUDIO_EBOUNDS;
    io.rd = read_fn;
    io.rd_user = read_user;
    io.wr = write_fn;
    io.wr_user = write_user;
    io.any_write_failed = 0;
    for (;;) {
        size_t msg_len = 0;
        int fr = lsp_read_frame(&io, msg, sizeof msg, &msg_len);
        if (fr == 0) break;                  /* clean EOF */
        if (fr < 0) return WEFT_STUDIO_EPARSE;
        {
            size_t resp_len = 0, notif_len = 0;
            int rc = weft_lsp_handle(ctx, msg, msg_len,
                                     resp, sizeof resp, &resp_len,
                                     notif, sizeof notif, &notif_len);
            if (rc == WEFT_STUDIO_EBOUNDS) return rc;
            if (resp_len) {
                rc = lsp_write_frame(&io, resp, resp_len);
                if (rc) return rc;
            }
            if (notif_len) {
                rc = lsp_write_frame(&io, notif, notif_len);
                if (rc) return rc;
            }
        }
        if (weft_lsp_exited(ctx)) break;
    }
    return io.any_write_failed ? WEFT_STUDIO_EBOUNDS : WEFT_STUDIO_OK;
}

