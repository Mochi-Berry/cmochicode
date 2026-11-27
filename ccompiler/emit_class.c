/* emit_class.c — class lowering: this-field scan, method lift, new/ctor.
 * Part of the stage-4 C99 emitter. */
#include "emit_internal.h"

void class_strctx(CmcNode *n, const char ***ctx, int *nn, int *cap) {
    if (!n) return;
    if (n->kind == N_BIN) {
        CmcNode *a = n->lhs, *b = n->rhs;
        int astr = a && (a->kind == N_STR || a->kind == N_TEMPLATE);
        int bstr = b && (b->kind == N_STR || b->kind == N_TEMPLATE);
        if ((astr || bstr) && *nn < 32) {
            CmcNode *mf = NULL;
            if (a && a->kind == N_MEMBER && a->obj && a->obj->kind == N_THIS && a->prop) mf = a;
            else if (b && b->kind == N_MEMBER && b->obj && b->obj->kind == N_THIS && b->prop) mf = b;
            if (mf && mf->prop->name) {
                int seen = 0;
                for (int i = 0; i < *nn; i++) if (strcmp((*ctx)[i], mf->prop->name) == 0) seen = 1;
                if (!seen) {
                    if (*nn == *cap) { *cap = *cap ? *cap * 2 : 8;
                        *ctx = (const char **)realloc(*ctx, sizeof(char *) * (size_t)*cap); }
                    (*ctx)[(*nn)++] = mf->prop->name;
                }
            }
        }
    }
    class_strctx(n->obj, ctx, nn, cap);
    class_strctx(n->prop, ctx, nn, cap);
    class_strctx(n->lhs, ctx, nn, cap);
    class_strctx(n->rhs, ctx, nn, cap);
    class_strctx(n->third, ctx, nn, cap);
    class_strctx(n->init, ctx, nn, cap);
    class_strctx(n->test, ctx, nn, cap);
    class_strctx(n->update, ctx, nn, cap);
    class_strctx(n->stmt, ctx, nn, cap);
    class_strctx(n->cons, ctx, nn, cap);
    class_strctx(n->alt, ctx, nn, cap);
    class_strctx(n->body, ctx, nn, cap);
    class_strctx(n->left, ctx, nn, cap);
    for (int i = 0; i < n->nargs; i++) class_strctx(n->args[i], ctx, nn, cap);
    for (int i = 0; i < n->nstmts; i++) class_strctx(n->stmts[i], ctx, nn, cap);
    for (int i = 0; i < n->ncases; i++) { class_strctx(n->case_tests[i], ctx, nn, cap); class_strctx(n->cases[i], ctx, nn, cap); }
    for (int i = 0; i < n->nparams; i++) class_strctx(n->params[i], ctx, nn, cap);
    for (int i = 0; i < n->nelems; i++) class_strctx(n->elems[i], ctx, nn, cap);
    for (int i = 0; i < n->nprops; i++) class_strctx(n->props[i].value, ctx, nn, cap);
    for (int i = 0; i < n->ndpat; i++) class_strctx(n->dpat_vals[i], ctx, nn, cap);
}

CmcType class_lit_type(CmcNode *n, const char *sn) {
    if (!n) return CT_UNKNOWN;
    switch (n->kind) {
    case N_THIS: return sn ? CT_OBJ : CT_UNKNOWN;
    case N_MEMBER:
        if (sn && n->obj && n->obj->kind == N_THIS && n->prop && !n->computed) {
            CmcType fk = cmc_struct_field_kind(sn, n->prop->name);
            if (fk != CT_UNKNOWN) return fk;
        }
        return CT_UNKNOWN;
    case N_NUM:    return (n->num == (long long)n->num) ? CT_INT : CT_DOUBLE;
    case N_BIGINT: return CT_INT64;
    case N_STR: case N_TEMPLATE: return CT_STR;
    case N_BOOL:   return CT_BOOL;
    case N_NULL:   return CT_NULL;
    case N_UNDEF:  return CT_UNDEF;
    case N_ARR:    return CT_ARR;
    case N_OBJ:    return CT_OBJ;
    case N_CALL:   return CT_INT;
    case N_RETURN: return class_lit_type(n->lhs, sn);
    case N_BIN:
        if (n->op == TK_SLASH) return CT_DOUBLE;
        return (class_lit_type(n->lhs, sn) == CT_STR) ? CT_STR : CT_INT;
    default:       return CT_UNKNOWN;
    }
}

/* walk the AST collecting `this.field = <expr>` -> field name + rhs node */

