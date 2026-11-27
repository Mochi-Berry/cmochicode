/* emit_stmt.c — statement emitters (var/if/for/forin/forof/switch/block).
 * Part of the stage-4 C99 emitter. */
#include "emit_internal.h"

void uninit_record(Emit *em, const char *name) {
    if (!name) return;
    for (int i = 0; i < em->u_n; i++)
        if (strcmp(em->u_names[i], name) == 0) return;
    if (em->u_n < 128) { em->u_names[em->u_n] = name; em->u_types[em->u_n] = CT_UNKNOWN; em->u_n++; }
}

CmcType uninit_type(Emit *em, const char *name) {
    for (int i = 0; i < em->u_n; i++)
        if (strcmp(em->u_names[i], name) == 0) return em->u_types[i];
    return CT_UNKNOWN;
}

void uninit_set(Emit *em, const char *name, CmcType t) {
    for (int i = 0; i < em->u_n; i++)
        if (strcmp(em->u_names[i], name) == 0 && em->u_types[i] == CT_UNKNOWN) em->u_types[i] = t;
}

void uninit_collect(Emit *em, CmcNode *s) {
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

void uninit_resolve(Emit *em, CmcNode *s) {
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

void uninit_prepass(Emit *em, CmcNode *block) {
    em->u_n = 0;
    uninit_collect(em, block);
    uninit_resolve(em, block);
}

/* ---------------------- statements ---------------------- */

/* names assigned (or updated) inside any try/catch subtree of this function.
 * Per C rules, an automatic object modified between setjmp and longjmp has an
 * indeterminate value afterwards unless it is volatile — so these locals must
 * live in memory, not in a register, or the catch branch reads a stale value. */
static void vol_collect_assign(CmcNode *n, const char **names, int *cnt) {
    if (!n) return;
    switch (n->kind) {
        case N_ASSIGN:
            if (n->lhs && n->lhs->kind == N_IDENT) {
                for (int i = 0; i < *cnt; i++) if (strcmp(names[i], n->lhs->name) == 0) break;
                if (*cnt < 64) {
                    int dup = 0; for (int i = 0; i < *cnt; i++) if (strcmp(names[i], n->lhs->name) == 0) dup = 1;
                    if (!dup) names[(*cnt)++] = n->lhs->name;
                }
            }
            vol_collect_assign(n->lhs, names, cnt);
            vol_collect_assign(n->rhs, names, cnt);
            return;
        case N_UPDATE:
            if (n->lhs && n->lhs->kind == N_IDENT && *cnt < 64) {
                int dup = 0; for (int i = 0; i < *cnt; i++) if (strcmp(names[i], n->lhs->name) == 0) dup = 1;
                if (!dup) names[(*cnt)++] = n->lhs->name;
            }
            return;
        case N_BLOCK: for (int i = 0; i < n->nstmts; i++) vol_collect_assign(n->stmts[i], names, cnt); return;
        case N_IF:  vol_collect_assign(n->test, names, cnt); vol_collect_assign(n->cons, names, cnt); vol_collect_assign(n->alt, names, cnt); return;
        case N_WHILE: case N_DOWHILE: case N_FOROF:
            vol_collect_assign(n->test, names, cnt); vol_collect_assign(n->stmt, names, cnt); return;
        case N_FOR:
            vol_collect_assign(n->init, names, cnt); vol_collect_assign(n->test, names, cnt);
            vol_collect_assign(n->update, names, cnt); vol_collect_assign(n->stmt, names, cnt); return;
        case N_VAR: for (int i = 0; i < n->nstmts; i++) vol_collect_assign(n->stmts[i], names, cnt); return;
        case N_EXPRSTMT: vol_collect_assign(n->lhs, names, cnt); return;
        case N_RETURN: vol_collect_assign(n->lhs, names, cnt); return;
        case N_CALL:
            if (n->obj) vol_collect_assign(n->obj, names, cnt);
            for (int i = 0; i < n->nargs; i++) vol_collect_assign(n->args[i], names, cnt);
            return;
        case N_MEMBER: vol_collect_assign(n->obj, names, cnt); return;
        case N_TRY:  vol_collect_assign(n->stmt, names, cnt); vol_collect_assign(n->alt, names, cnt); return;
        case N_FUNC: case N_CLASS: case N_ARROW: return;  /* separate function, own frame */
        default: return;
    }
}
void vol_pass(Emit *em, CmcNode *block) {
    em->vol_n = 0;
    vol_collect_assign(block, em->vol_names, &em->vol_n);
}
static int is_vol(Emit *em, const char *name) {
    for (int i = 0; i < em->vol_n; i++) if (strcmp(em->vol_names[i], name) == 0) return 1;
    return 0;
}
/* the declaration type text for a volatile local (NULL = not volatile) */
static char *vol_decl_type(Emit *em, const char *name, CmcType t, const char *sname) {
    if (!name || !is_vol(em, name)) return NULL;
    if (t == CT_STR) { char *r = malloc(32); strcpy(r, "const char * volatile"); return r; }
    if (t == CT_ARR) { char *r = malloc(32); strcpy(r, "mochi_array * volatile"); return r; }
    if (t == CT_OBJ || t == CT_NULL || t == CT_UNDEF) {
        Buf b; b_init(&b);
        if (t == CT_OBJ && sname) b_putf(&b, "struct %s * volatile", sname); else b_put(&b, "void * volatile");
        return b.s;
    }
    Buf b; b_init(&b);
    b_putf(&b, "volatile %s", cmc_type_name(t));
    return b.s;
}

void emit_var_decl(Emit *em, CmcNode *n) {
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
                    CmcNode *dv = NULL;
                    if (val->kind == N_ASSIGN) { dv = val->rhs; val = val->lhs; } /* default value */
                    if (val->kind != N_IDENT) continue;
                    const char *ft = sname ? cmc_struct_field_type(sname, key) : NULL;
                    int has_field = (ft != NULL);
                    if (!ft) ft = "double";
                    CmcType vt = CT_DOUBLE;
                    if (strcmp(ft, "int") == 0) vt = CT_INT;
                    else if (strcmp(ft, "int64_t") == 0) vt = CT_INT64;
                    else if (strcmp(ft, "double") == 0) vt = CT_DOUBLE;
                    else if (strcmp(ft, "const char *") == 0) vt = CT_STR;
                    else if (strcmp(ft, "mochi_array *") == 0) vt = CT_ARR;
                    cmc_env_decl(&em->env, val->name, vt, SYM_LOCAL);
                    if (has_field) {
                        /* SROA: read the field directly, value stays in a register */
                        e_linef(em, "%s %s = %s->%s;", ft, val->name, src.code, key);
                    } else {
                        e_linef(em, "%s %s = %s;", ft, val->name, (vt == CT_STR) ? "NULL" :
                                ((vt == CT_ARR) ? "NULL" : "0"));
                    }
                    /* best-effort default: struct fields default to 0 / NULL */
                    if (dv) {
                        EStr dvv = emit_expr(em, dv);
                        if (vt == CT_STR)
                            e_linef(em, "if (!%s) %s = (const char *)%s;", val->name, val->name, dvv.code);
                        else
                            e_linef(em, "if (%s == 0) %s = %s;", val->name, val->name, dvv.code);
                    }
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
            char *va = vol_decl_type(em, name, t, NULL);
            if (va) { e_linef(em, "%s %s = %s;", va, name, t == CT_STR ? "NULL" : (t == CT_OBJ ? "NULL" : "0")); free(va); }
            else if (t == CT_STR) e_linef(em, "const char *%s = NULL;", name);
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
            char *vb = vol_decl_type(em, name, rt, NULL);
            if (vb) { e_linef(em, "%s %s = %s;", vb, name, rt == CT_STR || rt == CT_OBJ ? "NULL" : "0"); free(vb); }
            else if (rt == CT_STR) e_linef(em, "const char *%s = NULL;", name);
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
        char *vc = vol_decl_type(em, name, t, e.struct_name);
        if (vc) { e_linef(em, "%s %s = %s;", vc, name, r.s); free(vc); }
        else if (t == CT_OBJ && e.struct_name)
            e_linef(em, "struct %s * %s = %s;", e.struct_name, name, r.s);
        else
            e_linef(em, "%s %s = %s;", cmc_type_name(t), name, r.s);
        free(r.s);
    }
}

void emit_if(Emit *em, CmcNode *n) {
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

void emit_block(Emit *em, CmcNode *n) {
    for (int i = 0; i < n->nstmts; i++) emit_stmt(em, n->stmts[i]);
}

void emit_while(Emit *em, CmcNode *n) {
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

void emit_for(Emit *em, CmcNode *n) {
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
                char *vd = vol_decl_type(em, d->lhs->name, t, NULL);
                b_putf(&head, "%s %s = %s; ", vd ? vd : cmc_type_name(t), d->lhs->name, r.s);
                free(vd); free(r.s);
            } else {
                cmc_env_decl(&em->env, d->lhs->name, CT_DOUBLE, SYM_LOCAL);
                char *ve = vol_decl_type(em, d->lhs->name, CT_DOUBLE, NULL);
                b_putf(&head, "%s %s = 0; ", ve ? ve : "double", d->lhs->name);
                free(ve);
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

void emit_forof(Emit *em, CmcNode *n) {
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

void emit_forin(Emit *em, CmcNode *n) {
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

void emit_switch(Emit *em, CmcNode *n) {
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

void emit_stmt(Emit *em, CmcNode *n) {
    flush_pre(em);
    switch (n->kind) {
        case N_VAR: emit_var_decl(em, n); break;
        case N_FUNC: break; /* hoisted above main() */
        case N_CLASS: break; /* hoisted above main() */
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
                for (int ti = 0; ti < em->try_depth; ti++) e_line(em, "mochi_try_pop();");
                e_linef(em, "return %s;", r.s);
                free(r.s);
            } else {
                for (int ti = 0; ti < em->try_depth; ti++) e_line(em, "mochi_try_pop();");
                e_line(em, "return;");
            };
            break;
        }
        case N_BREAK: e_line(em, "break;"); break;
        case N_CONTINUE: e_line(em, "continue;"); break;
        case N_BLOCK: emit_block(em, n); break;
        case N_SWITCH: emit_switch(em, n); break;
        case N_THROW: {
            /* real unwinding: longjmp to the innermost live try frame */
            EStr e = emit_expr(em, n->lhs);
            Buf r; b_init(&r);
            co_str(em, &r, e);
            e_linef(em, "mochi_throw(%s);", r.s);
            free(r.s);
            break;
        }
        case N_TRY: {
            /* try { ... } catch (e) { ... }
             * setjmp MUST be inline here: a longjmp into a frame that has
             * already returned is UB, so the buffer is a local of this very
             * function and only registered with the runtime frame stack. */
            e_linef(em, "{");
            const char *jb = tmp_name(em, "jb");
            e_linef(em, "MochiJmpBuf %s;", jb);
            e_linef(em, "if (setjmp(%s) == 0) {", jb);
            em->indent++;
            e_linef(em, "mochi_try_push(&%s);", jb);
            em->try_depth++;
            emit_stmt(em, n->stmt);
            em->try_depth--;
            e_line(em, "mochi_try_pop();");
            em->indent--;
            e_line(em, "} else {");
            em->indent++;
            e_line(em, "mochi_try_pop();");   /* the throw did not pop for us */
            if (n->alt) {
                if (n->third && n->third->kind == N_IDENT) {
                    cmc_env_decl(&em->env, n->third->name, CT_STR, SYM_LOCAL);
                    e_linef(em, "const char *%s = mochi_last_thrown ? mochi_last_thrown : \"\";", n->third->name);
                }
                emit_stmt(em, n->alt);
            }
            em->indent--;
            e_line(em, "}");
            e_line(em, "}");
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
