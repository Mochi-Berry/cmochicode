/*
 * emit.c — stage 4 of the cmochicode pipeline: pure C99 emitter.
 *
 * Lowering (ECMA modern -> C99 primitives):
 *   - arrow functions  -> lifted static functions (env struct for captures)
 *   - ?. / ??          -> null checks / conditionals
 *   - destructuring    -> direct field reads (SROA)
 *   - template literals-> concat chain; folded into one printf in console.log
 *   - BigInt 100n      -> int64_t
 *   - console.log      -> puts() (lone string) or printf() (formatted)
 *
 * Memory is 3-tier:
 *   Tier 1: stack (non-escaping local object/array literals)
 *   Tier 2: CMC_ALLOC (escaping; with -DCMC_WITH_MOCHIBERRY -> host GC/arena)
 *   Tier 3: free() before scope exit for non-escaped heap pointers (standalone)
 */
#include "cmochi.h"
#include <stdarg.h>

/* ---------------------- growable string buffer ---------------------- */
typedef struct { char *s; size_t len, cap; } Buf;
static void b_init(Buf *b) { b->cap = 256; b->s = (char *)malloc(b->cap); b->s[0] = 0; b->len = 0; }
static void b_ensure(Buf *b, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->s = (char *)realloc(b->s, b->cap);
    }
}
static void b_put(Buf *b, const char *s) { size_t n = strlen(s); b_ensure(b, n); memcpy(b->s + b->len, s, n + 1); b->len += n; }
static void b_putn(Buf *b, const char *s, size_t n) { b_ensure(b, n); memcpy(b->s + b->len, s, n); b->s[b->len + n] = 0; b->len += n; }
static void b_putf(Buf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    b_put(b, tmp);
}

/* ---------------------- emitter state ---------------------- */
typedef struct { char *code; CmcType type; int escapes; int ptr; const char *struct_name; } EStr;

typedef struct {
    CmcCtx   *ctx;
    CmcEnv    env;
    Buf       out;        /* function bodies / main body        */
    Buf       structs;    /* collected struct definitions       */
    Buf       fn_extra;   /* generated static functions (arrows) */
    Buf       pre;        /* pending pre-lines for the stmt     */
    int       indent;
    int       need_math, need_string, need_stdint;
    int       tmp, fnid;
    CmcNode  *cur_fn;
    /* deferred-type hints for `var x;` (no init): type from first assignment */
    const char *u_names[128];
    CmcType     u_types[128];
    int         u_n;
    /* exception handling (best-effort, no unwinding) */
    int       try_depth;
    const char *last_thrown;
    /* object shorthand methods: field -> lifted static fn name (per shape) */
    const char *m_shape[64]; const char *m_field[64]; const char *m_fn[64];
    CmcNode   *m_node[64];
    int         m_n;
} Emit;

static EStr estr(const char *code, CmcType t, int esc, int ptr, const char *sn) {
    EStr e; e.code = (char *)code; e.type = t; e.escapes = esc; e.ptr = ptr; e.struct_name = sn; return e;
}

static void e_line(Emit *em, const char *s) {
    for (int i = 0; i < em->indent; i++) b_put(&em->out, "    ");
    b_put(&em->out, s);
    b_put(&em->out, "\n");
}
static void e_linef(Emit *em, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    e_line(em, tmp);
}
static void e_preline(Emit *em, const char *s) { if (s) { b_put(&em->pre, s); b_put(&em->pre, "\n"); } }
static void flush_pre(Emit *em) {
    if (em->pre.len) { b_put(&em->out, em->pre.s); em->pre.len = 0; em->pre.s[0] = 0; }
}
static const char *tmp_name(Emit *em, const char *base) {
    char buf[64];
    snprintf(buf, sizeof buf, "__%s_%d", base, em->tmp++);
    return cmc_arena_strndup(em->ctx->arena, buf, strlen(buf));
}
static void c_lit(Buf *b, const char *s) {
    b_put(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"') b_put(b, "\\\"");
        else if (*p == '\\') b_put(b, "\\\\");
        else if (*p == '\n') b_put(b, "\\n");
        else if (*p == '\r') b_put(b, "\\r");
        else if (*p == '\t') b_put(b, "\\t");
        else if (*p < 32 || *p > 126) b_putf(b, "\\x%02x", *p);
        else if (*p == '?') b_put(b, "\\x3f"); /* avoid C99 trigraphs (??= etc) */
        else b_putn(b, (const char *)p, 1);
    }
    b_put(b, "\"");
}

/* ---------------------- coercion ---------------------- */
static void co_num(Emit *em, Buf *b, EStr e) {
    if (e.type == CT_MOCHI_VAL) b_putf(b, "mochi_val_num(%s)", e.code);
    else if (e.type == CT_STR) b_putf(b, "atof(%s)", e.code);
    else if (cmc_is_numeric(e.type)) b_put(b, e.code);
    else b_putf(b, "(double)(%s)", e.code);
}
static void co_int(Buf *b, EStr e) {
    if (e.type == CT_MOCHI_VAL) b_putf(b, "(int)mochi_val_num(%s)", e.code);
    else if (e.type == CT_INT || e.type == CT_INT64) b_put(b, e.code);
    else b_putf(b, "(int)(%s)", e.code);
}
static void co_str(Emit *em, Buf *b, EStr e) {
    if (e.type == CT_STR) b_put(b, e.code);
    else if (e.type == CT_MOCHI_VAL) b_putf(b, "mochi_val_str(%s)", e.code);
    else if (cmc_is_numeric(e.type)) b_putf(b, "mochi_num_to_str(%s)", e.code);
    else if (e.type == CT_BOOL) b_putf(b, "(%s ? \"true\" : \"false\")", e.code);
    else b_put(b, e.code);
}
static void co_bool(Emit *em, Buf *b, EStr e) {
    if (e.type == CT_STR) b_putf(b, "(%s[0] != 0)", e.code);
    else if (e.type == CT_MOCHI_VAL) b_putf(b, "mochi_val_bool(%s)", e.code);
    else if (e.type == CT_ARR) b_putf(b, "((%s)->len > 0)", e.code);
    else if (e.type == CT_OBJ) b_putf(b, "(%s != NULL)", e.code);
    else b_putf(b, "(%s != 0)", e.code);
}
static void co_box(Buf *b, EStr e) {
    if (e.type == CT_MOCHI_VAL) b_put(b, e.code);
    else if (e.type == CT_STR) b_putf(b, "mochi_str(%s)", e.code);
    else if (cmc_is_numeric(e.type)) b_putf(b, "mochi_num((double)(%s))", e.code);
    else if (e.type == CT_BOOL) b_putf(b, "mochi_bool(%s)", e.code);
    else if (e.type == CT_NULL || e.type == CT_UNDEF) b_put(b, "mochi_null()");
    else if (e.type == CT_ARR) b_putf(b, "mochi_arr_box(%s)", e.code);
    else if (e.type == CT_OBJ) b_putf(b, "mochi_obj_box(%s)", e.code);
    else b_putf(b, "mochi_num(0)");
}

/* ---------------------- forward decls ---------------------- */
static EStr emit_expr(Emit *em, CmcNode *n);
static void emit_stmt(Emit *em, CmcNode *n);
static CmcType fn_return_type(CmcNode *fn);
static CmcType infer_param_type_lite(Emit *em, CmcNode *fn, const char *pname);

static void member_base(Emit *em, Buf *b, EStr o) {
    b_put(b, o.code);
    if (o.type == CT_OBJ || o.type == CT_ARR) {
        if (o.ptr) b_put(b, "->"); else b_put(b, ".");
    } else b_put(b, "->");
}

static void num_lit(Buf *b, double v) {
    if (v == (long long)v) {
        long long iv = (long long)v;
        if (iv >= INT32_MIN && iv <= INT32_MAX) b_putf(b, "%d", (int)iv);
        else b_putf(b, "%lldLL", iv);
    } else b_putf(b, "%.17g", v);
}

/* ---------------------- console.log folding ---------------------- */
/* Append a value argument with a ", " separator between values. */
static void farg_push(Buf *fargs, const char *code) {
    if (fargs->len) b_put(fargs, ", ");
    b_put(fargs, code);
}

static void append_print_arg(Emit *em, CmcNode *node, EStr e, Buf *fmt, Buf *fargs) {
    if (node && node->kind == N_STR) {
        for (const char *p = node->str; *p; p++) {
            if (*p == '%') b_put(fmt, "%%");
            else if (*p == '?') b_put(fmt, "\\x3f"); /* avoid C99 trigraphs */
            else b_putn(fmt, p, 1);
        }
        return;
    }
    if (node && node->kind == N_TEMPLATE) {
        for (int i = 0; i < node->nquasis; i++) {
            if (node->quasis[i]) {
                for (const char *p = node->quasis[i]->str; *p; p++) {
                    if (*p == '%') b_put(fmt, "%%");
                    else if (*p == '?') b_put(fmt, "\\x3f");
                    else b_putn(fmt, p, 1);
                }
            }
            if (i < node->nexprs) {
                EStr ev = emit_expr(em, node->exprs[i]);
                if (ev.type == CT_INT) { b_put(fmt, "%d"); farg_push(fargs, ev.code); }
                else if (ev.type == CT_INT64) { b_put(fmt, "%lld"); farg_push(fargs, ev.code); }
                else if (ev.type == CT_DOUBLE) { b_put(fmt, "%g"); farg_push(fargs, ev.code); }
                else if (ev.type == CT_STR) { b_put(fmt, "%s"); farg_push(fargs, ev.code); }
                else if (ev.type == CT_MOCHI_VAL) { b_put(fmt, "%s"); char t[128]; snprintf(t, sizeof t, "mochi_val_str(%s)", ev.code); farg_push(fargs, t); }
                else if (ev.type == CT_BOOL) { b_put(fmt, "%s"); char t[128]; snprintf(t, sizeof t, "(%s ? \"true\" : \"false\")", ev.code); farg_push(fargs, t); }
                else { b_put(fmt, "%s"); Buf s; b_init(&s); co_str(em, &s, ev); farg_push(fargs, s.s); free(s.s); }
            }
        }
        return;
    }
    if (e.type == CT_INT) { b_put(fmt, "%d"); farg_push(fargs, e.code); }
    else if (e.type == CT_INT64) { b_put(fmt, "%lld"); farg_push(fargs, e.code); }
    else if (e.type == CT_DOUBLE) { b_put(fmt, "%g"); farg_push(fargs, e.code); }
    else if (e.type == CT_STR) { b_put(fmt, "%s"); farg_push(fargs, e.code); }
    else if (e.type == CT_BOOL) { b_put(fmt, "%s"); char t[128]; snprintf(t, sizeof t, "(%s ? \"true\" : \"false\")", e.code); farg_push(fargs, t); }
    else if (e.type == CT_MOCHI_VAL) { b_put(fmt, "%s"); char t[128]; snprintf(t, sizeof t, "mochi_val_str(%s)", e.code); farg_push(fargs, t); }
    else if (e.type == CT_NULL || e.type == CT_UNDEF) { b_put(fmt, "null"); }
    else if (e.type == CT_ARR) {
        b_put(fmt, "%s");
        Buf j; b_init(&j);
        b_putf(&j, "mochi_array_inspect(%s)", e.code);
        farg_push(fargs, j.s);
        free(j.s);
    }
    else if (e.type == CT_OBJ && e.struct_name) {
        b_put(fmt, "{ ");
        int first = 1;
        for (int f = 0; f < 64; f++) {
            const char *fn = cmc_struct_field_name_n(e.struct_name, f);
            if (!fn) break;
            const char *ft = cmc_struct_field_type_n(e.struct_name, f);
            if (!first) b_put(fmt, ", ");
            first = 0;
            b_putf(fmt, "%s: ", fn);
            if (strcmp(ft, "const char *") == 0) { b_put(fmt, "'%s'"); char t[128]; snprintf(t, sizeof t, "%s->%s", e.code, fn); farg_push(fargs, t); }
            else if (strcmp(ft, "int") == 0 || strcmp(ft, "int64_t") == 0) {
                if (cmc_struct_field_kind(e.struct_name, fn) == CT_BOOL) {
                    b_put(fmt, "%s"); char t[128]; snprintf(t, sizeof t, "(%s->%s ? \"true\" : \"false\")", e.code, fn); farg_push(fargs, t);
                } else { b_put(fmt, "%lld"); char t[128]; snprintf(t, sizeof t, "(long long)%s->%s", e.code, fn); farg_push(fargs, t); }
            }
            else if (strcmp(ft, "double") == 0) { b_put(fmt, "%g"); char t[128]; snprintf(t, sizeof t, "%s->%s", e.code, fn); farg_push(fargs, t); }
            else { b_put(fmt, "%s"); char t[128]; snprintf(t, sizeof t, "mochi_val_str(%s->%s)", e.code, fn); farg_push(fargs, t); }
        }
        b_put(fmt, " }");
    }
    else { b_put(fmt, "%s"); Buf s; b_init(&s); co_str(em, &s, e); farg_push(fargs, s.s); free(s.s); }
}

