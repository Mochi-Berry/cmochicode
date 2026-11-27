/*
 * lex.c — bump arena + zero-copy lexer for cmochicode.
 */
#include "cmochi.h"

/* --------------------------- arena --------------------------- */
#define ARENA_CHUNK 65536

void cmc_arena_init(CmcArena *a) { a->head = NULL; }

void *cmc_arena_alloc(CmcArena *a, size_t n) {
    if (n == 0) n = 1;
    n = (n + 15) & ~(size_t)15;
    CmcChunk *c = a->head;
    if (!c || c->used + n > c->cap) {
        size_t cap = c ? c->cap * 2 : ARENA_CHUNK;
        if (cap < n) cap = n;
        c = (CmcChunk *)calloc(1, sizeof(CmcChunk) + cap);
        if (!c) { fprintf(stderr, "cmochi: out of memory\n"); exit(1); }
        c->cap = cap;
        c->next = a->head;
        a->head = c;
    }
    void *p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

char *cmc_arena_strndup(CmcArena *a, const char *s, size_t n) {
    char *p = (char *)cmc_arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void cmc_arena_free(CmcArena *a) {
    CmcChunk *c = a->head;
    while (c) { CmcChunk *n = c->next; free(c); c = n; }
    a->head = NULL;
}

/* --------------------------- lexer --------------------------- */
typedef struct { const char *s; CmcTokKind k; } Keyword;
static const Keyword KW[] = {
    {"var", TK_VAR}, {"let", TK_LET}, {"const", TK_CONST},
    {"function", TK_FUNCTION}, {"if", TK_IF}, {"else", TK_ELSE},
    {"while", TK_WHILE}, {"do", TK_DO}, {"for", TK_FOR},
    {"return", TK_RETURN}, {"break", TK_BREAK}, {"continue", TK_CONTINUE},
    {"switch", TK_SWITCH}, {"case", TK_CASE}, {"default", TK_DEFAULT},
    {"new", TK_NEW}, {"typeof", TK_TYPEOF}, {"null", TK_NULL},
    {"true", TK_TRUE}, {"false", TK_FALSE}, {"undefined", TK_UNDEFINED},
    {"this", TK_THIS}, {"try", TK_TRY}, {"catch", TK_CATCH},
    {"finally", TK_FINALLY}, {"throw", TK_THROW}, {"in", TK_IN},
    {"of", TK_OF}, {"class", TK_CLASS}, {"extends", TK_EXTENDS},
    {"delete", TK_DELETE}, {"void", TK_VOID}, {"instanceof", TK_INSTANCEOF},
};

static CmcTokKind kw_lookup(CmcSlice s) {
    for (size_t i = 0; i < sizeof(KW) / sizeof(KW[0]); i++) {
        if (cmc_slice_eq(s, KW[i].s)) return KW[i].k;
    }
    return TK_IDENT;
}

void cmc_lex_init(CmcLexer *lx, const char *src, size_t len, CmcArena *arena) {
    lx->src = src; lx->len = len; lx->pos = 0;
    lx->line = 1; lx->col = 1; lx->arena = arena;
}

static int peek(CmcLexer *lx) {
    return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : 0;
}
static int peek2(CmcLexer *lx) {
    return lx->pos + 1 < lx->len ? (unsigned char)lx->src[lx->pos + 1] : 0;
}
static int advance(CmcLexer *lx) {
    int c = peek(lx);
    if (c == 0) return 0;
    lx->pos++;
    if (c == '\n') { lx->line++; lx->col = 1; }
    else lx->col++;
    return c;
}
static void skip_ws(CmcLexer *lx) {
    for (;;) {
        int c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(lx); }
        else if (c == '/' && peek2(lx) == '/') {
            while (peek(lx) && peek(lx) != '\n') advance(lx);
        } else if (c == '/' && peek2(lx) == '*') {
            advance(lx); advance(lx);
            while (peek(lx) && !(peek(lx) == '*' && peek2(lx) == '/')) advance(lx);
            if (peek(lx)) { advance(lx); advance(lx); }
        } else break;
    }
}

static CmcToken tok(CmcTokKind k, CmcLexer *lx, size_t start, int line, int col) {
    CmcToken t;
    t.kind = k;
    t.text.ptr = lx->src + start;
    t.text.len = lx->pos - start;
    t.num = 0; t.bigint = 0;
    t.line = line; t.col = col;
    return t;
}

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static int is_ident_char(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }

static CmcToken lex_string(CmcLexer *lx, int quote, size_t start, int line, int col) {
    advance(lx); /* opening quote */
    /* decode into arena */
    size_t cap = 32, len = 0;
    char *buf = (char *)cmc_arena_alloc(lx->arena, cap);
    for (;;) {
        int c = advance(lx);
        if (c == 0 || c == '\n') { fprintf(stderr, "cmochi:%d:%d: unterminated string\n", line, col); break; }
        if (c == quote) break;
        if (c == '\\') {
            int e = advance(lx);
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '0': c = '\0'; break;
                case 'x': {
                    int h1 = advance(lx), h2 = advance(lx);
                    c = (h1 >= '0' && h1 <= '9' ? h1 - '0' : (h1|0x20) - 'a' + 10) * 16
                      + (h2 >= '0' && h2 <= '9' ? h2 - '0' : (h2|0x20) - 'a' + 10);
                    break;
                }
                case 'u': {
                    /* \uXXXX -> emit UTF-8 */
                    unsigned v = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = advance(lx);
                        v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (h|0x20) - 'a' + 10);
                    }
                    if (v < 0x80) c = v;
                    else {
                        unsigned char u[4]; int n = 0;
                        if (v < 0x800) { u[0] = 0xC0 | (v >> 6); u[1] = 0x80 | (v & 0x3F); n = 2; }
                        else { u[0] = 0xE0 | (v >> 12); u[1] = 0x80 | ((v >> 6) & 0x3F); u[2] = 0x80 | (v & 0x3F); n = 3; }
                        if (len + n >= cap) { cap *= 2; char *nb = (char *)cmc_arena_alloc(lx->arena, cap); memcpy(nb, buf, len); buf = nb; }
                        memcpy(buf + len, u, n); len += n;
                        continue;
                    }
                    break;
                }
                default: c = e; break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)cmc_arena_alloc(lx->arena, cap);
            memcpy(nb, buf, len); buf = nb;
        }
        buf[len++] = (char)c;
    }
    buf[len] = 0;
    CmcToken t = tok(TK_STR, lx, start, line, col);
    t.text.ptr = buf;   /* decoded, NUL-terminated */
    t.text.len = len;
    return t;
}

