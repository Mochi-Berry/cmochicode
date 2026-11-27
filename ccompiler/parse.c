/*
 * parse.c — Pratt/recursive-descent parser for cmochicode.
 * Produces AST nodes allocated in the bump arena.
 */
#include "cmochi.h"

typedef struct {
    CmcLexer  lx;
    CmcToken  cur;
    CmcArena *arena;
    int       err;
    /* parse-time table of const string bindings: lets `{ [k]: v }` name a
     * real field when k is a compile-time constant string. */
    const char *cs_name[64];
    const char *cs_val[64];
    int       cs_n;
} Parser;

static CmcNode *new_node(Parser *p, CmcNodeKind k) {
    CmcNode *n = (CmcNode *)cmc_arena_alloc(p->arena, sizeof(CmcNode));
    n->kind = k;
    n->line = p->cur.line;
    n->col = p->cur.col;
    return n;
}
static void next(Parser *p) { p->cur = cmc_lex_next(&p->lx); }
static int check(Parser *p, CmcTokKind k) { return p->cur.kind == k; }
static int accept(Parser *p, CmcTokKind k) { if (check(p, k)) { next(p); return 1; } return 0; }
static void expect(Parser *p, CmcTokKind k) {
    if (!accept(p, k)) {
        fprintf(stderr, "cmochi:%d:%d: expected token %d, got %d\n", p->cur.line, p->cur.col, k, p->cur.kind);
        p->err = 1;
    }
}

static const char *ident_name(Parser *p) {
    if (p->cur.kind != TK_IDENT) { expect(p, TK_IDENT); return ""; }
    const char *s = cmc_arena_strndup(p->arena, p->cur.text.ptr, p->cur.text.len);
    next(p);
    return s;
}

static void p_cstr_set(Parser *p, const char *name, const char *val) {
    for (int i = 0; i < p->cs_n; i++)
        if (strcmp(p->cs_name[i], name) == 0) { p->cs_val[i] = val; return; }
    if (p->cs_n >= 64) return;
    p->cs_name[p->cs_n] = name;
    p->cs_val[p->cs_n] = val;
    p->cs_n++;
}

/* fold an expression to a compile-time constant string, or NULL */
static const char *p_fold_str(Parser *p, CmcNode *n) {
    if (!n) return NULL;
    if (n->kind == N_STR) return n->str;
    if (n->kind == N_IDENT) {
        for (int i = 0; i < p->cs_n; i++)
            if (strcmp(p->cs_name[i], n->name) == 0) return p->cs_val[i];
        return NULL;
    }
    if (n->kind == N_BIN && n->op == TK_PLUS) {
        const char *l = p_fold_str(p, n->lhs);
        if (!l) return NULL;
        const char *r = p_fold_str(p, n->rhs);
        if (!r) return NULL;
        size_t ln = strlen(l), rn = strlen(r);
        char *buf = (char *)cmc_arena_alloc(p->arena, ln + rn + 1);
        memcpy(buf, l, ln);
        memcpy(buf + ln, r, rn);
        buf[ln + rn] = 0;
        return buf;
    }
    return NULL;
}

/* ------- expression forward decls ------- */
static CmcNode *parse_expr(Parser *p, int min_prec);
static CmcNode *parse_stmt(Parser *p);
static CmcNode *parse_template(Parser *p, const char *raw, size_t len);
static CmcNode *parse_block(Parser *p);
static CmcNode *parse_binding_target(Parser *p);

static int prec_of(CmcTokKind k) {
    switch (k) {
        case TK_EQ: case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ:
        case TK_SLASHEQ: case TK_PERCENTEQ: case TK_STARSTAREQ:
        case TK_ANDEQ: case TK_OREQ: case TK_XOREQ: case TK_SHLEQ:
        case TK_SHREQ: case TK_SHRUEQ:
        case TK_ANDANDEQ: case TK_OROREQ: case TK_NULLISHEQ: return 1;
        case TK_QMARK: return 2;
        case TK_NULLISH: return 3;
        case TK_OROR: return 4;
        case TK_ANDAND: return 5;
        case TK_OR: return 6;
        case TK_XOR: return 7;
        case TK_AND: return 8;
        case TK_EQEQ: case TK_EQEQEQ: case TK_NE: case TK_NEEQ: return 9;
        case TK_LT: case TK_LE: case TK_GT: case TK_GE: case TK_IN: case TK_INSTANCEOF: return 10;
        case TK_SHL: case TK_SHR: case TK_SHRU: return 11;
        case TK_PLUS: case TK_MINUS: return 12;
        case TK_STAR: case TK_SLASH: case TK_PERCENT: return 13;
        case TK_STARSTAR: return 14;
        default: return -1;
    }
}

static int right_assoc(CmcTokKind k) { return k == TK_STARSTAR || k == TK_EQ || prec_of(k) == 1; }