void class_scan(CmcNode *n, const char ***names, CmcNode ***vals, int *nn, int *cap) {
    if (!n) return;
    if (n->kind == N_ASSIGN && n->lhs && n->lhs->kind == N_MEMBER &&
        n->lhs->obj && n->lhs->obj->kind == N_THIS && n->lhs->prop) {
        const char *f = n->lhs->prop->name;
        int seen = 0, i;
        for (i = 0; i < *nn; i++) if (strcmp((*names)[i], f) == 0) { seen = 1; break; }
        if (!seen && f && *nn < 32) {
            if (*nn == *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *names = (const char **)realloc(*names, sizeof(char *) * (size_t)*cap);
                *vals = (CmcNode **)realloc(*vals, sizeof(CmcNode *) * (size_t)*cap);
            }
            (*names)[*nn] = f;
            (*vals)[*nn] = n->rhs;
            (*nn)++;
        }
    }
    class_scan(n->obj, names, vals, nn, cap);
    class_scan(n->prop, names, vals, nn, cap);
    class_scan(n->lhs, names, vals, nn, cap);
    class_scan(n->rhs, names, vals, nn, cap);
    class_scan(n->third, names, vals, nn, cap);
    class_scan(n->init, names, vals, nn, cap);
    class_scan(n->test, names, vals, nn, cap);
    class_scan(n->update, names, vals, nn, cap);
    class_scan(n->stmt, names, vals, nn, cap);
    class_scan(n->cons, names, vals, nn, cap);
    class_scan(n->alt, names, vals, nn, cap);
    class_scan(n->body, names, vals, nn, cap);
    class_scan(n->left, names, vals, nn, cap);
    for (int i = 0; i < n->nargs; i++) class_scan(n->args[i], names, vals, nn, cap);
    for (int i = 0; i < n->nstmts; i++) class_scan(n->stmts[i], names, vals, nn, cap);
    for (int i = 0; i < n->ncases; i++) {
        class_scan(n->case_tests[i], names, vals, nn, cap);
        class_scan(n->cases[i], names, vals, nn, cap);
    }
    for (int i = 0; i < n->nparams; i++) class_scan(n->params[i], names, vals, nn, cap);
    for (int i = 0; i < n->nelems; i++) class_scan(n->elems[i], names, vals, nn, cap);
    for (int i = 0; i < n->nprops; i++) class_scan(n->props[i].value, names, vals, nn, cap);
    for (int i = 0; i < n->ndpat; i++) class_scan(n->dpat_vals[i], names, vals, nn, cap);
}

/* infer a parameter's type from `this.<field> = <param>` inside the body:
   the field's shape type is the best signal for constructor args. */

CmcType class_param_type(CmcNode *n, const char *sn, const char *pname) {
    if (!n) return CT_UNKNOWN;
    if (n->kind == N_ASSIGN && n->lhs && n->lhs->kind == N_MEMBER &&
        n->lhs->obj && n->lhs->obj->kind == N_THIS && n->lhs->prop &&
        n->rhs && n->rhs->kind == N_IDENT && strcmp(n->rhs->name, pname) == 0) {
        CmcType fk = cmc_struct_field_kind(sn, n->lhs->prop->name);
        if (fk != CT_UNKNOWN) return fk;
    }
    CmcType t;
    t = class_param_type(n->obj, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->prop, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->lhs, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->rhs, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->third, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->init, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->test, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->update, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->stmt, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->cons, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->alt, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->body, sn, pname); if (t != CT_UNKNOWN) return t;
    t = class_param_type(n->left, sn, pname); if (t != CT_UNKNOWN) return t;
    for (int i = 0; i < n->nargs; i++) { t = class_param_type(n->args[i], sn, pname); if (t != CT_UNKNOWN) return t; }
    for (int i = 0; i < n->nstmts; i++) { t = class_param_type(n->stmts[i], sn, pname); if (t != CT_UNKNOWN) return t; }
    for (int i = 0; i < n->ncases; i++) { t = class_param_type(n->cases[i], sn, pname); if (t != CT_UNKNOWN) return t; }
    for (int i = 0; i < n->nparams; i++) { t = class_param_type(n->params[i], sn, pname); if (t != CT_UNKNOWN) return t; }
    for (int i = 0; i < n->nelems; i++) { t = class_param_type(n->elems[i], sn, pname); if (t != CT_UNKNOWN) return t; }
    for (int i = 0; i < n->nprops; i++) { t = class_param_type(n->props[i].value, sn, pname); if (t != CT_UNKNOWN) return t; }
    for (int i = 0; i < n->ndpat; i++) { t = class_param_type(n->dpat_vals[i], sn, pname); if (t != CT_UNKNOWN) return t; }
    return CT_UNKNOWN;
}

