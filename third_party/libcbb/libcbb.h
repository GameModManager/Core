/*
 * libcbb - tiny BBCode-to-HTML microlibrary (pure C11, zero deps)
 *
 * MIT License
 * Copyright (c) 2026 Petrica
 *
 * Single-header, stb-style amalgamated distribution. Two-function API:
 * cbb_to_html(input) / cbb_free(s). See README.md for the supported tag set
 * and the three consumption patterns (header-only / one .c shim / CMake).
 */

#ifndef LIBCBB_H
#define LIBCBB_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert a NUL-terminated BBCode string to HTML. NULL input is treated as
 * "". Returns a newly allocated, NUL-terminated HTML string, or NULL on
 * allocation failure. Release with cbb_free.
 *
 * Supported tags: b, i, u, s, strike, color, size, font, url, img, quote,
 * code, list, olist, *, hr, center, right, spoiler, noparse, heading, h1-h4,
 * sub, sup, youtube, email, table, tr, th, td, line. Unknown tags drop but
 * their content passes through as escaped text.
 *
 * [code] and [noparse] both imply noparse semantics: the first matching
 * close terminates the block (a stray [/code] inside [noparse] - or vice
 * versa - is shown as literal text, not treated as a closer), and inner
 * content is HTML-escaped but emitted verbatim.
 *
 * [img] accepts both [img=src] and [img]src[/img] form; inner BBCode markup
 * inside [img]...[/img] is stripped so only the text content becomes the
 * src. URL args reject tab/newline/CR/control chars and leading/trailing
 * whitespace (WHATWG-strip sanitizer-bypass defense), and the color/size/
 * font args reject CSS meta-characters (;, {, }, (, ), ", ') and whitespace
 * other than a single ASCII space (CSS-injection and attribute-breakout
 * defense).
 */
char *cbb_to_html(const char *input);

/* Free a string returned by cbb_to_html. NULL is a no-op. */
void cbb_free(char *s);

#ifdef __cplusplus
}
#endif

/* ========================================================================
 * Implementation - compiled only when CBB_IMPLEMENTATION is defined in
 * exactly one translation unit before including this header.
 * ======================================================================== */
#ifdef CBB_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

typedef struct cbb_tag_def {
    const char *name;       /* lowercase BBCode tag name */
    const char *html_open;  /* opening template; %s becomes the arg when has_arg */
    const char *html_close; /* closing markup; may be empty */
    int has_arg;            /* 1 if [name=arg] is supported */
} cbb_tag_def;

static const cbb_tag_def cbb_tags[] = {
    {"b",       "<b>",                                          "</b>",          0},
    {"i",       "<i>",                                          "</i>",          0},
    {"u",       "<u>",                                          "</u>",          0},
    {"s",       "<s>",                                          "</s>",          0},
    {"strike",  "<s>",                                          "</s>",          0},
    {"color",   "<span style=\"color:%s;\">",                   "</span>",       1},
    {"size",    "<span style=\"font-size:%s;\">",               "</span>",       1},
    {"font",    "<span style=\"font-family:%s;\">",             "</span>",       1},
    {"url",     "<a href=\"%s\">",                               "</a>",          1},
    {"img",     "<img src=\"%s\" alt=\"\">",                    "",              1},
    {"quote",   "<blockquote>",                                 "</blockquote>", 0},
    {"code",    "<pre><code>",                                  "</code></pre>", 0},
    {"list",    "<ul>",                                         "</ul>",         0},
    {"olist",   "<ol>",                                         "</ol>",         0},
    {"ol",      "<ol>",                                         "</ol>",         0},
    {"ul",      "<ul>",                                         "</ul>",         0},
    {"*",       "<li>",                                         "</li>",         0},
    {"li",      "<li>",                                         "</li>",         0},
    {"hr",      "<hr>",                                         "",              0},
    {"line",    "<hr>",                                         "",              0},
    {"center",  "<div style=\"text-align:center;\">",           "</div>",        0},
    {"right",   "<div style=\"text-align:right;\">",            "</div>",        0},
    {"spoiler", "<details><summary>Spoiler</summary>",          "</details>",    0},
    {"noparse", "",                                             "",              0},
    {"heading", "<h3>",                                         "</h3>",         0},
    {"h1",      "<h1>",                                         "</h1>",         0},
    {"h2",      "<h2>",                                         "</h2>",         0},
    {"h3",      "<h3>",                                         "</h3>",         0},
    {"h4",      "<h4>",                                         "</h4>",         0},
    {"sub",     "<sub>",                                        "</sub>",        0},
    {"sup",     "<sup>",                                        "</sup>",        0},
    {"youtube", "<a href=\"https://www.youtube.com/watch?v=%s\">YouTube</a>", "", 1},
    {"email",   "<a href=\"mailto:%s\">",                       "</a>",          1},
    {"table",   "<table>",                                      "</table>",      0},
    {"tr",      "<tr>",                                         "</tr>",         0},
    {"th",      "<th>",                                         "</th>",         0},
    {"td",      "<td>",                                         "</td>",         0},
};
#define CBB_N_TAGS ((int)(sizeof cbb_tags / sizeof cbb_tags[0]))