static int is_assign_op(CmcTokKind k) {
    return k == TK_EQ || k == TK_PLUSEQ || k == TK_MINUSEQ || k == TK_STAREQ ||
           k == TK_SLASHEQ || k == TK_PERCENTEQ || k == TK_STARSTAREQ ||
           k == TK_ANDEQ || k == TK_OREQ || k == TK_XOREQ || k == TK_SHLEQ ||
           k == TK_SHREQ || k == TK_SHRUEQ ||
           k == TK_ANDANDEQ || k == TK_OROREQ || k == TK_NULLISHEQ;
}

/* ------- primary / postfix ------- */
static CmcNode *parse_arrow_params(Parser *p) {
    /* '(' already consumed; parse a parameter list, then expect ')' and '=>' */
    CmcNode *fn = new_node(p, N_ARROW);
    int cap = 4, n = 0;
    CmcNode **params = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
    while (!check(p, TK_RPAREN)) {
        int rest = 0;
        if (check(p, TK_DOTDOTDOT)) { next(p); rest = 1; }
        CmcNode *param = parse_binding_target(p);
        if (rest) param->is_rest = 1;
        if (accept(p, TK_EQ)) {
            /* default value: wrap as N_ASSIGN-like marker — store rhs in third */
            CmcNode *def = new_node(p, N_ASSIGN);
            def->lhs = param;
            def->rhs = parse_expr(p, 2);
            def->is_rest = 1;
            param = def;
        }
        if (n == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, params, n * sizeof(CmcNode *)); params = np; }
        params[n++] = param;
        if (!accept(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN);
    expect(p, TK_ARROW);
    fn->params = params; fn->nparams = n;
    fn->is_expr_body = !check(p, TK_LBRACE);
    fn->body = fn->is_expr_body ? parse_expr(p, 2) : parse_block(p);
    return fn;
}

static CmcNode *parse_array_literal(Parser *p) {
    next(p); /* [ */
    CmcNode *n = new_node(p, N_ARR);
    int cap = 4, cnt = 0;
    CmcNode **elems = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
    while (!check(p, TK_RBRACKET) && !check(p, TK_EOF)) {
        if (check(p, TK_COMMA)) { next(p); continue; }
        if (check(p, TK_DOTDOTDOT)) next(p); /* spread: ignore marker for v0.1 */
        CmcNode *e = parse_expr(p, 2);
        if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, elems, cnt * sizeof(CmcNode *)); elems = np; }
        elems[cnt++] = e;
        if (!accept(p, TK_COMMA)) break;
    }
    expect(p, TK_RBRACKET);
    n->elems = elems; n->nelems = cnt;
    return n;
}

