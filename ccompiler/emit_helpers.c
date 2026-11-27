/* emit_helpers.c — growable buffers, string/number coercion, console folding.
 * Part of the stage-4 C99 emitter (see emit_internal.h). */
#include "emit_internal.h"

void b_init(Buf *b) { b->cap = 256; b->s = (char *)malloc(b->cap); b->s[0] = 0; b->len = 0; }

void b_ensure(Buf *b, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->s = (char *)realloc(b->s, b->cap);
    }
}

void b_put(Buf *b, const char *s) { size_t n = strlen(s); b_ensure(b, n); memcpy(b->s + b->len, s, n + 1); b->len += n; }

void b_putn(Buf *b, const char *s, size_t n) { b_ensure(b, n); memcpy(b->s + b->len, s, n); b->s[b->len + n] = 0; b->len += n; }

void b_putf(Buf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    b_put(b, tmp);
}


EStr estr(const char *code, CmcType t, int esc, int ptr, const char *sn) {
    EStr e; e.code = (char *)code; e.type = t; e.escapes = esc; e.ptr = ptr; e.struct_name = sn; return e;
}

void e_line(Emit *em, const char *s) {
    for (int i = 0; i < em->indent; i++) b_put(&em->out, "    ");
    b_put(&em->out, s);
    b_put(&em->out, "\n");
}

void e_linef(Emit *em, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    e_line(em, tmp);
}

void e_preline(Emit *em, const char *s) { if (s) { b_put(&em->pre, s); b_put(&em->pre, "\n"); } }

void flush_pre(Emit *em) {
    if (em->pre.len) { b_put(&em->out, em->pre.s); em->pre.len = 0; em->pre.s[0] = 0; }
}

const char *tmp_name(Emit *em, const char *base) {
    char buf[64];
    snprintf(buf, sizeof buf, "__%s_%d", base, em->tmp++);
    return cmc_arena_strndup(em->ctx->arena, buf, strlen(buf));
}

void c_lit(Buf *b, const char *s) {
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

void co_num(Emit *em, Buf *b, EStr e) {
    if (e.type == CT_MOCHI_VAL) b_putf(b, "mochi_val_num(%s)", e.code);
    else if (e.type == CT_STR) b_putf(b, "atof(%s)", e.code);
    else if (cmc_is_numeric(e.type)) b_put(b, e.code);
    else b_putf(b, "(double)(%s)", e.code);
}

void co_int(Buf *b, EStr e) {
    if (e.type == CT_MOCHI_VAL) b_putf(b, "(int)mochi_val_num(%s)", e.code);
    else if (e.type == CT_INT || e.type == CT_INT64) b_put(b, e.code);
    else b_putf(b, "(int)(%s)", e.code);
}

void co_str(Emit *em, Buf *b, EStr e) {
    if (e.type == CT_STR) b_put(b, e.code);
    else if (e.type == CT_MOCHI_VAL) b_putf(b, "mochi_val_str(%s)", e.code);
    else if (cmc_is_numeric(e.type)) b_putf(b, "mochi_num_to_str(%s)", e.code);
    else if (e.type == CT_BOOL) b_putf(b, "(%s ? \"true\" : \"false\")", e.code);
    else b_put(b, e.code);
}

void co_bool(Emit *em, Buf *b, EStr e) {
    if (e.type == CT_STR) b_putf(b, "(%s[0] != 0)", e.code);
    else if (e.type == CT_MOCHI_VAL) b_putf(b, "mochi_val_bool(%s)", e.code);
    else if (e.type == CT_ARR) b_putf(b, "((%s)->len > 0)", e.code);
    else if (e.type == CT_OBJ) b_putf(b, "(%s != NULL)", e.code);
    else b_putf(b, "(%s != 0)", e.code);
}

void co_box(Buf *b, EStr e) {
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
EStr emit_expr(Emit *em, CmcNode *n);
void emit_stmt(Emit *em, CmcNode *n);
CmcType fn_return_type(CmcNode *fn);
CmcType infer_param_type_lite(Emit *em, CmcNode *fn, const char *pname);

void member_base(Emit *em, Buf *b, EStr o) {
    b_put(b, o.code);
    if (o.type == CT_OBJ || o.type == CT_ARR) {
        if (o.ptr) b_put(b, "->"); else b_put(b, ".");
    } else b_put(b, "->");
}

void num_lit(Buf *b, double v) {
    if (v == (long long)v) {
        long long iv = (long long)v;
        if (iv >= INT32_MIN && iv <= INT32_MAX) b_putf(b, "%d", (int)iv);
        else b_putf(b, "%lldLL", iv);
    } else b_putf(b, "%.17g", v);
}

/* ---------------------- console.log folding ---------------------- */
/* Append a value argument with a ", " separator between values. */

void farg_push(Buf *fargs, const char *code) {
    if (fargs->len) b_put(fargs, ", ");
    b_put(fargs, code);
}

void append_print_arg(Emit *em, CmcNode *node, EStr e, Buf *fmt, Buf *fargs) {
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

/* does evaluating this expression have an observable side effect?
   (calls, assignments, updates). Pure literals/idents/field reads are safe
   to fold into a single printf; side-effecting args must be sequenced. */

int expr_has_side_effect(CmcNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case N_CALL: return 1;
    case N_ASSIGN: return 1;
    case N_UPDATE: return 1;
    case N_SEQ: return 1;
    default: break;
    }
    if (n->kind == N_MEMBER && n->obj && expr_has_side_effect(n->obj)) return 1;
    if (expr_has_side_effect(n->obj)) return 1;
    if (expr_has_side_effect(n->prop)) return 1;
    if (expr_has_side_effect(n->lhs)) return 1;
    if (expr_has_side_effect(n->rhs)) return 1;
    if (expr_has_side_effect(n->third)) return 1;
    for (int i = 0; i < n->nargs; i++) if (expr_has_side_effect(n->args[i])) return 1;
    for (int i = 0; i < n->nelems; i++) if (expr_has_side_effect(n->elems[i])) return 1;
    for (int i = 0; i < n->nprops; i++) if (expr_has_side_effect(n->props[i].value)) return 1;
    return 0;
}

void emit_console(Emit *em, CmcNode **args, int nargs, int to_stderr) {
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
        if (expr_has_side_effect(args[i])) {
            /* sequence side effects left-to-right: store in a typed temp first */
            char tname[32];
            snprintf(tname, sizeof tname, "__arg%d", em->tmp++);
            const char *ty = cmc_type_name(e.type);
            if (e.type == CT_VOID) ty = "int";
            Buf tl; b_init(&tl);
            b_putf(&tl, "%s %s = %s;", ty, tname, e.code);
            e_preline(em, tl.s);
            free(tl.s);
            e = estr(tname, e.type, 0, 0, e.struct_name);
        }
        append_print_arg(em, args[i], e, &fmt, &fargs);
    }
    flush_pre(em);
    if (fargs.len) e_linef(em, "(fprintf(%s, \"%s\\n\", %s));", to_stderr ? "stderr" : "stdout", fmt.s, fargs.s);
    else e_linef(em, "(fputs(\"%s\\n\", %s));", fmt.s, to_stderr ? "stderr" : "stdout");
    free(fmt.s); free(fargs.s);
}

/* ---------------------- expressions ---------------------- */