/* return type of a method body: first `return <expr>` wins */

CmcType class_ret_type(CmcNode *n, const char *sn) {
    if (!n) return CT_UNKNOWN;
    if (n->kind == N_RETURN) return n->lhs ? class_lit_type(n->lhs, sn) : CT_VOID;
    CmcType t;
    t = class_ret_type(n->body, sn); if (t != CT_UNKNOWN) return t;
    t = class_ret_type(n->cons, sn); if (t != CT_UNKNOWN) return t;
    t = class_ret_type(n->alt, sn); if (t != CT_UNKNOWN) return t;
    t = class_ret_type(n->stmt, sn); if (t != CT_UNKNOWN) return t;
    t = class_ret_type(n->init, sn); if (t != CT_UNKNOWN) return t;
    for (int i = 0; i < n->nstmts; i++) { t = class_ret_type(n->stmts[i], sn); if (t != CT_UNKNOWN) return t; }
    for (int i = 0; i < n->ncases; i++) { t = class_ret_type(n->cases[i], sn); if (t != CT_UNKNOWN) return t; }
    return CT_UNKNOWN;
}

/* lift one class method: static RET name(struct SN *this_, <params>) */

const char *emit_class_method(Emit *em, CmcNode *cls, CmcProp *pr,
                                     const char *sn, int has_ctor) {
    char fnbuf[64];
    snprintf(fnbuf, sizeof fnbuf, "__cmc_%s_%d", cls->fname, em->fnid++);
    const char *fnname = cmc_arena_strndup(em->ctx->arena, fnbuf, strlen(fnbuf));
    CmcNode *fn = pr->value;
    int np = fn ? (fn->nparams ? fn->nparams : 0) : 0;
    const char *thisname = "this_";
    CmcType ret = class_ret_type(fn ? fn->body : NULL, sn);
    if (ret == CT_UNKNOWN || ret == CT_VOID) ret = CT_INT;
    if (has_ctor) ret = CT_VOID;

    Buf sig; b_init(&sig);
    const char *retty = (ret == CT_OBJ) ? NULL : cmc_type_name(ret);
    char retbuf[64];
    if (ret == CT_OBJ) { snprintf(retbuf, sizeof retbuf, "struct %s *", sn); retty = retbuf; }
    b_putf(&sig, "static %s %s(struct %s *%s", retty, fnname, sn, thisname);
    cmc_env_push(&em->env, em->ctx->arena);
    CmcSym *ts = cmc_env_decl(&em->env, "this", CT_OBJ, SYM_PARAM);
    ts->cname = thisname;
    ts->struct_name = sn;
    for (int i = 0; i < np; i++) {
        CmcNode *pn = fn->params[i];
        const char *nm = (pn->kind == N_IDENT) ? pn->name :
                          ((pn->kind == N_ASSIGN && pn->lhs && pn->lhs->kind == N_IDENT) ? pn->lhs->name : "p");
        CmcType pt = CT_DOUBLE;
        if (pn->kind == N_ASSIGN) pt = class_lit_type(pn->rhs, sn);
        if (pt == CT_UNKNOWN || pt == CT_VOID || pt == CT_DOUBLE) {
            /* arg that feeds `this.<field> = arg`: use the field's type as the signal */
            CmcType fp = class_param_type(fn ? fn->body : NULL, sn, nm);
            if (fp != CT_UNKNOWN) pt = fp;
        }
        b_putf(&sig, ", %s %s", cmc_type_name(pt), nm);
        cmc_env_decl(&em->env, nm, pt, SYM_PARAM);
    }
    b_put(&sig, ") {");

    Buf save = em->out;
    Buf body; b_init(&body);
    em->out = body;
    const char *save_this = em->this_name;
    em->this_name = thisname;
    em->indent++;
    e_line(em, sig.s);
    if (fn && fn->body) emit_stmt(em, fn->body);
    em->indent--;
    em->this_name = save_this;
    e_line(em, "}");
    cmc_env_pop(&em->env);
    em->out = save;
    if (!em->fn_extra.s) b_init(&em->fn_extra);
    b_put(&em->fn_extra, body.s);
    b_put(&em->fn_extra, "\n");
    free(body.s);
    free(sig.s);
    return fnname;
}