static CmcNode *parse_primary(Parser *p) {
    CmcTokKind k = p->cur.kind;
    switch (k) {
        case TK_NUM: {
            CmcNode *n = new_node(p, N_NUM);
            n->num = p->cur.num;
            next(p);
            return n;
        }
        case TK_BIGINT: {
            CmcNode *n = new_node(p, N_BIGINT);
            n->bigint = p->cur.bigint;
            next(p);
            return n;
        }
        case TK_STR: {
            CmcNode *n = new_node(p, N_STR);
            n->str = p->cur.text.ptr;
            next(p);
            return n;
        }
        case TK_TEMPLATE: {
            CmcNode *n = parse_template(p, p->cur.text.ptr, p->cur.text.len);
            next(p);
            return n;
        }
        case TK_TRUE: case TK_FALSE: {
            CmcNode *n = new_node(p, N_BOOL);
            n->boolval = k == TK_TRUE;
            next(p);
            return n;
        }
        case TK_NULL: { CmcNode *n = new_node(p, N_NULL); next(p); return n; }
        case TK_UNDEFINED: { CmcNode *n = new_node(p, N_UNDEF); next(p); return n; }
        case TK_THIS: { CmcNode *n = new_node(p, N_THIS); next(p); return n; }
        case TK_IDENT: {
            CmcNode *n = new_node(p, N_IDENT);
            n->name = ident_name(p);
            return n;
        }
        case TK_FUNCTION: {
            next(p);
            CmcNode *n = new_node(p, N_ARROW);
            n->is_async = 0;
            if (check(p, TK_IDENT)) n->fname = ident_name(p);
            expect(p, TK_LPAREN);
            int cap = 4, cnt = 0;
            CmcNode **params = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
            while (!check(p, TK_RPAREN)) {
                int rest = 0;
                if (check(p, TK_DOTDOTDOT)) { next(p); rest = 1; }
                CmcNode *param = parse_binding_target(p);
                if (rest) param->is_rest = 1;
                if (accept(p, TK_EQ)) { CmcNode *def = new_node(p, N_ASSIGN); def->lhs = param; def->rhs = parse_expr(p, 2); param = def; }
                if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, params, cnt * sizeof(CmcNode *)); params = np; }
                params[cnt++] = param;
                if (!accept(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN);
            n->params = params; n->nparams = cnt;
            n->is_expr_body = !check(p, TK_LBRACE);
            n->body = n->is_expr_body ? parse_expr(p, 2) : parse_block(p);
            return n;
        }
        case TK_LPAREN: {
            next(p);
            if (check(p, TK_RPAREN)) {
                next(p);
                if (accept(p, TK_ARROW)) {
                    CmcNode *fn = new_node(p, N_ARROW);
                    fn->nparams = 0;
                    fn->is_expr_body = !check(p, TK_LBRACE);
                    fn->body = fn->is_expr_body ? parse_expr(p, 2) : parse_block(p);
                    return fn;
                }
                CmcNode *n = new_node(p, N_SEQ); /* empty parens -> treat as undefined */
                n->nstmts = 0;
                return n;
            }
            /* Could be arrow params or parenthesized expr. Parse comma list, then check for '=>'. */
            int cap = 4, cnt = 0;
            CmcNode **items = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
            int rf = 0;
            if (check(p, TK_DOTDOTDOT)) { next(p); rf = 1; }
            items[cnt++] = parse_expr(p, 1); /* prec 1: allow `b = 10` (default param / assign) */
            if (rf && items[0]) items[0]->is_rest = 1;
            while (accept(p, TK_COMMA)) {
                rf = 0;
                if (check(p, TK_DOTDOTDOT)) { next(p); rf = 1; }
                if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, items, cnt * sizeof(CmcNode *)); items = np; }
                items[cnt++] = parse_expr(p, 1);
                if (rf && items[cnt-1]) items[cnt-1]->is_rest = 1;
            }
            expect(p, TK_RPAREN);
            if (check(p, TK_ARROW)) {
                next(p);
                CmcNode *fn = new_node(p, N_ARROW);
                fn->params = items; fn->nparams = cnt;
                fn->is_expr_body = !check(p, TK_LBRACE);
                fn->body = fn->is_expr_body ? parse_expr(p, 2) : parse_block(p);
                return fn;
            }
            if (cnt == 1) return items[0]; /* parenthesized */
            CmcNode *seq = new_node(p, N_SEQ);
            seq->stmts = items; seq->nstmts = cnt;
            return seq;
        }
        case TK_LBRACKET: return parse_array_literal(p); /* array literal in expression context */
        case TK_LBRACE: {
            /* object literal */
            next(p);
            CmcNode *n = new_node(p, N_OBJ);
            int cap = 4, cnt = 0;
            CmcProp *props = (CmcProp *)cmc_arena_alloc(p->arena, sizeof(CmcProp) * cap);
            while (!check(p, TK_RBRACE)) {
                if (check(p, TK_DOTDOTDOT)) {
                    /* spread: {...src} — record as a special prop */
                    next(p);
                    if (cnt == cap) { cap *= 2; CmcProp *np = (CmcProp *)cmc_arena_alloc(p->arena, sizeof(CmcProp) * cap); memcpy(np, props, cnt * sizeof(CmcProp)); props = np; }
                    props[cnt].key = NULL; props[cnt].computed = 0; props[cnt].spread = 1;
                    props[cnt].value = parse_expr(p, 2);
                    cnt++;
                    if (!accept(p, TK_COMMA)) break;
                    continue;
                }
                const char *key = NULL; int computed = 0;
                if (check(p, TK_IDENT) || p->cur.kind == TK_STR || p->cur.kind == TK_NUM) {
                    if (p->cur.kind == TK_IDENT) key = ident_name(p);
                    else { key = cmc_arena_strndup(p->arena, p->cur.text.ptr, p->cur.text.len); next(p); }
                } else if (check(p, TK_LBRACKET)) {
                    computed = 1;
                    next(p);
                    CmcNode *ke = parse_expr(p, 2);
                    key = p_fold_str(p, ke);
                    if (!key) key = "?";
                    expect(p, TK_RBRACKET);
                }
                if (accept(p, TK_COLON)) {
                    if (cnt == cap) { cap *= 2; CmcProp *np = (CmcProp *)cmc_arena_alloc(p->arena, sizeof(CmcProp) * cap); memcpy(np, props, cnt * sizeof(CmcProp)); props = np; }
                    props[cnt].key = key;
                    props[cnt].computed = computed;
                    props[cnt].value = parse_expr(p, 2);
                    cnt++;
                } else if (check(p, TK_LPAREN)) {
                    /* shorthand method { f() { ... } } — lower to an arrow fn */
                    if (cnt == cap) { cap *= 2; CmcProp *np = (CmcProp *)cmc_arena_alloc(p->arena, sizeof(CmcProp) * cap); memcpy(np, props, cnt * sizeof(CmcProp)); props = np; }
                    props[cnt].key = key; props[cnt].computed = computed;
                    CmcNode *fn = new_node(p, N_ARROW);
                    fn->is_expr_body = 0;
                    /* parse ( params ) { body } as a normal function */
                    expect(p, TK_LPAREN);
                    int pcap = 4, pcnt = 0;
                    CmcNode **params = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * pcap);
                    while (!check(p, TK_RPAREN)) {
                        CmcNode *param = parse_binding_target(p);
                        if (accept(p, TK_EQ)) { CmcNode *def = new_node(p, N_ASSIGN); def->lhs = param; def->rhs = parse_expr(p, 2); param = def; }
                        if (pcnt == pcap) { pcap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * pcap); memcpy(np, params, pcnt * sizeof(CmcNode *)); params = np; }
                        params[pcnt++] = param;
                        if (!accept(p, TK_COMMA)) break;
                    }
                    expect(p, TK_RPAREN);
                    fn->params = params; fn->nparams = pcnt;
                    fn->body = parse_block(p);
                    props[cnt].value = fn;
                    cnt++;
                } else {
                    /* shorthand {a} */
                    if (cnt == cap) { cap *= 2; CmcProp *np = (CmcProp *)cmc_arena_alloc(p->arena, sizeof(CmcProp) * cap); memcpy(np, props, cnt * sizeof(CmcProp)); props = np; }
                    props[cnt].key = key; props[cnt].computed = 0;
                    CmcNode *id = new_node(p, N_IDENT); id->name = key;
                    props[cnt].value = id;
                    cnt++;
                }
                if (!accept(p, TK_COMMA)) break;
            }
            expect(p, TK_RBRACE);
            n->props = props; n->nprops = cnt;
            return n;
        }
        case TK_NEW: {
            next(p);
            CmcNode *n = new_node(p, N_CALL);
            n->is_new = 1;
            n->obj = parse_primary(p);
            if (accept(p, TK_LPAREN)) {
                int cap = 4, cnt = 0;
                CmcNode **args = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
                while (!check(p, TK_RPAREN)) {
                    if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, args, cnt * sizeof(CmcNode *)); args = np; }
                    args[cnt++] = parse_expr(p, 2);
                    if (!accept(p, TK_COMMA)) break;
                }
                expect(p, TK_RPAREN);
                n->args = args; n->nargs = cnt;
            }
            return n;
        }
        default: {
            fprintf(stderr, "cmochi:%d:%d: unexpected token %d in expression\n", p->cur.line, p->cur.col, k);
            p->err = 1;
            next(p);
            CmcNode *n = new_node(p, N_NUM); n->num = 0;
            return n;
        }
    }
}