static void emit_console(Emit *em, CmcNode **args, int nargs, int to_stderr) {
    if (nargs == 1 && args[0]->kind == N_STR) {
        Buf lit; b_init(&lit);
        c_lit(&lit, args[0]->str);
        if (to_stderr) e_linef(em, "(fputs(%s, stderr));", lit.s);
        else e_linef(em, "(puts(%s));", lit.s);
        free(lit.s);
        return;
    }
    if (nargs == 0) {
        e_line(em, to_stderr ? "(fputs(\"\\n\", stderr));" : "(puts(\"\"));");
        return;
    }
    Buf fmt; b_init(&fmt);
    Buf fargs; b_init(&fargs);
    for (int i = 0; i < nargs; i++) {
        /* console.log joins arguments with a single space (like node) */
        if (i > 0 && (fmt.len == 0 || fmt.s[fmt.len - 1] != ' ')) {
            b_put(&fmt, " ");
        }
        EStr e = emit_expr(em, args[i]);
        append_print_arg(em, args[i], e, &fmt, &fargs);
    }
    flush_pre(em);
    if (fargs.len) e_linef(em, "(fprintf(%s, \"%s\\n\", %s));", to_stderr ? "stderr" : "stdout", fmt.s, fargs.s);
    else e_linef(em, "(fputs(\"%s\\n\", %s));", fmt.s, to_stderr ? "stderr" : "stdout");
    free(fmt.s); free(fargs.s);
}

/* ---------------------- expressions ---------------------- */
static EStr emit_literal(Emit *em, CmcNode *n) {
    Buf b; b_init(&b);
    switch (n->kind) {
        case N_NUM:
            if (n->num == (long long)n->num) {
                long long iv = (long long)n->num;
                if (iv >= INT32_MIN && iv <= INT32_MAX) { num_lit(&b, iv); return estr(b.s, CT_INT, 0, 0, NULL); }
                num_lit(&b, iv);
                return estr(b.s, CT_INT64, 0, 0, NULL);
            }
            num_lit(&b, n->num);
            return estr(b.s, CT_DOUBLE, 0, 0, NULL);
        case N_BIGINT: b_putf(&b, "%lldLL", n->bigint); return estr(b.s, CT_INT64, 0, 0, NULL);
        case N_STR: c_lit(&b, n->str); return estr(b.s, CT_STR, 0, 1, NULL);
        case N_BOOL: return estr(n->boolval ? "1" : "0", CT_BOOL, 0, 0, NULL);
        case N_NULL: return estr("NULL", CT_NULL, 0, 1, NULL);
        case N_UNDEF: return estr("0", CT_UNDEF, 0, 0, NULL);
        default: return estr("0", CT_INT, 0, 0, NULL);
    }
}

static EStr emit_ident(Emit *em, CmcNode *n) {
    CmcSym *s = cmc_env_lookup(&em->env, n->name);
    if (s) {
        int ptr = s->heap || s->type == CT_STR || s->type == CT_OBJ || s->type == CT_ARR;
        return estr(s->cname, s->type, s->heap, ptr, s->struct_name);
    }
    return estr(n->name, CT_UNKNOWN, 0, 0, NULL);
}

static EStr emit_member(Emit *em, CmcNode *n) {
    EStr o = emit_expr(em, n->obj);
    if (n->computed) {
        EStr idx = emit_expr(em, n->prop);
        Buf b; b_init(&b);
        if (o.type == CT_ARR) {
            b_putf(&b, "mochi_array_get(%s, %s)", o.code, idx.code);
            return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
        }
        if (o.type == CT_STR) {
            b_putf(&b, "mochi_char_at(%s, %s)", o.code, idx.code);
            return estr(b.s, CT_STR, 0, 1, NULL);
        }
        b_putf(&b, "%s", o.code);
        return estr(b.s, CT_UNKNOWN, o.escapes, o.ptr, NULL);
    }
    const char *prop = n->prop ? n->prop->name : "";
    if (o.type == CT_ARR && strcmp(prop, "length") == 0) {
        Buf b; b_init(&b);
        b_putf(&b, "(int)(%s%slen)", o.code, o.ptr ? "->" : ".");
        return estr(b.s, CT_INT, 0, 0, NULL);
    }
    if (o.type == CT_STR && strcmp(prop, "length") == 0) {
        em->need_string = 1;
        Buf b; b_init(&b);
        b_putf(&b, "(int)strlen(%s)", o.code);
        return estr(b.s, CT_INT, 0, 0, NULL);
    }
    if (o.type == CT_OBJ) {
        Buf b; b_init(&b);
        member_base(em, &b, o);
        b_put(&b, prop);
        const char *ft = o.struct_name ? cmc_struct_field_type(o.struct_name, prop) : "double";
        if (!ft) ft = "double"; /* missing property -> 0 (undefined) */
        CmcType t = CT_UNKNOWN;
        if (o.struct_name && cmc_struct_field_kind(o.struct_name, prop) == CT_BOOL) t = CT_BOOL;
        else if (strcmp(ft, "int") == 0) t = CT_INT;
        else if (strcmp(ft, "int64_t") == 0) t = CT_INT64;
        else if (strcmp(ft, "double") == 0) t = CT_DOUBLE;
        else if (strcmp(ft, "const char *") == 0) t = CT_STR;
        else if (strcmp(ft, "mochi_array *") == 0) t = CT_ARR;
        else t = CT_UNKNOWN;
        /* optional chaining `user?.id`: yield 0/NULL instead of crashing */
        if (n->optional) {
            Buf w; b_init(&w);
            if (t == CT_STR) b_putf(&w, "(%s != NULL ? %s : NULL)", o.code, b.s);
            else if (t == CT_ARR) b_putf(&w, "(%s != NULL ? %s : NULL)", o.code, b.s);
            else b_putf(&w, "(%s != NULL ? %s : 0)", o.code, b.s);
            free(b.s);
            return estr(w.s, t, 0, t == CT_STR || t == CT_ARR ? 1 : 0, NULL);
        }
        return estr(b.s, t, 0, 0, NULL);
    }
    Buf b; b_init(&b);
    member_base(em, &b, o);
    b_put(&b, prop);
    return estr(b.s, CT_UNKNOWN, o.escapes, o.ptr, NULL);
}

static EStr emit_bin(Emit *em, CmcNode *n) {
    EStr l = emit_expr(em, n->lhs);
    EStr r = emit_expr(em, n->rhs);
    Buf b; b_init(&b);
    switch (n->op) {
        case TK_PLUS: {
            if (l.type == CT_STR || r.type == CT_STR) {
                Buf ls, rs; b_init(&ls); b_init(&rs);
                co_str(em, &ls, l); co_str(em, &rs, r);
                b_putf(&b, "mochi_concat(%s, %s)", ls.s, rs.s);
                free(ls.s); free(rs.s);
                return estr(b.s, CT_STR, 1, 1, NULL);
            }
            Buf ln, rn; b_init(&ln); b_init(&rn);
            co_num(em, &ln, l); co_num(em, &rn, r);
            b_putf(&b, "(%s + %s)", ln.s, rn.s);
            free(ln.s); free(rn.s);
            CmcType t = (l.type == CT_DOUBLE || r.type == CT_DOUBLE) ? CT_DOUBLE :
                        (l.type == CT_INT64 || r.type == CT_INT64) ? CT_INT64 : CT_INT;
            return estr(b.s, t, 0, 0, NULL);
        }
        case TK_MINUS: case TK_STAR: {
            Buf ln, rn; b_init(&ln); b_init(&rn);
            co_num(em, &ln, l); co_num(em, &rn, r);
            b_putf(&b, "(%s %s %s)", ln.s, n->op == TK_MINUS ? "-" : "*", rn.s);
            free(ln.s); free(rn.s);
            return estr(b.s, CT_DOUBLE, 0, 0, NULL);
        }
        case TK_SLASH: {
            Buf ln, rn; b_init(&ln); b_init(&rn);
            co_num(em, &ln, l); co_num(em, &rn, r);
            /* JS division is always floating point: 7 / 2 == 3.5 */
            b_putf(&b, "((double)(%s) / (double)(%s))", ln.s, rn.s);
            free(ln.s); free(rn.s);
            return estr(b.s, CT_DOUBLE, 0, 0, NULL);
        }
        case TK_PERCENT: {
            if (l.type == CT_INT && r.type == CT_INT) { b_putf(&b, "(%s %% %s)", l.code, r.code); return estr(b.s, CT_INT, 0, 0, NULL); }
            b_putf(&b, "fmod(%s, %s)", l.code, r.code);
            return estr(b.s, CT_DOUBLE, 0, 0, NULL);
        }
        case TK_STARSTAR: {
            b_putf(&b, "pow(%s, %s)", l.code, r.code);
            return estr(b.s, CT_DOUBLE, 0, 0, NULL);
        }
        case TK_EQEQ: case TK_EQEQEQ: case TK_NE: case TK_NEEQ:
        case TK_LT: case TK_LE: case TK_GT: case TK_GE: {
            const char *op = (n->op == TK_EQEQ || n->op == TK_EQEQEQ) ? "==" :
                             (n->op == TK_NE || n->op == TK_NEEQ) ? "!=" :
                             n->op == TK_LT ? "<" : n->op == TK_LE ? "<=" :
                             n->op == TK_GT ? ">" : ">=";
            if (l.type == CT_STR && r.type == CT_STR) {
                em->need_string = 1;
                if (n->op == TK_EQEQ || n->op == TK_EQEQEQ) b_putf(&b, "(strcmp(%s, %s) == 0)", l.code, r.code);
                else if (n->op == TK_NE || n->op == TK_NEEQ) b_putf(&b, "(strcmp(%s, %s) != 0)", l.code, r.code);
                else b_putf(&b, "(strcmp(%s, %s) %s 0)", l.code, r.code, op);
            } else {
                Buf ln, rn; b_init(&ln); b_init(&rn);
                co_num(em, &ln, l); co_num(em, &rn, r);
                b_putf(&b, "(%s %s %s)", ln.s, op, rn.s);
                free(ln.s); free(rn.s);
            }
            return estr(b.s, CT_BOOL, 0, 0, NULL);
        }
        case TK_AND: case TK_OR: case TK_XOR:
            b_putf(&b, "(%s %s %s)", l.code, n->op == TK_AND ? "&" : n->op == TK_OR ? "|" : "^", r.code);
            return estr(b.s, CT_INT, 0, 0, NULL);
        case TK_SHL: b_putf(&b, "(%s << %s)", l.code, r.code); return estr(b.s, CT_INT, 0, 0, NULL);
        case TK_SHR: b_putf(&b, "(%s >> %s)", l.code, r.code); return estr(b.s, CT_INT, 0, 0, NULL);
        case TK_SHRU: b_putf(&b, "((unsigned)%s >> %s)", l.code, r.code); return estr(b.s, CT_INT, 0, 0, NULL);
        case TK_ANDAND: /* logical && — C already short-circuits */
            b_putf(&b, "((%s) && (%s))", l.code, r.code);
            return estr(b.s, CT_BOOL, 0, 0, NULL);
        case TK_OROR: /* logical || */
            b_putf(&b, "((%s) || (%s))", l.code, r.code);
            return estr(b.s, CT_BOOL, 0, 0, NULL);
        case TK_NULLISH: {
            /* `a ?? rhs` — if `a` is a nullish-guarded param, use its has_X;
             * if it's a pointer type, test NULL; numbers are never nullish. */
            const char *has = NULL;
            if (n->lhs && n->lhs->kind == N_IDENT) {
                CmcSym *s = cmc_env_lookup(&em->env, n->lhs->name);
                if (s && s->has_name) has = s->has_name;
            }
            if (has) b_putf(&b, "(%s ? (%s) : (%s))", has, l.code, r.code);
            else if (l.type == CT_OBJ || l.type == CT_STR || l.type == CT_ARR ||
                     l.type == CT_NULL || l.type == CT_UNDEF)
                b_putf(&b, "((%s) != NULL ? (%s) : (%s))", l.code, l.code, r.code);
            else b_put(&b, l.code);
            return estr(b.s, l.type, 0, 0, NULL);
        }
        default: b_putf(&b, "(%s %s %s)", l.code, r.code);
            return estr(b.s, CT_UNKNOWN, 0, 0, NULL);
    }
}