static int cbb_lo(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int cbb_eq(const char *a, int alen, const char *b) {
    int blen = (int)strlen(b);
    if (blen != alen) return 0;
    for (int i = 0; i < alen; ++i)
        if (cbb_lo((unsigned char)a[i]) != cbb_lo((unsigned char)b[i])) return 0;
    return 1;
}

static const cbb_tag_def *cbb_lookup(const char *name, int nlen) {
    for (int i = 0; i < CBB_N_TAGS; ++i)
        if (cbb_eq(name, nlen, cbb_tags[i].name)) return &cbb_tags[i];
    return NULL;
}

/* --------- growable output buffer --------- */

typedef struct { char *data; size_t len, cap; int oom; } cbb_buf;

static void cbb_grow(cbb_buf *b, size_t need) {
    if (b->oom || b->cap - b->len > need) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap - b->len <= need) {
        if (cap > (size_t)-1 / 2) { b->oom = 1; return; }
        cap *= 2;
    }
    char *p = (char *)realloc(b->data, cap);
    if (!p) { b->oom = 1; return; }
    b->data = p; b->cap = cap;
}

static void cbb_putc(cbb_buf *b, char c) {
    cbb_grow(b, 1);
    if (b->oom) return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void cbb_putsn(cbb_buf *b, const char *s, size_t n) {
    cbb_grow(b, n);
    if (b->oom) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void cbb_puts(cbb_buf *b, const char *s) { cbb_putsn(b, s, strlen(s)); }

/* HTML-escape `s` (length n) into `b`. Same entity set for element text
 * and attribute values. */
static void cbb_esc(cbb_buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '<':  cbb_puts(b, "&lt;");   break;
        case '>':  cbb_puts(b, "&gt;");   break;
        case '&':  cbb_puts(b, "&amp;");  break;
        case '"':  cbb_puts(b, "&quot;"); break;
        case '\'': cbb_puts(b, "&#x27;"); break;
        default:   cbb_putc(b, (char)c);  break;
        }
    }
}