static CmcNode *parse_postfix(Parser *p, CmcNode *base) {
    for (;;) {
        if (check(p, TK_DOT) || check(p, TK_QMARKDOT)) {
            int optional = check(p, TK_QMARKDOT);
            next(p);
            CmcNode *m = new_node(p, N_MEMBER);
            m->obj = base;
            m->computed = 0;
            m->optional = optional;
            if (check(p, TK_IDENT)) m->prop = new_node(p, N_IDENT), m->prop->name = ident_name(p);
            else m->prop = new_node(p, N_IDENT), m->prop->name = "";
            base = m;
        } else if (check(p, TK_LBRACKET)) {
            next(p);
            CmcNode *m = new_node(p, N_MEMBER);
            m->obj = base;
            m->computed = 1;
            m->prop = parse_expr(p, 2);
            expect(p, TK_RBRACKET);
            base = m;
        } else if (check(p, TK_LPAREN)) {
            next(p);
            CmcNode *c = new_node(p, N_CALL);
            c->obj = base;
            int cap = 4, cnt = 0;
            CmcNode **args = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
            while (!check(p, TK_RPAREN)) {
                if (check(p, TK_DOTDOTDOT)) {
                    /* call spread: only array literals are spliced (v0.1) */
                    next(p);
                    CmcNode *sp = parse_expr(p, 2);
                    if (sp && sp->kind == N_ARR) {
                        for (int j = 0; j < sp->nelems; j++) {
                            if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, args, cnt * sizeof(CmcNode *)); args = np; }
                            args[cnt++] = sp->elems[j];
                        }
                    } else {
                        /* unknown spread source: keep as a single arg marker */
                        if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, args, cnt * sizeof(CmcNode *)); args = np; }
                        args[cnt++] = sp;
                    }
                } else {
                    if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, args, cnt * sizeof(CmcNode *)); args = np; }
                    args[cnt++] = parse_expr(p, 2);
                }
                if (!accept(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN);
            c->args = args; c->nargs = cnt;
            base = c;
        } else if (check(p, TK_PLUSPLUS) || check(p, TK_MINUSMINUS)) {
            CmcNode *u = new_node(p, N_UPDATE);
            u->lhs = base;
            u->op = p->cur.kind == TK_PLUSPLUS ? 1 : -1;
            u->third = NULL; /* postfix */
            next(p);
            base = u;
        } else break;
    }
    return base;
}