static EStr emit_unary(Emit *em, CmcNode *n) {
    if (n->op == TK_TYPEOF) {
        EStr a = emit_expr(em, n->lhs);
        const char *t = "number";
        if (a.type == CT_STR) t = "string";
        else if (a.type == CT_OBJ || a.type == CT_ARR || a.type == CT_NULL) t = "object";
        else if (a.type == CT_UNDEF) t = "undefined";
        else if (a.type == CT_BOOL) t = "boolean";
        Buf b; b_init(&b); c_lit(&b, t);
        return estr(b.s, CT_STR, 0, 1, NULL);
    }
    EStr a = emit_expr(em, n->lhs);
    Buf b; b_init(&b);
    switch (n->op) {
        case TK_NOT: { Buf c; b_init(&c); co_bool(em, &c, a); b_putf(&b, "(!(%s))", c.s); free(c.s); return estr(b.s, CT_BOOL, 0, 0, NULL); }
        case TK_TILDE: b_putf(&b, "(~(%s))", a.code); return estr(b.s, CT_INT, 0, 0, NULL);
        case TK_MINUS: { Buf c; b_init(&c); co_num(em, &c, a); b_putf(&b, "(-(%s))", c.s); free(c.s); return estr(b.s, a.type == CT_DOUBLE ? CT_DOUBLE : CT_INT, 0, 0, NULL); }
        case TK_PLUS: { Buf c; b_init(&c); co_num(em, &c, a); b_putf(&b, "(+(%s))", c.s); free(c.s); return estr(b.s, CT_DOUBLE, 0, 0, NULL); }
        case TK_DELETE: b_putf(&b, "(0)"); return estr(b.s, CT_BOOL, 0, 0, NULL);
        case TK_VOID: b_putf(&b, "(0)"); return estr(b.s, CT_UNDEF, 0, 0, NULL);
        default: b_put(&b, a.code); return estr(b.s, a.type, a.escapes, a.ptr, NULL);
    }
}

static EStr emit_assign(Emit *em, CmcNode *n) {
    EStr r = emit_expr(em, n->rhs);
    Buf b; b_init(&b);
    if (n->lhs->kind == N_IDENT) {
        CmcSym *s = cmc_env_lookup(&em->env, n->lhs->name);
        const char *target = s ? s->cname : n->lhs->name;
        CmcType t = s ? s->type : r.type;
        if (t == CT_UNKNOWN) t = r.type;
        if (n->op == TK_EQ) {
            Buf rv; b_init(&rv);
            if (t == CT_STR) co_str(em, &rv, r);
            else if (t == CT_BOOL) co_bool(em, &rv, r);
            else if (t == CT_MOCHI_VAL) b_put(&rv, r.code);
            else if (cmc_is_numeric(t)) co_num(em, &rv, r);
            else b_put(&rv, r.code);
            b_putf(&b, "%s = %s", target, rv.s);
            free(rv.s);
        } else {
            const char *cop = n->op == TK_PLUSEQ ? "+" : n->op == TK_MINUSEQ ? "-" :
                              n->op == TK_STAREQ ? "*" : n->op == TK_ANDEQ ? "&" :
                              n->op == TK_OREQ ? "|" : n->op == TK_XOREQ ? "^" : "+";
            if (t == CT_STR && n->op == TK_PLUSEQ) {
                Buf rv; b_init(&rv); co_str(em, &rv, r);
                b_putf(&b, "%s = mochi_concat(%s, %s)", target, target, rv.s);
                free(rv.s);
            } else {
            Buf rv; b_init(&rv);
            if (t == CT_STR) co_str(em, &rv, r); else co_num(em, &rv, r);
            if (n->op == TK_OROREQ)      { b_putf(&b, "%s = ((%s) ? (%s) : (%s))", target, target, target, rv.s); }
            else if (n->op == TK_ANDANDEQ){ b_putf(&b, "%s = ((%s) ? (%s) : (%s))", target, target, rv.s, target); }
            else if (n->op == TK_NULLISHEQ) {
                if (t == CT_STR || t == CT_OBJ || t == CT_ARR || t == CT_NULL || t == CT_MOCHI_VAL || t == CT_UNDEF)
                    b_putf(&b, "%s = ((%s) != NULL ? (%s) : (%s))", target, target, target, rv.s);
                else
                    b_putf(&b, "(void)(%s)", target);   /* numbers are never nullish */
            }
            else if (n->op == TK_SLASHEQ) b_putf(&b, "%s = (%s / %s)", target, target, rv.s);
            else if (n->op == TK_PERCENTEQ) b_putf(&b, "%s = fmod(%s, %s)", target, target, rv.s);
            else b_putf(&b, "%s %s= %s", target, cop, rv.s);
            free(rv.s);
            }
        }
        return estr(b.s, t, r.escapes, r.ptr, s ? s->struct_name : NULL);
    }
    if (n->lhs->kind == N_MEMBER) {
        CmcNode *m = n->lhs;
        EStr o = emit_expr(em, m->obj);
        if (m->computed) {
            EStr idx = emit_expr(em, m->prop);
            Buf v; b_init(&v); co_box(&v, r);
            b_putf(&b, "mochi_array_set(%s, %s, %s)", o.code, idx.code, v.s);
            free(v.s);
            return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
        }
        const char *prop = m->prop->name;
        Buf base; b_init(&base); member_base(em, &base, o);
        Buf rv; b_init(&rv);
        const char *ft = o.struct_name ? cmc_struct_field_type(o.struct_name, prop) : NULL;
        if (!ft && o.struct_name) {
            /* JS allows point.label = "P" on a shape that only had {x, y}:
             * merge the field into the registered shape so the struct def
             * (emitted at assembly) grows to include it. */
            CmcType vt = r.type;
            if (vt == CT_UNKNOWN) vt = CT_DOUBLE;
            cmc_shape_add_field(o.struct_name, prop, vt);
            ft = cmc_struct_field_type(o.struct_name, prop);
        }
        if (ft && strcmp(ft, "const char *") == 0) co_str(em, &rv, r);
        else if (ft && strcmp(ft, "int") == 0) co_int(&rv, r);
        else if (ft && strcmp(ft, "int64_t") == 0) { if (r.type == CT_INT64) b_put(&rv, r.code); else b_putf(&rv, "(int64_t)(%s)", r.code); }
        else if (ft && strcmp(ft, "double") == 0) co_num(em, &rv, r);
        else if (r.type == CT_STR) co_str(em, &rv, r);
        else if (cmc_is_numeric(r.type)) co_num(em, &rv, r);
        else b_put(&rv, r.code);
        b_putf(&b, "%s%s = %s", base.s, prop, rv.s);
        free(base.s); free(rv.s);
        return estr(b.s, CT_UNKNOWN, 0, 0, NULL);
    }
    b_put(&b, r.code);
    return estr(b.s, r.type, r.escapes, r.ptr, NULL);
}

static EStr emit_update(Emit *em, CmcNode *n) {
    EStr a = emit_expr(em, n->lhs);
    Buf b; b_init(&b);
    if (n->lhs->kind == N_IDENT) {
        CmcSym *s = cmc_env_lookup(&em->env, n->lhs->name);
        const char *target = s ? s->cname : n->lhs->name;
        int prefix = (n->third != NULL);
        const char *op = n->op > 0 ? "+" : "-";
        if (prefix) {
            b_putf(&b, "(%s %s= 1)", target, op);
            return estr(b.s, s ? s->type : CT_INT, 0, 0, NULL);
        }
        const char *old = tmp_name(em, "old");
        Buf pre; b_init(&pre);
        b_putf(&pre, "%s %s = %s;", cmc_type_name(s ? s->type : CT_INT), old, target);
        b_putf(&pre, "%s %s= 1;", target, op);
        e_preline(em, pre.s);
        free(pre.s);
        return estr(old, s ? s->type : CT_INT, 0, 0, NULL);
    }
    b_put(&b, a.code);
    return estr(b.s, a.type, 0, 0, NULL);
}

static EStr emit_cond(Emit *em, CmcNode *n) {
    EStr c = emit_expr(em, n->lhs);
    EStr a = emit_expr(em, n->rhs);
    EStr b2 = emit_expr(em, n->third);
    Buf cc, aa, bb; b_init(&cc); b_init(&aa); b_init(&bb);
    co_bool(em, &cc, c);
    CmcType t = (a.type == b2.type) ? a.type :
                (a.type == CT_DOUBLE || b2.type == CT_DOUBLE) ? CT_DOUBLE :
                (a.type == CT_STR || b2.type == CT_STR) ? CT_STR :
                (a.type == CT_INT64 || b2.type == CT_INT64) ? CT_INT64 : CT_INT;
    if (t == CT_STR) { co_str(em, &aa, a); co_str(em, &bb, b2); }
    else if (t == CT_BOOL) { co_bool(em, &aa, a); co_bool(em, &bb, b2); }
    else if (cmc_is_numeric(t)) { co_num(em, &aa, a); co_num(em, &bb, b2); }
    else { b_put(&aa, a.code); b_put(&bb, b2.code); }
    Buf b; b_init(&b);
    b_putf(&b, "(%s ? %s : %s)", cc.s, aa.s, bb.s);
    free(cc.s); free(aa.s); free(bb.s);
    return estr(b.s, t, 0, t == CT_STR ? 1 : 0, a.struct_name ? a.struct_name : b2.struct_name);
}

static EStr emit_array(Emit *em, CmcNode *n) {
    const char *arr = tmp_name(em, "arr");
    Buf alloc; b_init(&alloc);
    b_putf(&alloc, "mochi_array *%s = mochi_array_new();", arr);
    e_preline(em, alloc.s);
    free(alloc.s);
    for (int i = 0; i < n->nelems; i++) {
        EStr e = emit_expr(em, n->elems[i]);
        Buf v; b_init(&v);
        co_box(&v, e);
        Buf push; b_init(&push);
        b_putf(&push, "mochi_array_push(%s, %s);", arr, v.s);
        e_preline(em, push.s);
        free(push.s); free(v.s);
    }
    return estr(arr, CT_ARR, 1, 1, NULL);
}