CmcToken cmc_lex_next(CmcLexer *lx) {
    skip_ws(lx);
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;
    int c = peek(lx);
    if (c == 0) return tok(TK_EOF, lx, start, line, col);

    /* identifiers / keywords */
    if (is_ident_start(c)) {
        while (is_ident_char(peek(lx))) advance(lx);
        CmcSlice s = { lx->src + start, lx->pos - start };
        return tok(kw_lookup(s), lx, start, line, col);
    }

    /* numbers */
    if (is_digit(c) || (c == '.' && is_digit(peek2(lx)))) {
        int is_bigint = 0;
        if (c == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
            advance(lx); advance(lx);
            while (is_ident_char(peek(lx))) advance(lx);
        } else {
            while (is_digit(peek(lx))) advance(lx);
            if (peek(lx) == '.' && is_digit(peek2(lx))) {
                advance(lx);
                while (is_digit(peek(lx))) advance(lx);
            }
            if (peek(lx) == 'e' || peek(lx) == 'E') {
                advance(lx);
                if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
                while (is_digit(peek(lx))) advance(lx);
            }
        }
        if (peek(lx) == 'n') { advance(lx); is_bigint = 1; }
        /* materialize NUL-terminated copy to parse */
        size_t n = lx->pos - start;
        char *tmp = (char *)cmc_arena_alloc(lx->arena, n + 1);
        memcpy(tmp, lx->src + start, n); tmp[n] = 0;
        CmcToken t = tok(is_bigint ? TK_BIGINT : TK_NUM, lx, start, line, col);
        if (is_bigint) {
            char *endp = NULL;
            t.bigint = strtoll(tmp, &endp, 0);
            if (endp && *endp == 'n') *endp = 0;
            /* recompute without trailing n */
            t.bigint = strtoll(tmp, NULL, 0);
        } else {
            t.num = strtod(tmp, NULL);
        }
        return t;
    }

    /* strings */
    if (c == '"' || c == '\'') return lex_string(lx, c, start, line, col);

    /* template literal: raw body until the closing backtick */
    if (c == '`') {
        advance(lx);
        size_t tstart = lx->pos;
        while (peek(lx) && peek(lx) != '`') advance(lx);
        size_t tlen = lx->pos - tstart;
        if (peek(lx)) advance(lx); /* closing backtick */
        char *raw = (char *)cmc_arena_alloc(lx->arena, tlen + 1);
        memcpy(raw, lx->src + tstart, tlen); raw[tlen] = 0;
        CmcToken t = tok(TK_TEMPLATE, lx, tstart, line, col);
        t.text.ptr = raw; t.text.len = tlen;
        return t;
    }

    /* punctuation / operators (longest match first) */
    advance(lx);
    int c2 = peek(lx);
    #define TWO(k1, k2) if (c == c1 && c2 == c2c) { advance(lx); return tok(k2, lx, start, line, col); }
    switch (c) {
        case '(': return tok(TK_LPAREN, lx, start, line, col);
        case ')': return tok(TK_RPAREN, lx, start, line, col);
        case '{': return tok(TK_LBRACE, lx, start, line, col);
        case '}': return tok(TK_RBRACE, lx, start, line, col);
        case '[': return tok(TK_LBRACKET, lx, start, line, col);
        case ']': return tok(TK_RBRACKET, lx, start, line, col);
        case ';': return tok(TK_SEMI, lx, start, line, col);
        case ',': return tok(TK_COMMA, lx, start, line, col);
        case ':': return tok(TK_COLON, lx, start, line, col);
        case '?':
            if (c2 == '?') { advance(lx); return tok(TK_NULLISH, lx, start, line, col); }
            if (c2 == '.') { advance(lx); return tok(TK_QMARKDOT, lx, start, line, col); }
            return tok(TK_QMARK, lx, start, line, col);
        case '.': if (c2 == '.') { advance(lx); if (peek(lx) == '.') { advance(lx); return tok(TK_DOTDOTDOT, lx, start, line, col); } return tok(TK_DOTDOTDOT, lx, start, line, col); } return tok(TK_DOT, lx, start, line, col);
        case '+':
            if (c2 == '+') { advance(lx); return tok(TK_PLUSPLUS, lx, start, line, col); }
            if (c2 == '=') { advance(lx); return tok(TK_PLUSEQ, lx, start, line, col); }
            return tok(TK_PLUS, lx, start, line, col);
        case '-':
            if (c2 == '-') { advance(lx); return tok(TK_MINUSMINUS, lx, start, line, col); }
            if (c2 == '=') { advance(lx); return tok(TK_MINUSEQ, lx, start, line, col); }
            return tok(TK_MINUS, lx, start, line, col);
        case '*':
            if (c2 == '*') { advance(lx); if (peek(lx) == '=') { advance(lx); return tok(TK_STARSTAREQ, lx, start, line, col); } return tok(TK_STARSTAR, lx, start, line, col); }
            if (c2 == '=') { advance(lx); return tok(TK_STAREQ, lx, start, line, col); }
            return tok(TK_STAR, lx, start, line, col);
        case '/': if (c2 == '=') { advance(lx); return tok(TK_SLASHEQ, lx, start, line, col); } return tok(TK_SLASH, lx, start, line, col);
        case '%': if (c2 == '=') { advance(lx); return tok(TK_PERCENTEQ, lx, start, line, col); } return tok(TK_PERCENT, lx, start, line, col);
        case '=':
            if (c2 == '=') { advance(lx); if (peek(lx) == '=') { advance(lx); return tok(TK_EQEQEQ, lx, start, line, col); } return tok(TK_EQEQ, lx, start, line, col); }
            if (c2 == '>') { advance(lx); return tok(TK_ARROW, lx, start, line, col); }
            return tok(TK_EQ, lx, start, line, col);
        case '!': if (c2 == '=') { advance(lx); if (peek(lx) == '=') { advance(lx); return tok(TK_NEEQ, lx, start, line, col); } return tok(TK_NE, lx, start, line, col); } return tok(TK_NOT, lx, start, line, col);
        case '<':
            if (c2 == '<') { advance(lx); if (peek(lx) == '=') { advance(lx); return tok(TK_SHLEQ, lx, start, line, col); } return tok(TK_SHL, lx, start, line, col); }
            if (c2 == '=') { advance(lx); return tok(TK_LE, lx, start, line, col); }
            return tok(TK_LT, lx, start, line, col);
        case '>':
            if (c2 == '>') {
                advance(lx);
                if (peek(lx) == '>') { advance(lx); if (peek(lx) == '=') { advance(lx); return tok(TK_SHRUEQ, lx, start, line, col); } return tok(TK_SHRU, lx, start, line, col); }
                if (peek(lx) == '=') { advance(lx); return tok(TK_SHREQ, lx, start, line, col); }
                return tok(TK_SHR, lx, start, line, col);
            }
            if (c2 == '=') { advance(lx); return tok(TK_GE, lx, start, line, col); }
            return tok(TK_GT, lx, start, line, col);
        case '&': if (c2 == '&') { advance(lx); return tok(TK_ANDAND, lx, start, line, col); } if (c2 == '=') { advance(lx); return tok(TK_ANDEQ, lx, start, line, col); } return tok(TK_AND, lx, start, line, col);
        case '|': if (c2 == '|') { advance(lx); return tok(TK_OROR, lx, start, line, col); } if (c2 == '=') { advance(lx); return tok(TK_OREQ, lx, start, line, col); } return tok(TK_OR, lx, start, line, col);
        case '^': if (c2 == '=') { advance(lx); return tok(TK_XOREQ, lx, start, line, col); } return tok(TK_XOR, lx, start, line, col);
        case '~': return tok(TK_TILDE, lx, start, line, col);
        default:
            fprintf(stderr, "cmochi:%d:%d: unexpected character '%c'\n", line, col, c);
            return tok(TK_EOF, lx, start, line, col);
    }
    #undef TWO
}