static CmcNode *parse_prefix(Parser *p) {
    CmcTokKind k = p->cur.kind;
    if (k == TK_NOT || k == TK_TILDE || k == TK_MINUS || k == TK_PLUS ||
        k == TK_TYPEOF || k == TK_DELETE || k == TK_VOID) {
        next(p);
        CmcNode *u = new_node(p, N_UNARY);
        u->op = k;
        u->lhs = parse_expr(p, 15);
        return u;
    }
    if (k == TK_PLUSPLUS || k == TK_MINUSMINUS) {
        next(p);
        CmcNode *u = new_node(p, N_UPDATE);
        u->op = k == TK_PLUSPLUS ? 1 : -1;
        u->lhs = parse_expr(p, 15);
        u->third = (CmcNode *)1; /* prefix */
        return u;
    }
    return parse_postfix(p, parse_primary(p));
}

/* arrow function as bare identifier: `x => ...` */
static CmcNode *maybe_arrow_from_ident(Parser *p, CmcNode *id) {
    if (id && id->kind == N_IDENT && check(p, TK_ARROW)) {
        next(p);
        CmcNode *fn = new_node(p, N_ARROW);
        CmcNode **params = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *));
        params[0] = id;
        fn->params = params; fn->nparams = 1;
        fn->is_expr_body = !check(p, TK_LBRACE);
        fn->body = fn->is_expr_body ? parse_expr(p, 2) : parse_block(p);
        return fn;
    }
    return id;
}

static CmcNode *parse_expr(Parser *p, int min_prec) {
    CmcNode *lhs = parse_prefix(p);
    if (lhs && lhs->kind == N_IDENT && check(p, TK_ARROW)) {
        return maybe_arrow_from_ident(p, lhs);
    }
    for (;;) {
        CmcTokKind op = p->cur.kind;
        if (op == TK_QMARK) {
            /* ternary */
            if (min_prec > 2) break;
            next(p);
            CmcNode *c = new_node(p, N_COND);
            c->lhs = lhs;
            c->rhs = parse_expr(p, 2);
            expect(p, TK_COLON);
            c->third = parse_expr(p, 2);
            lhs = c;
            continue;
        }
        int prec = prec_of(op);
        if (prec < 0 || prec < min_prec) break;
        next(p);
        int next_min = right_assoc(op) ? prec : prec + 1;
        CmcNode *rhs = parse_expr(p, next_min);
        if (is_assign_op(op)) {
            CmcNode *a = new_node(p, N_ASSIGN);
            a->lhs = lhs; a->op = op; a->rhs = rhs;
            lhs = a;
        } else {
            CmcNode *b = new_node(p, N_BIN);
            b->op = op; b->lhs = lhs; b->rhs = rhs;
            lhs = b;
        }
    }
    return lhs;
}

/* Sub-parse a JS expression from a raw slice (for template ${...}). */
static CmcNode *sub_parse_expr(Parser *p, const char *src, size_t len) {
    Parser sp;
    cmc_lex_init(&sp.lx, src, len, p->arena);
    sp.arena = p->arena;
    sp.err = 0;
    next(&sp);
    CmcNode *e = parse_expr(&sp, 1);
    return e;
}