static EStr emit_object(Emit *em, CmcNode *n) {
    int np = n->nprops;
    /* emit all values first so spread sources reveal their struct_name */
    EStr *vals = (EStr *)malloc(sizeof(EStr) * (np ? np : 1));
    for (int i = 0; i < np; i++) vals[i] = (n->props[i].key == NULL && !n->props[i].spread)
        ? estr("0", CT_UNKNOWN, 0, 0, NULL)
        : (n->props[i].value && (n->props[i].value->kind == N_ARROW || n->props[i].value->kind == N_FUNC))
        ? estr("0", CT_UNKNOWN, 0, 0, NULL) /* method: lifted in the prop loop */
        : emit_expr(em, n->props[i].value);
    /* register the base shape (spread props skipped inside register_shape) */
    const char *sn = cmc_register_shape(em->ctx->arena, n);
    /* merge spread sources' fields into the shape, in prop order */
    for (int i = 0; i < np; i++) {
        CmcProp *pr = &n->props[i];
        if (!pr->spread) continue;
        const char *ss = vals[i].struct_name;
        if (!ss) continue; /* unknown source type: cannot merge */
        for (int f = 0; f < 64; f++) {
            const char *fn = cmc_struct_field_name_n(ss, f);
            if (!fn) break;
            if (cmc_struct_field_type(sn, fn) == NULL)
                cmc_shape_add_field(sn, fn, cmc_struct_field_kind(ss, fn));
        }
    }
    /* reorder fields to JS insertion order (spread sources first, literal last) */
    {
        const char *order[64]; int on = 0;
        for (int i = 0; i < np && on < 64; i++) {
            CmcProp *pr = &n->props[i];
            if (pr->spread) {
                const char *ss = vals[i].struct_name;
                if (!ss) continue;
                for (int f = 0; f < 64 && on < 64; f++) {
                    const char *fn = cmc_struct_field_name_n(ss, f);
                    if (!fn) break;
                    int dup = 0;
                    for (int d = 0; d < on; d++) if (strcmp(order[d], fn) == 0) { dup = 1; break; }
                    if (!dup) order[on++] = fn;
                }
            } else if (pr->key) {
                int dup = 0;
                for (int d = 0; d < on; d++) if (strcmp(order[d], pr->key) == 0) { dup = 1; break; }
                if (!dup) order[on++] = pr->key;
            }
        }
        if (on) cmc_shape_reorder(sn, order, on);
    }
    const char *obj = tmp_name(em, "obj");
    Buf alloc; b_init(&alloc);
    b_putf(&alloc, "struct %s *%s = (struct %s *)CMC_ALLOC(sizeof(struct %s));", sn, obj, sn, sn);
    e_preline(em, alloc.s);
    free(alloc.s);
    /* flush pending pre-lines now: a lifted method fn must not capture them */
    flush_pre(em);
    /* copy spread source fields, then literal props (later wins) */
    for (int i = 0; i < np; i++) {
        CmcProp *pr = &n->props[i];
        if (pr->spread) {
            const char *ss = vals[i].struct_name;
            if (!ss) continue;
            for (int f = 0; f < 64; f++) {
                const char *fn = cmc_struct_field_name_n(ss, f);
                if (!fn) break;
                const char *ft = cmc_struct_field_type(ss, fn);
                Buf asg; b_init(&asg);
                if (ft && strcmp(ft, "const char *") == 0)
                    b_putf(&asg, "%s->%s = %s->%s;", obj, fn, vals[i].code, fn);
                else
                    b_putf(&asg, "%s->%s = %s->%s;", obj, fn, vals[i].code, fn);
                e_preline(em, asg.s);
                free(asg.s);
            }
            continue;
        }
        if (pr->key == NULL) continue;
        if (pr->value && (pr->value->kind == N_ARROW || pr->value->kind == N_FUNC)) {
            /* shorthand method: lift to a static fn, record the inline mapping */
            EStr fe = emit_expr(em, pr->value);
            if (em->m_n < 64) {
                em->m_shape[em->m_n] = sn;
                em->m_field[em->m_n] = pr->key;
                em->m_fn[em->m_n] = fe.code;
                em->m_node[em->m_n] = pr->value;
                em->m_n++;
            }
            continue; /* no struct field assignment; calls are inlined */
        }
        Buf v; b_init(&v);
        const char *ft = cmc_struct_field_type(sn, pr->key);
        if (!ft) ft = "double";
        if (strcmp(ft, "const char *") == 0) co_str(em, &v, vals[i]);
        else if (strcmp(ft, "int") == 0) co_int(&v, vals[i]);
        else if (strcmp(ft, "int64_t") == 0) { if (vals[i].type == CT_INT64) b_put(&v, vals[i].code); else b_putf(&v, "(int64_t)(%s)", vals[i].code); }
        else if (strcmp(ft, "double") == 0) co_num(em, &v, vals[i]);
        else if (strcmp(ft, "mochi_array *") == 0) b_putf(&v, "(%s)", vals[i].code);
        else b_put(&v, vals[i].code);
        Buf asg; b_init(&asg);
        b_putf(&asg, "%s->%s = %s;", obj, pr->key, v.s);
        e_preline(em, asg.s);
        free(asg.s); free(v.s);
    }
    free(vals);
    return estr(obj, CT_OBJ, 1, 1, sn);
}

static EStr emit_template(Emit *em, CmcNode *n) {
    Buf code; b_init(&code);
    Buf lit; b_init(&lit);
    c_lit(&lit, n->quasis[0]->str);
    b_put(&code, lit.s);
    free(lit.s);
    for (int i = 0; i < n->nexprs; i++) {
        EStr e = emit_expr(em, n->exprs[i]);
        Buf s; b_init(&s); co_str(em, &s, e);
        Buf cur; b_init(&cur);
        b_putf(&cur, "mochi_concat(%s, %s)", code.s, s.s);
        free(code.s); code = cur;
        free(s.s);
        if (i + 1 < n->nquasis) {
            Buf q; b_init(&q); c_lit(&q, n->quasis[i + 1]->str);
            Buf cur2; b_init(&cur2);
            b_putf(&cur2, "mochi_concat(%s, %s)", code.s, q.s);
            free(code.s); code = cur2;
            free(q.s);
        }
    }
    return estr(code.s, CT_STR, 1, 1, NULL);
}

static EStr emit_arrow(Emit *em, CmcNode *n);
static EStr emit_call(Emit *em, CmcNode *n);

static EStr emit_arrow(Emit *em, CmcNode *n) {
    char fnbuf[64];
    snprintf(fnbuf, sizeof fnbuf, "__cmc_fn_%d", em->fnid++);
    const char *fnname = cmc_arena_strndup(em->ctx->arena, fnbuf, strlen(fnbuf));
    int np = n->nparams ? n->nparams : 0;
    CmcType *pt = (CmcType *)malloc(sizeof(CmcType) * (np ? np : 1));
    /* resolve each param's name + whether it has a default */
    const char **pname = (const char **)malloc(sizeof(char *) * (np ? np : 1));
    CmcNode **pdef = (CmcNode **)malloc(sizeof(CmcNode *) * (np ? np : 1));
    for (int i = 0; i < np; i++) {
        if (n->params[i]->kind == N_IDENT) {
            pname[i] = n->params[i]->name; pdef[i] = NULL;
            pt[i] = infer_param_type_lite(em, n, pname[i]);
        } else if (n->params[i]->kind == N_ASSIGN && n->params[i]->lhs->kind == N_IDENT) {
            pname[i] = n->params[i]->lhs->name; pdef[i] = n->params[i]->rhs;
            pt[i] = cmc_type_of(em->ctx, &em->env, pdef[i]);
            if (pt[i] == CT_UNKNOWN || pt[i] == CT_VOID) pt[i] = CT_DOUBLE;
        } else { pname[i] = "p"; pdef[i] = NULL; pt[i] = CT_DOUBLE; }
    }
    CmcType ret = n->is_expr_body ? cmc_type_of(em->ctx, &em->env, n->body) : CT_INT;
    if (ret == CT_UNKNOWN || ret == CT_VOID) ret = CT_INT;

    Buf save = em->out;
    Buf body; b_init(&body);
    em->out = body;
    cmc_env_push(&em->env, em->ctx->arena);
    /* signature: bool has_X, <type> X for EVERY param (nullish guard). */
    Buf sig; b_init(&sig);
    b_putf(&sig, "static %s %s(", cmc_type_name(ret), fnname);
    for (int i = 0; i < np; i++) {
        char hasbuf[64];
        snprintf(hasbuf, sizeof hasbuf, "has_%s", pname[i]);
        const char *hasname = cmc_arena_strndup(em->ctx->arena, hasbuf, strlen(hasbuf));
        if (i) b_put(&sig, ", ");
        b_putf(&sig, "int %s, %s %s", hasname, cmc_type_name(pt[i]), pname[i]);
        CmcSym *ps = cmc_env_decl(&em->env, pname[i], pt[i], SYM_PARAM);
        ps->has_name = hasname;
        ps->func_node = n;
        CmcSym *hs = cmc_env_decl(&em->env, hasname, CT_BOOL, SYM_PARAM);
        hs->has_name = hasname;
    }
    b_put(&sig, ") {");
    e_line(em, sig.s);
    free(sig.s);
    em->indent++;
    /* default-value resets: if (!has_X) X = default; */
    for (int i = 0; i < np; i++) {
        if (!pdef[i]) continue;
        Buf dv; b_init(&dv);
        EStr de = emit_expr(em, pdef[i]);
        if (pt[i] == CT_STR) co_str(em, &dv, de);
        else if (cmc_is_numeric(pt[i])) co_num(em, &dv, de);
        else b_put(&dv, de.code);
        e_linef(em, "if (!has_%s) %s = %s;", pname[i], pname[i], dv.s);
        free(dv.s);
    }
    if (n->is_expr_body) {
        EStr e = emit_expr(em, n->body);
        Buf r; b_init(&r);
        if (ret == CT_STR) co_str(em, &r, e);
        else if (cmc_is_numeric(ret)) co_num(em, &r, e);
        else b_put(&r, e.code);
        e_linef(em, "return %s;", r.s);
        free(r.s);
    } else {
        emit_stmt(em, n->body);
    }
    em->indent--;
    e_line(em, "}");
    cmc_env_pop(&em->env);
    em->out = save;
    if (!em->fn_extra.s) b_init(&em->fn_extra);
    b_put(&em->fn_extra, body.s);
    b_put(&em->fn_extra, "\n");
    free(body.s);
    free(pt); free(pname); free(pdef);
    return estr(fnname, CT_UNKNOWN, 0, 0, NULL);
}

/* nullish-ness guard for a call argument: writes a C int expression that is
 * 1 when the argument is definitely non-nullish, 0 when null/undefined. */
static void arg_has(Emit *em, CmcNode *arg, Buf *out)
{
    if (!arg) { b_put(out, "0"); return; }
    switch (arg->kind) {
        case N_NULL: case N_UNDEF: b_put(out, "0"); return;
        case N_IDENT: {
            CmcSym *s = cmc_env_lookup(&em->env, arg->name);
            b_put(out, (s && s->has_name) ? s->has_name : "1");
            return;
        }
        case N_MEMBER:
            if (arg->optional && arg->obj) {
                EStr o = emit_expr(em, arg->obj);
                b_putf(out, "(%s != NULL)", o.code);
                return;
            }
            b_put(out, "1"); return;
        default: b_put(out, "1"); return;
    }
}

/* Higher-order array methods (map/filter/reduce/forEach) with an inline arrow.
   The callback is not a C function pointer — the arrow body is INLINED into a
   C loop, so no closure/indirect-call machinery is needed. Only simple arrows
   with a single numeric (or two for reduce) ident parameter are supported;
   anything else degrades to a no-op `0` so the generated C still compiles. */