/* URL sanitizer. Returns 0 if `raw` is safe to emit as an href / img src,
 * -1 otherwise. Safe schemes: http, https, mailto, ftp, ftps.
 *
 * Defense against the WHATWG URL-strip bypass: the WHATWG URL parser strips
 * ASCII tab/LF/CR and trims leading/trailing C0 controls + whitespace
 * before parsing the scheme, so a sanitizer that scans the raw bytes will
 * miss bypasses like "java\tscript:alert(1)" or " javascript:alert(1)" -
 * both render in the browser as the dangerous "javascript:" scheme.
 *
 * Two layers: (1) outright reject any raw URL containing C0/DEL/whitespace
 * anywhere (those characters never appear in well-formed URLs), then
 * (2) build a normalized copy with C0 controls + edge whitespace removed,
 * and check the scheme against the allowlist. Pass 1 catches bypasses
 * before normalization; pass 2 is defense-in-depth in case pass 1 is ever
 * loosened. The normalized copy is heap-allocated so legitimately long
 * URLs (presigned S3/CDN/image URLs well over 256 bytes) are not
 * truncated. */
static int cbb_url_ok(const char *raw) {
    /* Pass 1: reject any C0 control, DEL, or whitespace anywhere. */
    size_t n = strlen(raw);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)raw[i];
        if (c <= 0x20 || c == 0x7f) return -1;
    }

    /* Pass 2: build a normalized copy (strip C0, trim edge whitespace). */
    char *buf = (char *)malloc(n + 1);
    if (!buf) return -1;
    size_t j = 0;
    int lead_ws = 1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)raw[i];
        int is_ctrl = (c <= 0x1f) || c == 0x7f;
        int is_sp = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (is_ctrl) continue;
        if (is_sp && lead_ws) continue;
        lead_ws = 0;
        if (is_sp && i + 1 == n) continue;
        buf[j++] = (char)c;
    }
    buf[j] = '\0';

    /* Pass 3: scheme detection on the normalized form. */
    const char *colon = NULL;
    for (const char *p = buf; *p; ++p) {
        int ch = (unsigned char)*p;
        if (ch == ':') { colon = p; break; }
        if (ch == '/' || ch == '?' || ch == '#') break;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'))
            break;
    }
    int rc;
    if (!colon) {
        rc = 0;
    } else {
        size_t slen = (size_t)(colon - buf);
        if (slen == 0 || slen > 15) {
            rc = -1;
        } else {
            char s[16];
            for (size_t i = 0; i < slen; ++i)
                s[i] = (char)cbb_lo((unsigned char)buf[i]);
            s[slen] = '\0';
            static const char *safe[] = {"http", "https", "mailto", "ftp", "ftps", NULL};
            int found = 0;
            for (int i = 0; safe[i]; ++i)
                if (strcmp(s, safe[i]) == 0) { found = 1; break; }
            rc = found ? 0 : -1;
        }
    }
    free(buf);
    return rc;
}

/* --------- tokenizer --------- */

typedef enum { T_TEXT, T_OPEN, T_CLOSE } cbb_kind;

typedef struct cbb_tok {
    cbb_kind kind;
    const char *name;       /* points into input */
    int name_len;
    const char *arg;        /* NULL if no argument */
    int arg_len;
    /* OPEN/CLOSE only: full original "[...]" substring, used to emit
     * verbatim when we are inside [noparse]. */
    const char *raw;
    int raw_len;
    /* TEXT only: */
    const char *text;
    int text_len;
} cbb_tok;

static int cbb_tok_(const char *in, cbb_tok *o, int cap) {
    int n = 0;
    const char *p = in, *ts = p;
    while (*p && n < cap) {
        if (*p != '[') { ++p; continue; }
        if (p > ts) {
            o[n].kind = T_TEXT; o[n].text = ts;
            o[n].text_len = (int)(p - ts);
            ++n;
            if (n >= cap) return n;
        }
        const char *rb = strchr(p, ']');
        if (!rb) { ++p; continue; }  /* unterminated [ -> literal text */
        const char *q = p + 1;
        int cl = (*q == '/');
        if (cl) ++q;
        const char *ns = q;
        while (q < rb && (((*q | 32) >= 'a' && (*q | 32) <= 'z') ||
                          (*q >= '0' && *q <= '9') || *q == '_' || *q == '*'))
            ++q;
        if (q == ns) { ts = p + 1; ++p; continue; } /* empty name */
        cbb_tok *t = &o[n++];
        t->name = ns; t->name_len = (int)(q - ns);
        t->arg = NULL; t->arg_len = 0;
        if (!cl && q < rb && *q == '=') { ++q; t->arg = q; t->arg_len = (int)(rb - q); }
        t->kind = cl ? T_CLOSE : T_OPEN;
        t->raw = p; t->raw_len = (int)(rb - p) + 1;
        p = rb + 1; ts = p;
    }
    if (*ts && n < cap) {
        o[n].kind = T_TEXT; o[n].text = ts;
        o[n].text_len = (int)strlen(ts);
        ++n;
    }
    return n;
}