/* Parse a template literal body: literal parts + ${expr} interleaved. */
static CmcNode *parse_template(Parser *p, const char *raw, size_t len) {
    CmcNode *n = new_node(p, N_TEMPLATE);
    int qcap = 4, ecap = 4, qn = 0, en = 0;
    CmcNode **quasis = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * qcap);
    CmcNode **exprs = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * ecap);
    size_t i = 0, lit = 0;
    while (i < len) {
        if (raw[i] == '$' && i + 1 < len && raw[i + 1] == '{') {
            CmcNode *q = new_node(p, N_STR);
            q->str = cmc_arena_strndup(p->arena, raw + lit, i - lit);
            if (qn == qcap) { qcap *= 2; CmcNode **nq = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * qcap); memcpy(nq, quasis, qn * sizeof(CmcNode *)); quasis = nq; }
            quasis[qn++] = q;
            size_t j = i + 2; int depth = 1;
            while (j < len && depth > 0) {
                if (raw[j] == '{') depth++;
                else if (raw[j] == '}') depth--;
                if (depth > 0) j++;
            }
            if (en == ecap) { ecap *= 2; CmcNode **ne = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * ecap); memcpy(ne, exprs, en * sizeof(CmcNode *)); exprs = ne; }
            exprs[en++] = sub_parse_expr(p, raw + i + 2, j - (i + 2));
            i = j + 1; lit = i;
        } else i++;
    }
    CmcNode *q = new_node(p, N_STR);
    q->str = cmc_arena_strndup(p->arena, raw + lit, len - lit);
    if (qn == qcap) { qcap *= 2; CmcNode **nq = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * qcap); memcpy(nq, quasis, qn * sizeof(CmcNode *)); quasis = nq; }
    quasis[qn++] = q;
    n->quasis = quasis; n->nquasis = qn;
    n->exprs = exprs; n->nexprs = en;
    return n;
}
static CmcNode *parse_binding_target(Parser *p) {
    if (check(p, TK_IDENT)) {
        CmcNode *n = new_node(p, N_IDENT);
        n->name = ident_name(p);
        return n;
    }
    if (check(p, TK_LBRACKET)) {
        next(p);
        CmcNode *n = new_node(p, N_DESTRUCT_ARR);
        int cap = 4, cnt = 0;
        CmcNode **elems = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
        while (!check(p, TK_RBRACKET)) {
            if (check(p, TK_DOTDOTDOT)) { next(p); }
            if (check(p, TK_COMMA)) { next(p); continue; }
            CmcNode *el = parse_binding_target(p);
            if (accept(p, TK_EQ)) { CmcNode *def = new_node(p, N_ASSIGN); def->lhs = el; def->rhs = parse_expr(p, 2); el = def; }
            if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, elems, cnt * sizeof(CmcNode *)); elems = np; }
            elems[cnt++] = el;
            if (!accept(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACKET);
        n->elems = elems; n->nelems = cnt;
        return n;
    }
    if (check(p, TK_LBRACE)) {
        next(p);
        CmcNode *n = new_node(p, N_DESTRUCT_OBJ);
        int cap = 4, cnt = 0;
        CmcNode **keys = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
        CmcNode **vals = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
        while (!check(p, TK_RBRACE)) {
            if (check(p, TK_DOTDOTDOT)) { next(p); }
            const char *key = NULL;
            if (check(p, TK_IDENT) || check(p, TK_STR)) {
                key = cmc_arena_strndup(p->arena, p->cur.text.ptr, p->cur.text.len);
                next(p);
            }
            CmcNode *val = NULL;
            if (accept(p, TK_COLON)) val = parse_binding_target(p);
            else { val = new_node(p, N_IDENT); val->name = key; }
            if (accept(p, TK_EQ)) { CmcNode *def = new_node(p, N_ASSIGN); def->lhs = val; def->rhs = parse_expr(p, 2); val = def; }
            if (cnt == cap) { cap *= 2; CmcNode **nk = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(nk, keys, cnt * sizeof(CmcNode *)); keys = nk; CmcNode **nv = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(nv, vals, cnt * sizeof(CmcNode *)); vals = nv; }
            keys[cnt] = new_node(p, N_STR); keys[cnt]->str = key ? key : "?";
            vals[cnt] = val;
            cnt++;
            if (!accept(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE);
        n->dpat_keys = keys; n->dpat_vals = vals; n->ndpat = cnt;
        return n;
    }
    fprintf(stderr, "cmochi:%d:%d: expected binding target\n", p->cur.line, p->cur.col);
    p->err = 1;
    next(p);
    CmcNode *n = new_node(p, N_IDENT); n->name = "_";
    return n;
}

/* ------- statements ------- */
static CmcNode *parse_var_decl(Parser *p, CmcNodeKind kind) {
    next(p); /* var/let/const */
    CmcNode *n = new_node(p, N_VAR);
    int cap = 4, cnt = 0;
    CmcNode **decls = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
    for (;;) {
        CmcNode *id = parse_binding_target(p);
        CmcNode *init = NULL;
        if (accept(p, TK_EQ)) init = parse_expr(p, 2);
        CmcNode *d = new_node(p, N_ASSIGN);
        d->lhs = id; d->rhs = init; d->op = kind; /* reuse op to carry var kind */
        if (id && id->kind == N_IDENT && init) {
            const char *sv = p_fold_str(p, init);
            if (sv) p_cstr_set(p, id->name, sv);
        }
        if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, decls, cnt * sizeof(CmcNode *)); decls = np; }
        decls[cnt++] = d;
        if (!accept(p, TK_COMMA)) break;
    }
    n->stmts = decls; n->nstmts = cnt;
    return n;
}

static CmcNode *parse_block(Parser *p) {
    expect(p, TK_LBRACE);
    CmcNode *n = new_node(p, N_BLOCK);
    int cap = 8, cnt = 0;
    CmcNode **stmts = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        CmcNode *st = parse_stmt(p);
        if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, stmts, cnt * sizeof(CmcNode *)); stmts = np; }
        stmts[cnt++] = st;
    }
    expect(p, TK_RBRACE);
    n->stmts = stmts; n->nstmts = cnt;
    return n;
}