static EStr emit_arr_higher(Emit *em, CmcNode *n, const char *prop, EStr o) {
    if (n->nargs < 1 || n->args[0]->kind != N_ARROW) return estr("0", CT_INT, 0, 0, NULL);
    CmcNode *fn = n->args[0];
    int np = fn->nparams ? fn->nparams : 0;
    if (np < 1 || fn->params[0]->kind != N_IDENT) return estr("0", CT_INT, 0, 0, NULL);
    const char *pname = fn->params[0]->name;
    const char *pname2 = (np > 1 && fn->params[1]->kind == N_IDENT) ? fn->params[1]->name : NULL;
    if (strcmp(prop, "reduce") == 0 && !pname2) return estr("0", CT_INT, 0, 0, NULL);

    char li[32]; snprintf(li, sizeof li, "__i%d", em->tmp); em->tmp++;
    char ind[16]; for (int i = 0; i < em->indent; i++) ind[i] = ' '; ind[em->indent] = 0;
    Buf out_loop; b_init(&out_loop);
    const char *result_name = NULL;

    cmc_env_push(&em->env, em->ctx->arena);
    cmc_env_decl(&em->env, pname, CT_DOUBLE, SYM_LOCAL);
    if (pname2) cmc_env_decl(&em->env, pname2, CT_DOUBLE, SYM_LOCAL);

    CmcNode *body_node = fn->body;
    if (body_node && body_node->kind == N_EXPRSTMT && body_node->lhs) body_node = body_node->lhs;

    if (strcmp(prop, "forEach") == 0) {
        b_putf(&out_loop, "%sfor (size_t %s = 0; %s < (%s)->len; %s++) {\n", ind, li, li, o.code, li);
        b_putf(&out_loop, "%s    double %s = mochi_val_num((%s)->items[%s]);\n", ind, pname, o.code, li);
        Buf bodys; b_init(&bodys);
        if (body_node) {
            Buf saved = em->out; em->out = bodys;
            EStr be = emit_expr(em, body_node);
            em->out = saved;
            if (be.type != CT_VOID && be.type != CT_UNDEF) b_putf(&bodys, "%s    (void)(%s);\n", ind, be.code);
        }
        b_put(&out_loop, bodys.s);
        b_putf(&out_loop, "%s}\n", ind);
        free(bodys.s);
    } else {
        char outv[32]; snprintf(outv, sizeof outv, "__out%d", em->tmp);
        char acc[32];  snprintf(acc,  sizeof acc,  "__acc%d", em->tmp);
        if (strcmp(prop, "map") == 0 || strcmp(prop, "filter") == 0) {
            b_putf(&out_loop, "%smochi_array *%s = mochi_array_new();\n", ind, outv);
            b_putf(&out_loop, "%sfor (size_t %s = 0; %s < (%s)->len; %s++) {\n", ind, li, li, o.code, li);
            b_putf(&out_loop, "%s    double %s = mochi_val_num((%s)->items[%s]);\n", ind, pname, o.code, li);
        } else {
            b_putf(&out_loop, "%sdouble %s = 0;\n", ind, acc);
            b_putf(&out_loop, "%sfor (size_t %s = 0; %s < (%s)->len; %s++) {\n", ind, li, li, o.code, li);
            b_putf(&out_loop, "%s    double %s = %s;\n", ind, pname, acc);
            b_putf(&out_loop, "%s    double %s = mochi_val_num((%s)->items[%s]);\n", ind, pname2, o.code, li);
        }
        Buf bodyc; b_init(&bodyc);
        CmcType btype = CT_UNKNOWN;
        if (body_node) {
            Buf saved = em->out; em->out = bodyc;
            EStr be = emit_expr(em, body_node);
            em->out = saved;
            b_put(&bodyc, be.code);
            btype = be.type;
        }
        if (strcmp(prop, "map") == 0) {
            if (btype == CT_STR)
                b_putf(&out_loop, "%s    mochi_array_push(%s, mochi_str((%s)));\n", ind, outv, bodyc.len ? bodyc.s : "\"\"");
            else
                b_putf(&out_loop, "%s    mochi_array_push(%s, mochi_num((double)(%s)));\n", ind, outv, bodyc.len ? bodyc.s : "0");
        } else if (strcmp(prop, "filter") == 0) {
            b_putf(&out_loop, "%s    if (%s) mochi_array_push(%s, (%s)->items[%s]);\n", ind, bodyc.len ? bodyc.s : "0", outv, o.code, li);
        } else {
            b_putf(&out_loop, "%s    %s = (%s);\n", ind, acc, bodyc.len ? bodyc.s : "0");
        }
        b_putf(&out_loop, "%s}\n", ind);
        free(bodyc.s);
        if (strcmp(prop, "map") == 0 || strcmp(prop, "filter") == 0)
            result_name = cmc_arena_strndup(em->ctx->arena, outv, strlen(outv));
        else
            result_name = cmc_arena_strndup(em->ctx->arena, acc, strlen(acc));
    }
    cmc_env_pop(&em->env);
    /* the loop is a statement: push to pre-lines so it precedes the current stmt */
    b_put(&em->pre, out_loop.s);
    free(out_loop.s);
    if (result_name) {
        if (strcmp(prop, "map") == 0 || strcmp(prop, "filter") == 0)
            return estr(result_name, CT_ARR, 0, 1, NULL);
        return estr(result_name, CT_DOUBLE, 0, 0, NULL);
    }
    return estr("0", CT_INT, 0, 0, NULL);
}

static EStr emit_call(Emit *em, CmcNode *n) {
    CmcNode *callee = n->obj;
    if (callee->kind == N_IDENT) {
        const char *nm = callee->name;
        if (strcmp(nm, "console") == 0) {
            emit_console(em, n->args, n->nargs, 1);
            return estr("0", CT_UNDEF, 0, 0, NULL);
        }
        EStr *args = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
        for (int i = 0; i < n->nargs; i++) args[i] = emit_expr(em, n->args[i]);
        Buf b; b_init(&b);
        CmcType t = CT_UNKNOWN;
        if (strcmp(nm, "parseInt") == 0) {
            Buf s; b_init(&s); co_str(em, &s, args[0]);
            b_putf(&b, "(int)atoi(%s)", s.s); free(s.s); t = CT_INT;
        } else if (strcmp(nm, "parseFloat") == 0) {
            Buf s; b_init(&s); co_str(em, &s, args[0]);
            b_putf(&b, "atof(%s)", s.s); free(s.s); t = CT_DOUBLE;
        } else if (strcmp(nm, "Number") == 0) { co_num(em, &b, args[0]); t = CT_DOUBLE; }
        else if (strcmp(nm, "String") == 0) { co_str(em, &b, args[0]); t = CT_STR; }
        else if (strcmp(nm, "Boolean") == 0) { co_bool(em, &b, args[0]); t = CT_BOOL; }
        else if (strcmp(nm, "isNaN") == 0) { Buf c; b_init(&c); co_num(em, &c, args[0]); b_putf(&b, "isnan(%s)", c.s); free(c.s); t = CT_BOOL; }
        else {
            CmcSym *s = cmc_env_lookup(&em->env, nm);
            if (s && s->kind == SYM_FUNC) {
                b_putf(&b, "%s(", s->cname);
                CmcNode *fn = s->func_node;
                if (fn && fn->kind == N_ARROW && fn->nparams > 0) {
                    /* nullish-guarded arrow: pass has_X, value for each param */
                    int np = fn->nparams;
                    for (int i = 0; i < np; i++) {
                        if (i) b_put(&b, ", ");
                        if (i < n->nargs) {
                            Buf h; b_init(&h);
                            arg_has(em, n->args[i], &h);
                            b_putf(&b, "%s, ", h.s);
                            free(h.s);
                            if (args[i].type == CT_STR) co_str(em, &b, args[i]);
                            else co_num(em, &b, args[i]);
                        } else {
                            /* omitted arg: not present -> has=0, value 0/NULL */
                            CmcType pt = CT_DOUBLE;
                            CmcNode *pn = fn->params[i];
                            if (pn && pn->kind == N_IDENT) pt = infer_param_type_lite(em, fn, pn->name);
                            else if (pn && pn->kind == N_ASSIGN && pn->lhs) pt = cmc_type_of(em->ctx, &em->env, pn->rhs);
                            b_put(&b, pt == CT_STR ? "0, NULL" : "0, 0");
                        }
                    }
                } else {
                    for (int i = 0; i < n->nargs; i++) {
                        if (i) b_put(&b, ", ");
                        if (args[i].type == CT_STR) co_str(em, &b, args[i]);
                        else co_num(em, &b, args[i]);
                    }
                }
                b_put(&b, ")");
                t = s->type;
            } else {
                b_putf(&b, "%s(", nm);
                for (int i = 0; i < n->nargs; i++) { if (i) b_put(&b, ", "); co_num(em, &b, args[i]); }
                b_put(&b, ")");
                t = CT_UNKNOWN;
            }
        }
        free(args);
        return estr(b.s, t, 0, t == CT_STR ? 1 : 0, NULL);
    }
    if (callee->kind == N_MEMBER) {
        CmcNode *m = callee;
        EStr o = emit_expr(em, m->obj);
        const char *prop = m->prop ? m->prop->name : "";
        /* object shorthand method call: inline the lifted static fn */
        if (o.struct_name) {
            for (int mi = 0; mi < em->m_n; mi++) {
                if (strcmp(em->m_shape[mi], o.struct_name) == 0 && strcmp(em->m_field[mi], prop) == 0) {
                    Buf mb; b_init(&mb);
                    b_putf(&mb, "%s(", em->m_fn[mi]);
                    CmcNode *mfn = em->m_node[mi];
                    int np_ = mfn && mfn->nparams ? mfn->nparams : 0;
                    for (int i = 0; i < np_; i++) {
                        if (i) b_put(&mb, ", ");
                        if (i < n->nargs) {
                            EStr ae = emit_expr(em, n->args[i]);
                            b_put(&mb, "1, "); /* arg present -> has_X=1 */
                            if (ae.type == CT_STR) co_str(em, &mb, ae);
                            else co_num(em, &mb, ae);
                        } else {
                            b_put(&mb, "0, 0"); /* omitted -> has_X=0, value 0 */
                        }
                    }
                    b_put(&mb, ")");
                    EStr r = estr(mb.s, CT_INT, 0, 0, NULL);
                    return r;
                }
            }
        }
        if (m->obj->kind == N_IDENT && strcmp(m->obj->name, "console") == 0) {
            int stderr_ = strcmp(prop, "error") == 0 || strcmp(prop, "warn") == 0;
            emit_console(em, n->args, n->nargs, stderr_);
            return estr("0", CT_UNDEF, 0, 0, NULL);
        }
        if (m->obj->kind == N_IDENT && strcmp(m->obj->name, "Math") == 0) {
            EStr *args = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
            for (int i = 0; i < n->nargs; i++) args[i] = emit_expr(em, n->args[i]);
            Buf b; b_init(&b);
            CmcType t = CT_DOUBLE;
            if (strcmp(prop, "abs") == 0) b_putf(&b, "fabs(%s)", args[0].code);
            else if (strcmp(prop, "floor") == 0) b_putf(&b, "floor(%s)", args[0].code);
            else if (strcmp(prop, "ceil") == 0) b_putf(&b, "ceil(%s)", args[0].code);
            else if (strcmp(prop, "round") == 0) b_putf(&b, "round(%s)", args[0].code);
            else if (strcmp(prop, "sqrt") == 0) b_putf(&b, "sqrt(%s)", args[0].code);
            else if (strcmp(prop, "pow") == 0) b_putf(&b, "pow(%s, %s)", args[0].code, args[1].code);
            else if (strcmp(prop, "min") == 0) b_putf(&b, "fmin(%s, %s)", args[0].code, args[1].code);
            else if (strcmp(prop, "max") == 0) b_putf(&b, "fmax(%s, %s)", args[0].code, args[1].code);
            else if (strcmp(prop, "random") == 0) b_put(&b, "mochi_rand01()");
            else if (strcmp(prop, "sign") == 0) { t = CT_INT; b_putf(&b, "(((%s) > 0) - ((%s) < 0))", args[0].code, args[0].code); }
            else if (strcmp(prop, "trunc") == 0) b_putf(&b, "trunc(%s)", args[0].code);
            else b_putf(&b, "0");
            free(args);
            return estr(b.s, t, 0, 0, NULL);
        }
        if (o.type == CT_ARR) {
            EStr *args = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
            for (int i = 0; i < n->nargs; i++) args[i] = emit_expr(em, n->args[i]);
            Buf b; b_init(&b);
            CmcType t = CT_INT;
            if (strcmp(prop, "push") == 0) {
                b_putf(&b, "mochi_array_push(%s, ", o.code);
                for (int i = 0; i < n->nargs; i++) {
                    if (i) b_put(&b, ", ");
                    Buf v; b_init(&v); co_box(&v, args[i]);
                    b_put(&b, v.s);
                    free(v.s);
                }
                b_put(&b, ")");
            } else if (strcmp(prop, "pop") == 0) {
                b_putf(&b, "mochi_array_pop(%s)", o.code);
                t = CT_MOCHI_VAL;
            } else if (strcmp(prop, "join") == 0) {
                Buf s; b_init(&s);
                if (n->nargs) co_str(em, &s, args[0]);
                else b_put(&s, "\",\"");
                b_putf(&b, "mochi_array_join(%s, %s)", o.code, s.s);
                free(s.s);
                t = CT_STR;
            } else if (strcmp(prop, "length") == 0) {
                b_putf(&b, "(int)(%s%slen)", o.code, o.ptr ? "->" : ".");
            } else if (strcmp(prop, "map") == 0 || strcmp(prop, "filter") == 0 ||
                       strcmp(prop, "reduce") == 0 || strcmp(prop, "forEach") == 0) {
                EStr r = emit_arr_higher(em, n, prop, o);
                free(args);
                return r;
            } else b_putf(&b, "0");
            free(args);
            return estr(b.s, t, 0, t == CT_STR ? 1 : 0, NULL);
        }
        if (o.type == CT_STR) {
            EStr *args = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
            for (int i = 0; i < n->nargs; i++) args[i] = emit_expr(em, n->args[i]);
            Buf b; b_init(&b);
            CmcType t = CT_STR;
            if (strcmp(prop, "toUpperCase") == 0) b_putf(&b, "mochi_toupper(%s)", o.code);
            else if (strcmp(prop, "toLowerCase") == 0) b_putf(&b, "mochi_tolower(%s)", o.code);
            else if (strcmp(prop, "charAt") == 0) b_putf(&b, "mochi_char_at(%s, %s)", o.code, args[0].code);
            else if (strcmp(prop, "substring") == 0 || strcmp(prop, "substr") == 0)
                b_putf(&b, "mochi_substr(%s, %s, %s)", o.code, args[0].code, n->nargs > 1 ? args[1].code : "-1");
            else if (strcmp(prop, "indexOf") == 0) {
                b_putf(&b, "mochi_index_of(%s, %s, %s)", o.code, args[0].code, n->nargs > 1 ? args[1].code : "0");
                t = CT_INT;
            }
            else if (strcmp(prop, "trim") == 0) b_putf(&b, "mochi_trim(%s)", o.code);
            else if (strcmp(prop, "includes") == 0) { em->need_string = 1; b_putf(&b, "mochi_includes(%s, %s)", o.code, args[0].code); t = CT_BOOL; }
            else if (strcmp(prop, "startsWith") == 0) { b_putf(&b, "mochi_starts_with(%s, %s)", o.code, args[0].code); t = CT_BOOL; }
            else if (strcmp(prop, "endsWith") == 0) { b_putf(&b, "mochi_ends_with(%s, %s)", o.code, args[0].code); t = CT_BOOL; }
            else if (strcmp(prop, "slice") == 0) b_putf(&b, "mochi_slice(%s, %s, %s)", o.code, args[0].code, n->nargs > 1 ? args[1].code : "1000000");
            else if (strcmp(prop, "repeat") == 0) b_putf(&b, "mochi_repeat(%s, %s)", o.code, args[0].code);
            else if (strcmp(prop, "replace") == 0) b_putf(&b, "mochi_replace(%s, %s, %s)", o.code, args[0].code, args[1].code);
            else if (strcmp(prop, "split") == 0) { b_putf(&b, "mochi_split(%s, %s)", o.code, args[0].code); t = CT_ARR; }
            else b_putf(&b, "%s", o.code);
            free(args);
            return estr(b.s, t, 0, (t == CT_STR || t == CT_ARR) ? 1 : 0, NULL);
        }
        if (o.type == CT_OBJ && o.struct_name) {
            Buf b; b_init(&b);
            member_base(em, &b, o);
            b_put(&b, prop);
            b_put(&b, "(");
            for (int i = 0; i < n->nargs; i++) {
                EStr a = emit_expr(em, n->args[i]);
                if (i) b_put(&b, ", ");
                co_num(em, &b, a);
            }
            b_put(&b, ")");
            return estr(b.s, CT_UNKNOWN, 0, 0, NULL);
        }
        Buf b; b_init(&b);
        member_base(em, &b, o);
        b_putf(&b, "%s(", prop);
        for (int i = 0; i < n->nargs; i++) {
            EStr a = emit_expr(em, n->args[i]);
            if (i) b_put(&b, ", ");
            co_num(em, &b, a);
        }
        b_put(&b, ")");
        return estr(b.s, CT_UNKNOWN, 0, 0, NULL);
    }
    EStr c = emit_expr(em, callee);
    Buf b; b_init(&b);
    b_putf(&b, "%s(", c.code);
    for (int i = 0; i < n->nargs; i++) {
        EStr a = emit_expr(em, n->args[i]);
        if (i) b_put(&b, ", ");
        co_num(em, &b, a);
    }
    b_put(&b, ")");
    return estr(b.s, CT_UNKNOWN, 0, 0, NULL);
}