void emit_class(Emit *em, CmcNode *n) {
    /* 1. discover fields from `this.x = ...` across every method body */
    const char **fnames = NULL; CmcNode **fvals = NULL; int nn = 0, cap = 0;
    for (int i = 0; i < n->nprops; i++) class_scan(n->props[i].value, &fnames, &fvals, &nn, &cap);
    /* 1b. fields used in a string context are const char * no matter the init */
    const char **sc = NULL; int snn = 0, scap = 0;
    for (int i = 0; i < n->nprops; i++) class_strctx(n->props[i].value, &sc, &snn, &scap);
    int strctx(const char *f) {
        for (int i = 0; i < snn; i++) if (strcmp(sc[i], f) == 0) return 1;
        return 0;
    }
    /* 2. reuse the assignment rhs nodes so cmc_register_shape() infers real types */
    CmcNode *fake = (CmcNode *)cmc_arena_alloc(em->ctx->arena, sizeof(CmcNode));
    memset(fake, 0, sizeof(*fake));
    fake->kind = N_OBJ;
    fake->nprops = nn;
    fake->props = (CmcProp *)cmc_arena_alloc(em->ctx->arena, sizeof(CmcProp) * (nn ? nn : 1));
    for (int i = 0; i < nn; i++) {
        CmcNode *v = fvals[i];
        if (strctx(fnames[i]) && v && !(v->kind == N_STR || v->kind == N_TEMPLATE)) {
            /* used in a string concat but init isn't a literal -> force const char * */
            CmcNode *sv = (CmcNode *)cmc_arena_alloc(em->ctx->arena, sizeof(CmcNode));
            memset(sv, 0, sizeof(*sv));
            sv->kind = N_STR;
            sv->str = "";
            v = sv;
        }
        fake->props[i].key = fnames[i];
        fake->props[i].value = v;
        fake->props[i].spread = 0;
        fake->props[i].computed = 0;
    }
    const char *sn = cmc_register_shape(em->ctx->arena, fake);
    /* 3. lift each method; the constructor may have an empty body */
    const char *ctor = NULL;
    for (int i = 0; i < n->nprops; i++) {
        int hc = (strcmp(n->props[i].key, "constructor") == 0);
        CmcNode *fn = n->props[i].value;
        if (fn && hc && (!fn->body || fn->body->nstmts == 0)) {
            CmcNode *blk = (CmcNode *)cmc_arena_alloc(em->ctx->arena, sizeof(CmcNode));
            memset(blk, 0, sizeof(*blk));
            blk->kind = N_BLOCK;
            fn->body = blk;
        }
        const char *fnname = emit_class_method(em, n, &n->props[i], sn, hc);
        if (hc) ctor = fnname;
        if (em->m_n < 64) {
            CmcType rty = class_ret_type(fn ? fn->body : NULL, sn);
            if (rty == CT_UNKNOWN || rty == CT_VOID) rty = CT_INT;
            if (hc) rty = CT_VOID;
            em->m_shape[em->m_n] = sn;
            em->m_field[em->m_n] = n->props[i].key;
            em->m_fn[em->m_n] = fnname;
            em->m_node[em->m_n] = fn;
            em->m_guarded[em->m_n] = 0;
            em->m_nparams[em->m_n] = fn ? fn->nparams : 0;
            em->m_ret[em->m_n] = rty;
            em->m_n++;
        }
    }
    /* 4. remember the class so `new Name(...)` can allocate + construct */
    if (em->cl_n < 32) {
        em->cl_name[em->cl_n] = n->fname;
        em->cl_struct[em->cl_n] = sn;
        em->cl_ctor[em->cl_n] = ctor;
        em->cl_n++;
    }
    free(fnames);
    free(fvals);
}

EStr emit_new(Emit *em, CmcNode *n) {
    const char *nm = (n->obj && n->obj->kind == N_IDENT) ? n->obj->name : "";
    for (int i = 0; i < em->cl_n; i++) {
        if (strcmp(em->cl_name[i], nm) != 0) continue;
        const char *sn = em->cl_struct[i];
        const char *obj = tmp_name(em, "obj");
        Buf l; b_init(&l);
        b_putf(&l, "struct %s *%s = (struct %s *)CMC_ALLOC(sizeof(struct %s));", sn, obj, sn, sn);
        e_preline(em, l.s);
        free(l.s);
        if (em->cl_ctor[i]) {
            Buf c; b_init(&c);
            b_putf(&c, "%s(%s", em->cl_ctor[i], obj);
            for (int k = 0; k < n->nargs; k++) {
                EStr a = emit_expr(em, n->args[k]);
                b_put(&c, ", ");
                if (a.type == CT_STR) co_str(em, &c, a);
                else co_num(em, &c, a);
            }
            b_put(&c, ");");
            e_preline(em, c.s);
            free(c.s);
        }
        flush_pre(em);
        cmc_env_decl(&em->env, nm, CT_OBJ, SYM_FUNC);
        return estr(obj, CT_OBJ, 0, 1, sn);
    }
    return estr("0", CT_OBJ, 0, 1, NULL);
}