static CmcNode *parse_for(Parser *p) {
    next(p); /* for */
    CmcNode *n = new_node(p, N_FOR);
    expect(p, TK_LPAREN);
    if (check(p, TK_VAR) || check(p, TK_LET) || check(p, TK_CONST)) {
        n->init = parse_var_decl(p, p->cur.kind);
    } else if (!check(p, TK_SEMI)) {
        n->init = parse_expr(p, 2);
        accept(p, TK_SEMI);
    }
    /* detect for-of: `for (var x of arr)` */
    if (check(p, TK_OF)) {
        next(p);
        n->kind = N_FOROF;
        n->left = n->init->stmts[0]->lhs;
        n->test = parse_expr(p, 2);
        expect(p, TK_RPAREN);
        n->stmt = parse_stmt(p);
        return n;
    }
    /* detect for-in: `for (var k in obj)` */
    if (check(p, TK_IN)) {
        next(p);
        n->kind = N_FORIN;
        n->left = n->init->stmts[0]->lhs;
        n->test = parse_expr(p, 2);
        expect(p, TK_RPAREN);
        n->stmt = parse_stmt(p);
        return n;
    }
    if (accept(p, TK_SEMI)) {
        if (!check(p, TK_SEMI)) n->test = parse_expr(p, 2);
        expect(p, TK_SEMI);
        if (!check(p, TK_RPAREN)) n->update = parse_expr(p, 2);
    }
    expect(p, TK_RPAREN);
    n->stmt = parse_stmt(p);
    return n;
}