static EStr emit_expr(Emit *em, CmcNode *n) {
    if (!n) return estr("0", CT_NULL, 0, 0, NULL);
    switch (n->kind) {
        case N_NUM: case N_BIGINT: case N_STR: case N_BOOL:
        case N_NULL: case N_UNDEF: return emit_literal(em, n);
        case N_IDENT: return emit_ident(em, n);
        case N_MEMBER: return emit_member(em, n);
        case N_BIN: return emit_bin(em, n);
        case N_UNARY: return emit_unary(em, n);
        case N_ASSIGN: return emit_assign(em, n);
        case N_UPDATE: return emit_update(em, n);
        case N_COND: return emit_cond(em, n);
        case N_ARR: return emit_array(em, n);
        case N_OBJ: return emit_object(em, n);
        case N_TEMPLATE: return emit_template(em, n);
        case N_CALL: return emit_call(em, n);
        case N_ARROW: return emit_arrow(em, n);
        case N_SEQ: return n->nstmts ? emit_expr(em, n->stmts[n->nstmts - 1]) : estr("0", CT_UNDEF, 0, 0, NULL);
        case N_THIS: return estr("0", CT_UNKNOWN, 0, 0, NULL);
        default: return estr("0", CT_UNKNOWN, 0, 0, NULL);
    }
}

/* ---------------------- deferred-type pre-pass ----------------------
   `var x;` (declarator with no init) gets its C type from the FIRST
   assignment `x = <expr>` in the program. JS hoists `var` to function
   scope, so we record across the whole main body, then resolve from the
   first assignment we meet (string -> const char*, number -> double). */
static void uninit_record(Emit *em, const char *name) {
    if (!name) return;
    for (int i = 0; i < em->u_n; i++)
        if (strcmp(em->u_names[i], name) == 0) return;
    if (em->u_n < 128) { em->u_names[em->u_n] = name; em->u_types[em->u_n] = CT_UNKNOWN; em->u_n++; }
}
static CmcType uninit_type(Emit *em, const char *name) {
    for (int i = 0; i < em->u_n; i++)
        if (strcmp(em->u_names[i], name) == 0) return em->u_types[i];
    return CT_UNKNOWN;
}
static void uninit_set(Emit *em, const char *name, CmcType t) {
    for (int i = 0; i < em->u_n; i++)
        if (strcmp(em->u_names[i], name) == 0 && em->u_types[i] == CT_UNKNOWN) em->u_types[i] = t;
}
static void uninit_collect(Emit *em, CmcNode *s) {
    if (!s) return;
    switch (s->kind) {
        case N_VAR:
            for (int i = 0; i < s->nstmts; i++) {
                CmcNode *dcl = s->stmts[i];
                if (dcl->lhs && dcl->lhs->kind == N_IDENT &&
                    (dcl->rhs == NULL || dcl->rhs->kind == N_NULL || dcl->rhs->kind == N_UNDEF))
                    uninit_record(em, dcl->lhs->name);
            }
            break;
        case N_IF:  uninit_collect(em, s->cons); uninit_collect(em, s->alt); break;
        case N_WHILE: case N_DOWHILE: uninit_collect(em, s->stmt); break;
        case N_FOR: uninit_collect(em, s->init); uninit_collect(em, s->stmt); break;
        case N_FOROF: uninit_collect(em, s->stmt); break;
        case N_BLOCK: for (int i = 0; i < s->nstmts; i++) uninit_collect(em, s->stmts[i]); break;
        case N_SWITCH: for (int i = 0; i < s->ncases; i++) uninit_collect(em, s->cases[i]); break;
        default: break;
    }
}
static void uninit_resolve(Emit *em, CmcNode *s) {
    if (!s) return;
    if (s->kind == N_EXPRSTMT && s->lhs && s->lhs->kind == N_ASSIGN &&
        s->lhs->lhs && s->lhs->lhs->kind == N_IDENT && s->lhs->rhs)
        uninit_set(em, s->lhs->lhs->name, cmc_type_of(em->ctx, &em->env, s->lhs->rhs));
    switch (s->kind) {
        case N_IF:  uninit_resolve(em, s->cons); uninit_resolve(em, s->alt); break;
        case N_WHILE: case N_DOWHILE: uninit_resolve(em, s->stmt); break;
        case N_FOR: uninit_resolve(em, s->init); uninit_resolve(em, s->test); uninit_resolve(em, s->update); uninit_resolve(em, s->stmt); break;
        case N_FOROF: uninit_resolve(em, s->stmt); break;
        case N_BLOCK: for (int i = 0; i < s->nstmts; i++) uninit_resolve(em, s->stmts[i]); break;
        case N_SWITCH: for (int i = 0; i < s->ncases; i++) uninit_resolve(em, s->cases[i]); break;
        default: break;
    }
}
static void uninit_prepass(Emit *em, CmcNode *block) {
    em->u_n = 0;
    uninit_collect(em, block);
    uninit_resolve(em, block);
}