/* --------- validation / output --------- */

/* Stack entry. tag == NULL means an unknown / [noparse] tag that has no
 * HTML markup to emit but still occupies a slot for [/name] matching.
 *
 * If `img_pending` is non-zero, the entry was opened as `[img]` (no arg)
 * and is waiting for the inner text to be collected as src. tag is the
 * `img` def, `arg` is a heap buffer that accumulates TEXT fragments. */
typedef struct cbb_st {
    const cbb_tag_def *tag;
    char *arg;            /* heap-allocated NUL-terminated arg (NULL if no arg,
                           * or, for img_pending entries, the accumulating buffer) */
    int img_pending;      /* 1 if this entry was opened as `[img]` with no arg */
} cbb_st;

/* Growable tag stack. Grows on demand the same way cbb_buf grows; depth
 * is always <= cap. */
typedef struct { cbb_st *data; int len; int cap; int oom; } cbb_stk;

static void cbb_stk_push(cbb_stk *s, cbb_st e) {
    if (s->oom) { free(e.arg); return; }
    if (s->len == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 16;
        /* INT_MAX/2 leaves headroom for one more doubling before bail. */
        if (ncap < s->cap || ncap > 0x3fffffff) { s->oom = 1; free(e.arg); return; }
        cbb_st *p = (cbb_st *)realloc(s->data, (size_t)ncap * sizeof *p);
        if (!p) { s->oom = 1; free(e.arg); return; }
        s->data = p; s->cap = ncap;
    }
    s->data[s->len++] = e;
}

static void cbb_emit_open(cbb_buf *o, const cbb_st *e) {
    if (!e->tag || !e->tag->html_open[0]) return;
    const char *tm = e->tag->html_open;
    const char *pc = strchr(tm, '%');
    if (!pc || pc[1] != 's') { cbb_puts(o, tm); return; }
    cbb_putsn(o, tm, (size_t)(pc - tm));
    if (e->arg) cbb_esc(o, e->arg, strlen(e->arg));
    cbb_puts(o, pc + 2);
}

static void cbb_emit_close(cbb_buf *o, const cbb_st *e) {
    /* img_pending entries accumulate inner text as src; close renders
     * the <img> tag now that we know the src. */
    if (e->img_pending) {
        cbb_puts(o, "<img src=\"");
        if (e->arg) cbb_esc(o, e->arg, strlen(e->arg));
        cbb_puts(o, "\" alt=\"\">");
        return;
    }
    if (e->tag && e->tag->html_close[0]) cbb_puts(o, e->tag->html_close);
}

/* Find the topmost stack entry whose tag matches `name`, or -1. */
static int cbb_find(const cbb_stk *s, const char *n, int nl) {
    for (int i = s->len - 1; i >= 0; --i)
        if (s->data[i].tag && cbb_eq(n, nl, s->data[i].tag->name)) return i;
    return -1;
}

/* Close stack entries from `to` up to (len-1) (inclusive), emit closes,
 * free their args, and update len to `to`. */
static void cbb_pop_to(cbb_buf *o, cbb_stk *s, int to) {
    for (int j = s->len - 1; j > to; --j) cbb_emit_close(o, &s->data[j]);
    cbb_emit_close(o, &s->data[to]);
    for (int j = to; j < s->len; ++j) free(s->data[j].arg);
    s->len = to;
}