static CmcNode *parse_stmt(Parser *p) {
    CmcTokKind k = p->cur.kind;
    switch (k) {
        case TK_VAR: case TK_LET: case TK_CONST: {
            CmcNode *n = parse_var_decl(p, k);
            accept(p, TK_SEMI);
            return n;
        }
        case TK_FUNCTION: {
            next(p);
            CmcNode *n = new_node(p, N_FUNC);
            n->fname = ident_name(p);
            expect(p, TK_LPAREN);
            int cap = 4, cnt = 0;
            CmcNode **params = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
            while (!check(p, TK_RPAREN)) {
                int rest = 0;
                if (check(p, TK_DOTDOTDOT)) { next(p); rest = 1; }
                CmcNode *param = parse_binding_target(p);
                if (rest) param->is_rest = 1;
                if (accept(p, TK_EQ)) { CmcNode *def = new_node(p, N_ASSIGN); def->lhs = param; def->rhs = parse_expr(p, 2); param = def; }
                if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(np, params, cnt * sizeof(CmcNode *)); params = np; }
                params[cnt++] = param;
                if (!accept(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN);
            n->params = params; n->nparams = cnt;
            n->body = parse_block(p);
            return n;
        }
        case TK_CLASS: {
            /* class Name [extends Base] { ctor/methods } -> N_CLASS with CmcProp[] */
            next(p);
            CmcNode *n = new_node(p, N_CLASS);
            n->fname = ident_name(p);
            if (accept(p, TK_EXTENDS)) n->obj = parse_primary(p); /* base class name */
            CmcProp *props = (CmcProp *)cmc_arena_alloc(p->arena, sizeof(CmcProp) * 4);
            int cap = 4, cnt = 0;
            expect(p, TK_LBRACE);
            while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                if (check(p, TK_SEMI) || check(p, TK_DOTDOTDOT)) { next(p); continue; }
                const char *mname = ident_name(p);
                CmcNode *fn = new_node(p, N_ARROW);
                fn->is_expr_body = 0;
                fn->fname = mname;
                expect(p, TK_LPAREN);
                int pcap = 4, pcnt = 0;
                CmcNode **params = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * pcap);
                while (!check(p, TK_RPAREN)) {
                    CmcNode *par = parse_binding_target(p);
                    if (accept(p, TK_EQ)) { CmcNode *d = new_node(p, N_ASSIGN); d->lhs = par; d->rhs = parse_expr(p, 2); par = d; }
                    if (pcnt == pcap) { pcap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * pcap); memcpy(np, params, pcnt * sizeof(CmcNode *)); params = np; }
                    params[pcnt++] = par;
                    if (!accept(p, TK_COMMA)) break;
                }
                expect(p, TK_RPAREN);
                fn->params = params; fn->nparams = pcnt;
                fn->body = parse_block(p);
                if (cnt == cap) { cap *= 2; CmcProp *np = (CmcProp *)cmc_arena_alloc(p->arena, sizeof(CmcProp) * cap); memcpy(np, props, cnt * sizeof(CmcProp)); props = np; }
                props[cnt].key = mname;
                props[cnt].value = fn;
                props[cnt].spread = 0;
                props[cnt].computed = 0;
                cnt++;
                if (!accept(p, TK_SEMI)) ;
            }
            expect(p, TK_RBRACE);
            n->props = props; n->nprops = cnt;
            return n;
        }
        case TK_IF: {
            next(p);
            expect(p, TK_LPAREN);
            CmcNode *n = new_node(p, N_IF);
            n->test = parse_expr(p, 2);
            expect(p, TK_RPAREN);
            n->cons = parse_stmt(p);
            if (accept(p, TK_ELSE)) n->alt = parse_stmt(p);
            return n;
        }
        case TK_WHILE: {
            next(p);
            expect(p, TK_LPAREN);
            CmcNode *n = new_node(p, N_WHILE);
            n->test = parse_expr(p, 2);
            expect(p, TK_RPAREN);
            n->stmt = parse_stmt(p);
            return n;
        }
        case TK_DO: {
            next(p);
            CmcNode *n = new_node(p, N_DOWHILE);
            n->stmt = parse_stmt(p);
            expect(p, TK_WHILE);
            expect(p, TK_LPAREN);
            n->test = parse_expr(p, 2);
            expect(p, TK_RPAREN);
            accept(p, TK_SEMI);
            return n;
        }
        case TK_FOR: return parse_for(p);
        case TK_RETURN: {
            next(p);
            CmcNode *n = new_node(p, N_RETURN);
            if (!check(p, TK_SEMI) && !check(p, TK_RBRACE) && !check(p, TK_EOF)) n->lhs = parse_expr(p, 2);
            accept(p, TK_SEMI);
            return n;
        }
        case TK_BREAK: { next(p); accept(p, TK_SEMI); return new_node(p, N_BREAK); }
        case TK_CONTINUE: { next(p); accept(p, TK_SEMI); return new_node(p, N_CONTINUE); }
        case TK_THROW: {
            next(p);
            CmcNode *n = new_node(p, N_THROW);
            n->lhs = parse_expr(p, 2);
            accept(p, TK_SEMI);
            return n;
        }
        case TK_TRY: {
            next(p);
            CmcNode *n = new_node(p, N_TRY);
            n->stmt = parse_block(p);
            if (accept(p, TK_CATCH)) {
                if (accept(p, TK_LPAREN)) { /* bind error ident */
                    if (check(p, TK_IDENT)) { n->third = new_node(p, N_IDENT); n->third->name = ident_name(p); }
                    expect(p, TK_RPAREN);
                }
                n->alt = parse_block(p);
            }
            if (accept(p, TK_FINALLY)) n->cons = parse_block(p);
            return n;
        }
        case TK_SWITCH: {
            next(p);
            expect(p, TK_LPAREN);
            CmcNode *n = new_node(p, N_SWITCH);
            n->test = parse_expr(p, 2);
            expect(p, TK_RPAREN);
            expect(p, TK_LBRACE);
            int cap = 4, cnt = 0;
            CmcNode **cases = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
            CmcNode **tests = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap);
            while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                CmcNode *c = new_node(p, N_BLOCK);
                int ccap = 4, ccnt = 0;
                CmcNode **cstmts = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * ccap);
                if (accept(p, TK_CASE)) {
                    tests[cnt] = parse_expr(p, 2);
                    expect(p, TK_COLON);
                } else if (accept(p, TK_DEFAULT)) {
                    tests[cnt] = NULL;
                    expect(p, TK_COLON);
                } else break;
                while (!check(p, TK_CASE) && !check(p, TK_DEFAULT) && !check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                    CmcNode *st = parse_stmt(p);
                    if (ccnt == ccap) { ccap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * ccap); memcpy(np, cstmts, ccnt * sizeof(CmcNode *)); cstmts = np; }
                    cstmts[ccnt++] = st;
                }
                c->stmts = cstmts; c->nstmts = ccnt;
                if (cnt == cap) { cap *= 2; CmcNode **nc = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(nc, cases, cnt * sizeof(CmcNode *)); cases = nc; CmcNode **nt = (CmcNode **)cmc_arena_alloc(p->arena, sizeof(CmcNode *) * cap); memcpy(nt, tests, cnt * sizeof(CmcNode *)); tests = nt; }
                cases[cnt] = c; cnt++;
            }
            expect(p, TK_RBRACE);
            n->cases = cases; n->case_tests = tests; n->ncases = cnt;
            return n;
        }
        case TK_LBRACE: return parse_block(p);
        case TK_SEMI: { next(p); CmcNode *n = new_node(p, N_BLOCK); n->nstmts = 0; return n; }
        default: {
            CmcNode *e = parse_expr(p, 1);   /* allow assignment at statement level */
            accept(p, TK_SEMI);
            CmcNode *n = new_node(p, N_EXPRSTMT);
            n->lhs = e;
            return n;
        }
    }
}

CmcNode *cmc_parse(CmcCtx *ctx, const char *src, size_t len) {
    Parser p;
    memset(&p, 0, sizeof p);
    cmc_lex_init(&p.lx, src, len, ctx->arena);
    p.arena = ctx->arena;
    p.err = 0;
    next(&p);
    CmcNode *prog = new_node(&p, N_PROGRAM);
    int cap = 8, cnt = 0;
    CmcNode **stmts = (CmcNode **)cmc_arena_alloc(ctx->arena, sizeof(CmcNode *) * cap);
    while (!check(&p, TK_EOF)) {
        CmcNode *st = parse_stmt(&p);
        if (cnt == cap) { cap *= 2; CmcNode **np = (CmcNode **)cmc_arena_alloc(ctx->arena, sizeof(CmcNode *) * cap); memcpy(np, stmts, cnt * sizeof(CmcNode *)); stmts = np; }
        stmts[cnt++] = st;
    }
    prog->stmts = stmts; prog->nstmts = cnt;
    if (p.err) ctx->err_count++;
    return prog;
}