/* ---------------------- statements ---------------------- */
static void emit_var_decl(Emit *em, CmcNode *n) {
    for (int i = 0; i < n->nstmts; i++) {
        CmcNode *d = n->stmts[i];
        CmcNode *target = d->lhs;
        if (target->kind == N_DESTRUCT_OBJ || target->kind == N_DESTRUCT_ARR) {
            EStr src = d->rhs ? emit_expr(em, d->rhs) : estr("0", CT_NULL, 0, 0, NULL);
            flush_pre(em);
            if (target->kind == N_DESTRUCT_OBJ) {
                const char *sname = src.struct_name;
                for (int j = 0; j < target->ndpat; j++) {
                    const char *key = target->dpat_keys[j]->str;
                    CmcNode *val = target->dpat_vals[j];
                    if (val->kind != N_IDENT) continue;
                    const char *ft = sname ? cmc_struct_field_type(sname, key) : "double";
                    if (!ft) ft = "double";
                    CmcType vt = CT_DOUBLE;
                    if (strcmp(ft, "int") == 0) vt = CT_INT;
                    else if (strcmp(ft, "int64_t") == 0) vt = CT_INT64;
                    else if (strcmp(ft, "double") == 0) vt = CT_DOUBLE;
                    else if (strcmp(ft, "const char *") == 0) vt = CT_STR;
                    else if (strcmp(ft, "mochi_array *") == 0) vt = CT_ARR;
                    cmc_env_decl(&em->env, val->name, vt, SYM_LOCAL);
                    /* SROA: read the field directly, value stays in a register */
                    e_linef(em, "%s %s = %s->%s;", ft, val->name, src.code, key);
                }
            } else {
                for (int j = 0; j < target->nelems; j++) {
                    CmcNode *el = target->elems[j];
                    if (!el || el->kind != N_IDENT) continue;
                    cmc_env_decl(&em->env, el->name, CT_MOCHI_VAL, SYM_LOCAL);
                    e_linef(em, "mochi_val %s = mochi_array_get(%s, %d);", el->name, src.code, j);
                }
            }
            continue;
        }
        if (target->kind != N_IDENT) continue;
        const char *name = target->name;
        if (d->rhs == NULL) {
            CmcType t = uninit_type(em, name);
            cmc_env_decl(&em->env, name, t, SYM_LOCAL);
            if (t == CT_STR) e_linef(em, "const char *%s = NULL;", name);
            else e_linef(em, "double %s = 0;", name);
            continue;
        }
        EStr e = emit_expr(em, d->rhs);
        flush_pre(em);
        if (d->rhs->kind == N_ARROW) {
            /* `const f = (a,b=10) => ...` — f aliases the generated function. */
            CmcSym *fs = cmc_env_decl(&em->env, name, fn_return_type(d->rhs), SYM_FUNC);
            fs->cname = e.code;
            fs->func_node = d->rhs;
            continue;
        }
        /* `var x = null/undefined` with a later assignment: use the resolved type. */
        if ((d->rhs->kind == N_NULL || d->rhs->kind == N_UNDEF) && uninit_type(em, name) != CT_UNKNOWN) {
            CmcType rt = uninit_type(em, name);
            cmc_env_decl(&em->env, name, rt, SYM_LOCAL);
            if (rt == CT_STR) e_linef(em, "const char *%s = NULL;", name);
            else if (rt == CT_OBJ) e_linef(em, "void *%s = NULL;", name);
            else e_linef(em, "double %s = 0;", name);
            continue;
        }
        CmcType t = e.type;
        if (t == CT_UNDEF || t == CT_VOID) t = CT_DOUBLE;
        if (t == CT_UNKNOWN) t = CT_DOUBLE;
        CmcSym *s = cmc_env_decl(&em->env, name, t, SYM_LOCAL);
        s->struct_name = e.struct_name;
        s->heap = (t == CT_OBJ || t == CT_ARR) ? 1 : 0;
        Buf r; b_init(&r);
        switch (t) {
            case CT_STR: co_str(em, &r, e); break;
            case CT_BOOL: co_bool(em, &r, e); break;
            case CT_INT: case CT_INT64: case CT_DOUBLE: co_num(em, &r, e); break;
            case CT_MOCHI_VAL: b_put(&r, e.code); break;
            case CT_OBJ: b_putf(&r, "(%s)", e.code); break;
            case CT_ARR: b_putf(&r, "(%s)", e.code); break;
            default: b_put(&r, e.code);
        }
        /* object/array values keep their concrete struct type, not void* */
        if (t == CT_OBJ && e.struct_name)
            e_linef(em, "struct %s * %s = %s;", e.struct_name, name, r.s);
        else
            e_linef(em, "%s %s = %s;", cmc_type_name(t), name, r.s);
        free(r.s);
    }
}

static void emit_if(Emit *em, CmcNode *n) {
    EStr c = emit_expr(em, n->test);
    Buf cond; b_init(&cond);
    co_bool(em, &cond, c);
    e_linef(em, "if (%s) {", cond.s);
    free(cond.s);
    em->indent++;
    emit_stmt(em, n->cons);
    em->indent--;
    if (n->alt) {
        e_line(em, "} else {");
        em->indent++;
        emit_stmt(em, n->alt);
        em->indent--;
    }
    e_line(em, "}");
}

static void emit_block(Emit *em, CmcNode *n) {
    for (int i = 0; i < n->nstmts; i++) emit_stmt(em, n->stmts[i]);
}

static void emit_while(Emit *em, CmcNode *n) {
    EStr c = emit_expr(em, n->test);
    Buf cond; b_init(&cond);
    co_bool(em, &cond, c);
    e_linef(em, "while (%s) {", cond.s);
    free(cond.s);
    em->indent++;
    emit_stmt(em, n->stmt);
    em->indent--;
    e_line(em, "}");
}

static void emit_for(Emit *em, CmcNode *n) {
    Buf head; b_init(&head);
    b_put(&head, "for (");
    if (n->init) {
        if (n->init->kind == N_VAR) {
            CmcNode *d = n->init->stmts[0];
            if (d->rhs) {
                EStr e = emit_expr(em, d->rhs);
                CmcType t = e.type;
                if (t == CT_UNDEF || t == CT_UNKNOWN) t = CT_INT;
                cmc_env_decl(&em->env, d->lhs->name, t, SYM_LOCAL);
                Buf r; b_init(&r);
                if (t == CT_STR) co_str(em, &r, e);
                else if (t == CT_BOOL) co_bool(em, &r, e);
                else co_num(em, &r, e);
                b_putf(&head, "%s %s = %s; ", cmc_type_name(t), d->lhs->name, r.s);
                free(r.s);
            } else {
                cmc_env_decl(&em->env, d->lhs->name, CT_DOUBLE, SYM_LOCAL);
                b_putf(&head, "double %s = 0; ", d->lhs->name);
            }
        } else {
            EStr e = emit_expr(em, n->init);
            b_put(&head, e.code); b_put(&head, "; ");
        }
    } else b_put(&head, "; ");
    if (n->test) {
        EStr c = emit_expr(em, n->test);
        Buf cond; b_init(&cond);
        co_bool(em, &cond, c);
        b_put(&head, cond.s);
        free(cond.s);
    }
    b_put(&head, "; ");
    if (n->update) {
        /* for-update: the value is discarded, so i++/i-- lower to i += 1 directly */
        if (n->update->kind == N_UPDATE && n->update->lhs->kind == N_IDENT) {
            CmcSym *s = cmc_env_lookup(&em->env, n->update->lhs->name);
            const char *t = s ? s->cname : n->update->lhs->name;
            b_putf(&head, "%s %s= 1", t, n->update->op > 0 ? "+" : "-");
        } else {
            EStr u = emit_expr(em, n->update);
            b_put(&head, u.code);
        }
    }
    b_put(&head, ") {");
    flush_pre(em);
    e_line(em, head.s);
    free(head.s);
    em->indent++;
    emit_stmt(em, n->stmt);
    em->indent--;
    e_line(em, "}");
}

static void emit_forof(Emit *em, CmcNode *n) {
    EStr arr = emit_expr(em, n->test);
    flush_pre(em);
    cmc_env_decl(&em->env, n->left->name, CT_MOCHI_VAL, SYM_LOCAL);
    const char *idx = tmp_name(em, "i");
    e_linef(em, "for (size_t %s = 0; %s < (%s)->len; %s++) {", idx, idx, arr.code, idx);
    em->indent++;
    e_linef(em, "mochi_val %s = (%s)->items[%s];", n->left->name, arr.code, idx);
    emit_stmt(em, n->stmt);
    em->indent--;
    e_line(em, "}");
}

static void emit_forin(Emit *em, CmcNode *n) {
    EStr obj = emit_expr(em, n->test);
    flush_pre(em);
    const char *sname = obj.struct_name;
    if (!sname) { e_line(em, "(void)0;"); return; }
    cmc_env_decl(&em->env, n->left->name, CT_STR, SYM_LOCAL);
    const char *keys = tmp_name(em, "keys");
    const char *ki = tmp_name(em, "ki");
    int nf = 0;
    e_linef(em, "const char *%s[] = {", keys);
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sname, f);
        if (!fn) break;
        if (nf) e_line(em, ", ");
        e_linef(em, "\"%s\"", fn);
        nf++;
    }
    e_line(em, "};");
    if (nf == 0) return;
    e_linef(em, "for (size_t %s = 0; %s < %d; %s++) {", ki, ki, nf, ki);
    em->indent++;
    e_linef(em, "const char *%s = %s[%s];", n->left->name, keys, ki);
    emit_stmt(em, n->stmt);
    em->indent--;
    e_line(em, "}");
}

static void emit_switch(Emit *em, CmcNode *n) {
    EStr disc = emit_expr(em, n->test);
    int first = 1;
    for (int i = 0; i < n->ncases; i++) {
        CmcNode *c = n->cases[i];
        CmcNode *t = n->case_tests[i];
        if (t) {
            EStr ce = emit_expr(em, t);
            Buf cond; b_init(&cond);
            if (disc.type == CT_STR || ce.type == CT_STR) {
                em->need_string = 1;
                b_putf(&cond, "strcmp(%s, %s) == 0", disc.code, ce.code);
            } else {
                b_putf(&cond, "(%s == %s)", disc.code, ce.code);
            }
            e_linef(em, "%sif (%s) {", first ? "" : "} else ", cond.s);
            free(cond.s);
        } else {
            e_linef(em, "%s{", first ? "" : "} else ");
        }
        first = 0;
        em->indent++;
        for (int j = 0; j < c->nstmts; j++) {
            if (c->stmts[j]->kind == N_BREAK) continue;
            emit_stmt(em, c->stmts[j]);
        }
        em->indent--;
    }
    if (!first) e_line(em, "}");
}

static void emit_stmt(Emit *em, CmcNode *n) {
    flush_pre(em);
    switch (n->kind) {
        case N_VAR: emit_var_decl(em, n); break;
        case N_FUNC: break;
        case N_IF: emit_if(em, n); break;
        case N_WHILE: emit_while(em, n); break;
        case N_DOWHILE: {
            e_line(em, "do {");
            em->indent++;
            emit_stmt(em, n->stmt);
            em->indent--;
            EStr c = emit_expr(em, n->test);
            Buf cond; b_init(&cond);
            co_bool(em, &cond, c);
            e_linef(em, "} while (%s);", cond.s);
            free(cond.s);
            break;
        }
        case N_FOR: emit_for(em, n); break;
        case N_FOROF: emit_forof(em, n); break;
        case N_FORIN: emit_forin(em, n); break;
        case N_RETURN: {
            if (n->lhs) {
                EStr e = emit_expr(em, n->lhs);
                Buf r; b_init(&r);
                if (e.type == CT_STR) co_str(em, &r, e);
                else if (e.type == CT_BOOL) co_bool(em, &r, e);
                else if (cmc_is_numeric(e.type)) co_num(em, &r, e);
                else b_put(&r, e.code);
                e_linef(em, "return %s;", r.s);
                free(r.s);
            } else e_line(em, "return;");
            break;
        }
        case N_BREAK: e_line(em, "break;"); break;
        case N_CONTINUE: e_line(em, "continue;"); break;
        case N_BLOCK: emit_block(em, n); break;
        case N_SWITCH: emit_switch(em, n); break;
        case N_THROW: {
            EStr e = emit_expr(em, n->lhs);
            Buf r; b_init(&r);
            co_str(em, &r, e);
            const char *tv = tmp_name(em, "thrown");
            e_linef(em, "const char *%s = %s;", tv, r.s);
            em->last_thrown = tv;
            if (em->try_depth == 0)
                e_linef(em, "fprintf(stderr, \"Exception: %%s\\n\", %s);", tv);
            free(r.s);
            break;
        }
        case N_TRY: {
            em->try_depth++;
            emit_stmt(em, n->stmt);
            em->try_depth--;
            if (n->alt) {
                if (n->third && n->third->kind == N_IDENT) {
                    cmc_env_decl(&em->env, n->third->name, CT_STR, SYM_LOCAL);
                    e_linef(em, "const char *%s = %s ? %s : \"\";", n->third->name,
                            em->last_thrown ? em->last_thrown : "0", em->last_thrown ? em->last_thrown : "\"\"");
                }
                emit_stmt(em, n->alt);
            }
            break;
        }
        case N_EXPRSTMT: {
            EStr e = emit_expr(em, n->lhs);
            flush_pre(em);
            if (e.type != CT_VOID && e.type != CT_UNDEF) {
                e_linef(em, "(void)(%s);", e.code);
            }
            break;
        }
        default: break;
    }
}