/* Find the topmost <li> on the stack, or -1. */
static int cbb_find_li(const cbb_stk *s) {
    for (int i = s->len - 1; i >= 0; --i)
        if (s->data[i].tag && cbb_eq("*", 1, s->data[i].tag->name)) return i;
    return -1;
}

char *cbb_to_html(const char *input) {
    if (!input) input = "";

    size_t ilen = strlen(input);
    /* Defensive clamp: if input is huge (multi-GB) the size_t->int narrowing
     * would silently truncate. Cap at INT_MAX/2 so cbb_tok_ cannot loop past
     * int-range and the cast is well-defined. A truncation here means
     * "refuse to allocate this many, bail." Caller gets NULL. */
    int cap;
    if (ilen > (size_t)0x3fffffff) cap = 0x3fffffff;
    else cap = (int)(ilen / 2 + 16);
    cbb_tok *toks = (cbb_tok *)malloc(sizeof *toks * (size_t)cap);
    if (!toks) return NULL;
    int n = cbb_tok_(input, toks, cap);

    cbb_buf out = {0};
    cbb_stk stack = {0};
    int in_code = 0, noparse = 0;

    for (int i = 0; i < n && !out.oom; ++i) {
        const cbb_tok *t = &toks[i];
        if (t->kind == T_TEXT) {
            /* Topmost open tag is [img] waiting for its src: inner text
             * becomes the URL argument. We do NOT escape here - the arg
             * is the raw URL, and cbb_emit_close will escape it once at
             * close time (cbb_url_ok sanitization happens at that boundary,
             * same as for [img=src] form). */
            if (stack.len > 0 && stack.data[stack.len - 1].img_pending) {
                cbb_st *top = &stack.data[stack.len - 1];
                size_t cur = top->arg ? strlen(top->arg) : 0;
                size_t need = cur + (size_t)t->text_len + 1;
                if (need > (size_t)0x3fffffff) { out.oom = 1; break; }
                char *grown = (char *)realloc(top->arg, need);
                if (!grown) { out.oom = 1; break; }
                if (!top->arg) grown[0] = '\0';
                top->arg = grown;
                memcpy(top->arg + cur, t->text, (size_t)t->text_len);
                top->arg[need - 1] = '\0';
                continue;
            }
            cbb_esc(&out, t->text, (size_t)t->text_len);
            continue;
        }
        /* Topmost open tag is [img] waiting for its src: inner content
         * is the URL. TEXT is appended raw (handled above); any inner
         * OPEN/CLOSE tokens are stray BBCode markup that should be
         * STRIPPED - they are not part of the URL. EXCEPT the matching
         * closing [/img] which terminates the block and falls through
         * to the normal close path (which runs cbb_url_ok on the
         * accumulated src). Without this, [img][b]https://x.png[/b][/img]
         * would parse [b]...[/b] as BBCode and the URL would be lost. */
        if (stack.len > 0 && stack.data[stack.len - 1].img_pending) {
            const cbb_st *topp = &stack.data[stack.len - 1];
            int matching_close = (t->kind == T_CLOSE && topp->tag &&
                cbb_eq(t->name, t->name_len, topp->tag->name));
            if (!matching_close) continue;
        }
        const cbb_tag_def *def = cbb_lookup(t->name, t->name_len);

        /* Inside [noparse] or [code]: emit tags verbatim (escaped) until
         * the matching close. Both imply noparse semantics.
         *
         * The close handler consults the topmost stack entry: [code] pushes
         * the `code` tag def, [noparse] pushes NULL. The "cross-close" bug
         * is treating [/code] and [/noparse] as interchangeable terminators
         * - an inner close of the OTHER name would prematurely pop the
         * outer block and leak the rest into BBCode-parsed output. */
        if (noparse > 0) {
            if (t->kind == T_CLOSE && stack.len > 0) {
                const cbb_st *top = &stack.data[stack.len - 1];
                int top_is_noparse = (top->tag == NULL);
                int top_is_code = (top->tag && cbb_eq(top->tag->name, (int)strlen(top->tag->name), "code"));
                int close_is_noparse = cbb_eq(t->name, t->name_len, "noparse");
                int close_is_code = cbb_eq(t->name, t->name_len, "code");
                if ((top_is_noparse && close_is_noparse) ||
                    (top_is_code && close_is_code)) {
                    if (top_is_code) {
                        cbb_puts(&out, "</code></pre>");
                        in_code--;
                    }
                    noparse--;
                    stack.len--;
                    continue;
                }
            }
            cbb_esc(&out, t->raw, (size_t)t->raw_len);
            continue;
        }

        if (t->kind == T_OPEN) {
            if (def && cbb_eq(t->name, t->name_len, "code")) {
                cbb_stk_push(&stack, (cbb_st){def, NULL, 0});
                cbb_puts(&out, "<pre><code>");
                in_code++;
                noparse++; /* share the noparse suppression mechanism */
                continue;
            }
            if (def && cbb_eq(t->name, t->name_len, "noparse")) {
                cbb_stk_push(&stack, (cbb_st){NULL, NULL, 0});
                noparse++;
                continue;
            }
            /* [*] auto-closes any open <li>. */
            if (def && cbb_eq(t->name, t->name_len, "*")) {
                int li = cbb_find_li(&stack);
                if (li >= 0) cbb_pop_to(&out, &stack, li);
            }
            /* Unknown tag: drop the markers, content passes through. */
            if (!def) {
                cbb_stk_push(&stack, (cbb_st){NULL, NULL, 0});
                continue;
            }
            char *own = NULL;
            if (def->has_arg && t->arg) {
                own = (char *)malloc((size_t)t->arg_len + 1);
                if (!own) { out.oom = 1; break; }
                memcpy(own, t->arg, (size_t)t->arg_len);
                own[t->arg_len] = '\0';
                /* color, size, font land raw inside a style="..." attribute.
                 * Reject any arg containing CSS meta-characters (; { } ( )),
                 * any quote (so an attacker cannot break out of the
                 * attribute via " or '), or whitespace other than a single
                 * ASCII space - narrow enough to forbid property-injection. */
                if (cbb_eq(t->name, t->name_len, "color")
                 || cbb_eq(t->name, t->name_len, "size")
                 || cbb_eq(t->name, t->name_len, "font")) {
                    int bad = 0;
                    int sp = 0;
                    for (int k = 0; k < t->arg_len; ++k) {
                        unsigned char c = (unsigned char)own[k];
                        if (c == ';' || c == '{' || c == '}' ||
                            c == '(' || c == ')' ||
                            c == '"' || c == '\'') { bad = 1; break; }
                        if (c == ' ') { sp++; continue; }
                        if (c <= 0x20 || c == 0x7f) { bad = 1; break; }
                    }
                    if (bad || sp > 1) {
                        free(own);
                        cbb_stk_push(&stack, (cbb_st){NULL, NULL, 0});
                        continue;
                    }
                }
                int url_tag = cbb_eq(t->name, t->name_len, "url")
                           || cbb_eq(t->name, t->name_len, "img")
                           || cbb_eq(t->name, t->name_len, "youtube")
                           || cbb_eq(t->name, t->name_len, "email");
                if (url_tag && cbb_url_ok(own) != 0) {
                    /* Dangerous scheme: drop the tag. */
                    free(own);
                    cbb_stk_push(&stack, (cbb_st){NULL, NULL, 0});
                    continue;
                }
            }
            /* [img] without arg: enter "pending-src" mode. We push the
             * img def onto the stack with img_pending=1 and let the TEXT
             * handler accumulate inner text into arg. On the matching
             * [/img], cbb_pop_to emits the <img> tag (handled by
             * cbb_emit_close for img_pending entries). */
            if (cbb_eq(t->name, t->name_len, "img") && !t->arg) {
                cbb_st e = {def, NULL, 1};
                cbb_stk_push(&stack, e);
                if (stack.oom) { out.oom = 1; break; }
                continue;
            }
            cbb_st e = {def, own, 0};
            int idx = stack.len;
            cbb_stk_push(&stack, e);
            if (stack.oom) { out.oom = 1; break; }
            cbb_emit_open(&out, &stack.data[idx]);
            continue;
        }

        /* CLOSE
         *
         * Both [code] and [noparse] push distinct entries onto the stack
         * (code pushes the code tag def, noparse pushes NULL). A close
         * pop only fires if the top of the stack matches the close name -
         * same discipline used in the noparse>0 path above. */
        if (def && cbb_eq(t->name, t->name_len, "noparse")) {
            if (stack.len > 0 && stack.data[stack.len - 1].tag == NULL) {
                stack.len--;
                if (noparse > 0) noparse--;
            }
            continue;
        }
        if (def && cbb_eq(t->name, t->name_len, "code")) {
            if (stack.len > 0
             && stack.data[stack.len - 1].tag
             && cbb_eq(stack.data[stack.len - 1].tag->name,
                       (int)strlen(stack.data[stack.len - 1].tag->name),
                       "code")) {
                if (in_code > 0) { cbb_puts(&out, "</code></pre>"); in_code--; }
                if (noparse > 0) noparse--;
                stack.len--;
            }
            continue;
        }
        if (def && cbb_eq(t->name, t->name_len, "*")) {
            int li = cbb_find_li(&stack);
            if (li >= 0) cbb_pop_to(&out, &stack, li);
            continue;
        }
        /* Close for [img]src[/img]: if the matching entry is img_pending,
         * run URL sanitization on the accumulated src. If unsafe, drop
         * the tag entirely (no <img> emitted; inner text was consumed
         * into the entry's arg and gets freed by the cleanup). */
        if (def && cbb_eq(t->name, t->name_len, "img")) {
            int idx = -1;
            for (int j = stack.len - 1; j >= 0; --j)
                if (stack.data[j].tag && cbb_eq("img", 3, stack.data[j].tag->name)) {
                    idx = j; break;
                }
            if (idx >= 0 && stack.data[idx].img_pending) {
                char *src = stack.data[idx].arg;
                if (src && cbb_url_ok(src) != 0) {
                    /* Drop: free arg, shrink len, emit nothing. */
                    free(src);
                    stack.data[idx].arg = NULL;
                    for (int j = idx; j < stack.len - 1; ++j)
                        stack.data[j] = stack.data[j + 1];
                    stack.len--;
                    continue;
                }
            }
            int fi = cbb_find(&stack, t->name, t->name_len);
            if (fi >= 0) cbb_pop_to(&out, &stack, fi);
            continue;
        }
        int idx = cbb_find(&stack, t->name, t->name_len);
        if (idx >= 0) cbb_pop_to(&out, &stack, idx);
        /* unmatched close: silently ignored */
    }

    /* Auto-close any remaining open tags. */
    for (int j = stack.len - 1; j >= 0; --j) cbb_emit_close(&out, &stack.data[j]);
    for (int j = 0; j < stack.len; ++j) free(stack.data[j].arg);
    free(stack.data);
    free(toks);

    if (out.oom) { free(out.data); return NULL; }
    if (!out.data) { /* empty input -> malloc(1) so cbb_free has something */
        out.data = (char *)malloc(1);
        if (!out.data) return NULL;
        out.data[0] = '\0';
    }
    return out.data;
}

void cbb_free(char *s) { free(s); }

#endif /* CBB_IMPLEMENTATION */

#endif /* LIBCBB_H */