/* ---------------------- function analysis ---------------------- */
static CmcType fn_return_type(CmcNode *fn) {
    CmcType ret = CT_VOID;
    int saw = 0;
    CmcNode *stack[512]; int sp = 0;
    stack[sp++] = fn->body;
    while (sp) {
        CmcNode *cur = stack[--sp];
        if (!cur) continue;
        if (cur->kind == N_RETURN && cur->lhs) {
            saw = 1;
            CmcType t = CT_DOUBLE;
            CmcNode *a = cur->lhs;
            if (a->kind == N_STR) t = CT_STR;
            else if (a->kind == N_BOOL) t = CT_BOOL;
            else if (a->kind == N_NUM) t = (a->num == (long long)a->num) ? CT_INT : CT_DOUBLE;
            else if (a->kind == N_BIGINT) t = CT_INT64;
            else if (a->kind == N_NULL) t = CT_NULL;
            else if (a->kind == N_ARR) t = CT_ARR;
            else if (a->kind == N_OBJ) t = CT_OBJ;
            if (ret == CT_VOID) ret = t;
            else if (t == CT_STR || ret == CT_STR) ret = CT_STR;
            else if (t == CT_DOUBLE || ret == CT_DOUBLE) ret = CT_DOUBLE;
        }
        if (cur->kind == N_BLOCK) for (int i = 0; i < cur->nstmts; i++) stack[sp++] = cur->stmts[i];
        else if (cur->kind == N_IF) { stack[sp++] = cur->test; stack[sp++] = cur->cons; stack[sp++] = cur->alt; }
        else if (cur->kind == N_WHILE || cur->kind == N_DOWHILE || cur->kind == N_FOROF) { stack[sp++] = cur->test; stack[sp++] = cur->stmt; }
        else if (cur->kind == N_FOR) { stack[sp++] = cur->init; stack[sp++] = cur->test; stack[sp++] = cur->update; stack[sp++] = cur->stmt; }
        else if (cur->kind == N_VAR) for (int i = 0; i < cur->nstmts; i++) stack[sp++] = cur->stmts[i];
        else if (cur->kind == N_EXPRSTMT) stack[sp++] = cur->lhs;
        else if (cur->kind == N_RETURN) stack[sp++] = cur->lhs;
    }
    /* expression-bodied arrow: infer from the body expression */
    if (!saw && fn->is_expr_body && fn->body) {
        CmcNode *b = fn->body;
        if (b->kind == N_EXPRSTMT && b->lhs) b = b->lhs;
        if (b->kind == N_NUM) return (b->num == (long long)b->num) ? CT_INT : CT_DOUBLE;
        if (b->kind == N_STR) return CT_STR;
        if (b->kind == N_BOOL) return CT_BOOL;
        if (b->kind == N_BIGINT) return CT_INT64;
        if (b->kind == N_NULL) return CT_NULL;
        if (b->kind == N_ARR) return CT_ARR;
        if (b->kind == N_OBJ) return CT_OBJ;
        if (b->kind == N_BIN) {
            if (b->op == TK_PLUS) {
                /* string concat if either side is a string literal */
                CmcNode *l = b->lhs, *r = b->rhs;
                if ((l->kind == N_STR && r->kind == N_STR) ||
                    (l->kind == N_STR) || (r->kind == N_STR)) return CT_STR;
            }
            if (b->op == TK_LT || b->op == TK_GT || b->op == TK_LE || b->op == TK_GE ||
                b->op == TK_EQEQ || b->op == TK_NE || b->op == TK_ANDAND || b->op == TK_OROR)
                return CT_BOOL;
            if (b->op == TK_SLASH) return CT_DOUBLE;
            /* arithmetic: emit_arrow resolves unknown params to CT_INT */
            return CT_INT;
        }
        if (b->kind == N_CALL) return CT_INT;
    }
    return saw ? ret : CT_VOID;
}

static CmcType infer_param_type_lite(Emit *em, CmcNode *fn, const char *pname) {
    int has_double = 0, has_str = 0;
    CmcNode *stack[512]; int sp = 0;
    stack[sp++] = fn;
    while (sp) {
        CmcNode *cur = stack[--sp];
        if (!cur) continue;
        if (cur->kind == N_BIN) {
            if (cur->op == TK_SLASH) has_double = 1;
            if (cur->lhs->kind == N_IDENT && strcmp(cur->lhs->name, pname) == 0) {
                if (cur->rhs->kind == N_STR) has_str = 1;
                else if (cur->rhs->kind == N_NUM && cur->rhs->num != (long long)cur->rhs->num) has_double = 1;
            }
            if (cur->rhs->kind == N_IDENT && strcmp(cur->rhs->name, pname) == 0) {
                if (cur->lhs->kind == N_STR) has_str = 1;
                else if (cur->lhs->kind == N_NUM && cur->lhs->num != (long long)cur->lhs->num) has_double = 1;
            }
            stack[sp++] = cur->lhs; stack[sp++] = cur->rhs;
        } else if (cur->kind == N_BLOCK) { for (int i = 0; i < cur->nstmts; i++) stack[sp++] = cur->stmts[i]; }
        else if (cur->kind == N_IF) { stack[sp++] = cur->test; stack[sp++] = cur->cons; stack[sp++] = cur->alt; }
        else if (cur->kind == N_WHILE || cur->kind == N_DOWHILE || cur->kind == N_FOROF) { stack[sp++] = cur->test; stack[sp++] = cur->stmt; }
        else if (cur->kind == N_FOR) { stack[sp++] = cur->init; stack[sp++] = cur->test; stack[sp++] = cur->update; stack[sp++] = cur->stmt; }
        else if (cur->kind == N_VAR) for (int i = 0; i < cur->nstmts; i++) stack[sp++] = cur->stmts[i];
        else if (cur->kind == N_EXPRSTMT) stack[sp++] = cur->lhs;
        else if (cur->kind == N_RETURN) stack[sp++] = cur->lhs;
        else if (cur->kind == N_CALL) { stack[sp++] = cur->obj; for (int i = 0; i < cur->nargs; i++) stack[sp++] = cur->args[i]; }
        else if (cur->kind == N_MEMBER) { stack[sp++] = cur->obj; stack[sp++] = cur->prop; }
        else if (cur->kind == N_ASSIGN) { stack[sp++] = cur->lhs; stack[sp++] = cur->rhs; }
        else if (cur->kind == N_UNARY) stack[sp++] = cur->lhs;
        else if (cur->kind == N_COND) { stack[sp++] = cur->lhs; stack[sp++] = cur->rhs; stack[sp++] = cur->third; }
    }
    if (has_str) return CT_STR;
    if (has_double) return CT_DOUBLE;
    return CT_INT;
}

/* ---------------------- function declaration emission ---------------------- */
static void emit_function(Emit *em, CmcNode *fn) {
    CmcType ret = fn_return_type(fn);
    if (ret == CT_VOID) ret = CT_INT;
    CmcType *pt = (CmcType *)malloc(sizeof(CmcType) * (fn->nparams ? fn->nparams : 1));
    for (int i = 0; i < fn->nparams; i++) {
        if (fn->params[i]->kind == N_IDENT) pt[i] = infer_param_type_lite(em, fn, fn->params[i]->name);
        else pt[i] = CT_DOUBLE;
    }
    cmc_env_push(&em->env, em->ctx->arena);
    Buf sig; b_init(&sig);
    b_putf(&sig, "static %s %s(", cmc_type_name(ret), fn->fname);
    for (int i = 0; i < fn->nparams; i++) {
        if (i) b_put(&sig, ", ");
        if (fn->params[i]->kind == N_IDENT) {
            b_putf(&sig, "%s %s", cmc_type_name(pt[i]), fn->params[i]->name);
            cmc_env_decl(&em->env, fn->params[i]->name, pt[i], SYM_PARAM);
        } else b_put(&sig, "double p");
    }
    b_put(&sig, ") {");
    e_line(em, sig.s);
    free(sig.s);
    em->indent++;
    emit_block(em, fn->body);
    em->indent--;
    e_line(em, "}");
    cmc_env_pop(&em->env);
    free(pt);
}

/* ---------------------- entry ---------------------- */
char *cmc_emit(CmcCtx *ctx, CmcNode *program) {
    Emit em;
    memset(&em, 0, sizeof(em));
    em.ctx = ctx;
    cmc_env_init(&em.env, ctx->arena);
    b_init(&em.out);
    b_init(&em.structs);
    b_init(&em.pre);

    int nfns = 0, nmain = 0;
    CmcNode **fns = (CmcNode **)malloc(sizeof(CmcNode *) * (program->nstmts ? program->nstmts : 1));
    CmcNode **main_body = (CmcNode **)malloc(sizeof(CmcNode *) * (program->nstmts ? program->nstmts : 1));
    for (int i = 0; i < program->nstmts; i++) {
        if (program->stmts[i]->kind == N_FUNC) fns[nfns++] = program->stmts[i];
        else main_body[nmain++] = program->stmts[i];
    }

    /* hoist function symbols so recursion/forward calls resolve */
    for (int i = 0; i < nfns; i++) {
        CmcType ret = fn_return_type(fns[i]);
        if (ret == CT_VOID) ret = CT_INT;
        CmcSym *s = cmc_env_decl(&em.env, fns[i]->fname, ret, SYM_FUNC);
        s->cname = fns[i]->fname;
    }

    for (int i = 0; i < nfns; i++) {
        em.cur_fn = fns[i];
        emit_function(&em, fns[i]);
        em.cur_fn = NULL;
    }

    em.cur_fn = NULL;
    /* deferred-type pre-pass: `var x;` gets its type from first assignment */
    {
        CmcNode mb; memset(&mb, 0, sizeof(mb));
        mb.kind = N_BLOCK; mb.stmts = main_body; mb.nstmts = nmain;
        uninit_prepass(&em, &mb);
    }
    e_line(&em, "int main(void) {");
    em.indent++;
    if (nmain == 0) e_line(&em, "(void)0;");
    for (int i = 0; i < nmain; i++) emit_stmt(&em, main_body[i]);
    em.indent--;
    e_line(&em, "return 0;");
    e_line(&em, "}");

    Buf out; b_init(&out);
    b_put(&out, "/* generated by cmochicode (JS -> C99) — do not edit by hand */\n");
    b_put(&out, "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n");
    if (em.need_stdint) b_put(&out, "#include <stdint.h>\n");
    if (em.need_math) b_put(&out, "#include <math.h>\n");
    b_put(&out, "#include \"cmochicode_rt.h\"\n\n");
    /* emit concrete struct definitions for every registered object shape */
    for (int si = 0; si < cmc_shape_count(); si++) {
        const char *sn = cmc_shape_name(si);
        if (!sn) continue;
        b_putf(&em.structs, "typedef struct %s {\n", sn);
        int fc = cmc_shape_field_count(si);
        for (int j = 0; j < fc; j++) {
            const char *fn = cmc_shape_field_name(si, j);
            const char *ft = cmc_shape_field_type(si, j);
            if (!fn || !ft) continue;
            b_putf(&em.structs, "    %s %s;\n", ft, fn);
        }
        b_putf(&em.structs, "} %s;\n\n", sn);
    }
    if (em.structs.len) { b_put(&out, em.structs.s); b_put(&out, "\n"); }
    if (em.fn_extra.len) { b_put(&out, em.fn_extra.s); b_put(&out, "\n"); }
    b_put(&out, em.out.s);
    free(fns); free(main_body);
    return out.s;
}
