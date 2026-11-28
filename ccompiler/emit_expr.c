/* emit_expr.c — expression/function emitters (literals, calls, arrows, objects).
 * Part of the stage-4 C99 emitter. */
#include "emit_internal.h"

EStr emit_literal(Emit *em, CmcNode *n) {
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

EStr emit_ident(Emit *em, CmcNode *n) {
    /* a name captured from an enclosing function: read through the upvalue */
    if (em->cur_up && em->cur_ncapt > 0) {
        for (int i = 0; i < em->cur_ncapt; i++) {
            if (em->cur_capt[i] && !strcmp(em->cur_capt[i], n->name)) {
                char cb[128];
                if (em->cur_capt_ref[i])
                    snprintf(cb, sizeof cb, "*((struct %s *)__env)->%s", em->cur_up, n->name);
                else
                    snprintf(cb, sizeof cb, "((struct %s *)__env)->%s", em->cur_up, n->name);
                return estr(cmc_arena_strndup(em->ctx->arena, cb, strlen(cb)),
                            em->cur_capt_type[i], 0, 0, NULL);
            }
        }
    }
    CmcSym *s = cmc_env_lookup(&em->env, n->name);
    if (s) {
        if (s->kind == SYM_FUNC) {
            /* a function used as a value: box it into a callable (t=5) */
            char buf[128];
            snprintf(buf, sizeof buf, "mochi_fn_box_n(__shim_%s, NULL, \"%s\")", s->cname, n->name);
            const char *box = cmc_arena_strndup(em->ctx->arena, buf, strlen(buf));
            return estr(box, CT_MOCHI_VAL, 0, 0, NULL);
        }
        int ptr = s->heap || s->type == CT_STR || s->type == CT_OBJ || s->type == CT_ARR;
        return estr(s->cname, s->type, s->heap, ptr, s->struct_name);
    }
    return estr(n->name, CT_UNKNOWN, 0, 0, NULL);
}

void ensure_dk_helpers(Emit *em, const char *sn);
const char *fn_return_shape(Emit *em, CmcNode *fn);
void cmc_emit_str_helper(Emit *em, const char *sn);
void cmc_emit_json_helper(Emit *em, const char *sn);

static int capt_is_ref(Emit *em, const char *name);
static const char *capt_lvalue(Emit *em, const char *name);
EStr emit_member_field(Emit *em, EStr o, CmcNode *obj_node, const char *prop, int optional) {
    /* function.length: params before the first default / rest — must be
     * checked FIRST, because a function name used as a value is now boxed
     * to CT_MOCHI_VAL (the callable), which would otherwise take the generic
     * dynamic-length path and return 0. */
    if (strcmp(prop, "length") == 0 && obj_node && obj_node->kind == N_IDENT) {
        CmcSym *s = cmc_env_lookup(&em->env, obj_node->name);
        if (s && s->kind == SYM_FUNC && s->func_node) {
            int cnt = 0;
            for (int i = 0; i < s->func_node->nparams; i++) {
                CmcNode *pn = s->func_node->params[i];
                if (pn->is_rest || pn->kind == N_ASSIGN) break;
                cnt++;
            }
            char nb[32]; snprintf(nb, sizeof nb, "%d", cnt);
            return estr(cmc_arena_strndup(em->ctx->arena, nb, strlen(nb)), CT_INT, 0, 0, NULL);
        }
    }
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
    if (o.type == CT_MOCHI_VAL && strcmp(prop, "length") == 0) {
            Buf b; b_init(&b);
            b_putf(&b, "mochi_val_length(%s)", o.code);
            return estr(b.s, CT_INT, 0, 0, NULL);
        }
    if (o.type == CT_OBJ) {
        /* Unknown static key: dispatch via the shape's get helper so the
         * dynamic overflow map is consulted instead of a bogus field read. */
        const char *hasf = o.struct_name ? cmc_struct_field_type(o.struct_name, prop) : NULL;
        if (!hasf && o.struct_name) {
            ensure_dk_helpers(em, o.struct_name);
            Buf b; b_init(&b);
            b_putf(&b, "%s_get(%s, \"%s\")", o.struct_name, o.code, prop);
            return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
        }
        Buf b; b_init(&b);
        member_base(em, &b, o);
        b_put(&b, prop);
        const char *ft = hasf;
        if (!ft) ft = "double"; /* missing property -> 0 (undefined) */
        CmcType t = CT_UNKNOWN;
        if (o.struct_name && cmc_struct_field_kind(o.struct_name, prop) == CT_BOOL) t = CT_BOOL;
        else if (strcmp(ft, "int") == 0) t = CT_INT;
        else if (strcmp(ft, "int64_t") == 0) t = CT_INT64;
        else if (strcmp(ft, "double") == 0) t = CT_DOUBLE;
        else if (strcmp(ft, "const char *") == 0) t = CT_STR;
        else if (strcmp(ft, "mochi_array *") == 0) t = CT_ARR;
        else if (strcmp(ft, "mochi_val") == 0) t = CT_MOCHI_VAL;
        else if (strncmp(ft, "struct ", 7) == 0) t = CT_OBJ;
        else t = CT_UNKNOWN;
        const char *inner_sn = (t == CT_OBJ && o.struct_name)
            ? cmc_struct_field_shape(o.struct_name, prop) : NULL;
        /* optional chaining `user?.id`: yield 0/NULL instead of crashing */
        if (optional) {
            Buf w; b_init(&w);
            if (t == CT_STR) b_putf(&w, "(%s != NULL ? %s : NULL)", o.code, b.s);
            else if (t == CT_ARR) b_putf(&w, "(%s != NULL ? %s : NULL)", o.code, b.s);
            else b_putf(&w, "(%s != NULL ? %s : 0)", o.code, b.s);
            free(b.s);
            return estr(w.s, t, 0, t == CT_STR || t == CT_ARR ? 1 : 0, inner_sn);
        }
        /* object/array fields hold POINTERS: member_base must use `->` */
        return estr(b.s, t, 0, (t == CT_OBJ || t == CT_ARR) ? 1 : 0, inner_sn);
    }
    Buf b; b_init(&b);
    member_base(em, &b, o);
    b_put(&b, prop);
    return estr(b.s, CT_UNKNOWN, o.escapes, o.ptr, NULL);
}

/* ---- dynamic string-key dispatch for objects ----
 * `obj[k]` with a runtime (non-const) key cannot be a static struct field
 * access. We emit one pair of static helpers per shape that dispatch on the
 * key string against the shape's known fields and box/unbox a mochi_val.
 * Unknown keys read as null / write as no-op (the shape has no such field).
 * This is the dynamic-typing path: statically-known keys still lower to a
 * direct struct field access (the fast path); only genuinely runtime keys
 * pay for the dispatch. */
static char *dk_box(CmcType k, const char *expr) {
    if (k == CT_MOCHI_VAL) return strdup(expr);  /* already a mochi_val */
    if (k == CT_STR) { Buf b; b_init(&b); b_putf(&b, "mochi_str(%s)", expr); return b.s; }
    if (k == CT_BOOL) { Buf b; b_init(&b); b_putf(&b, "mochi_bool(%s)", expr); return b.s; }
    if (k == CT_ARR) { Buf b; b_init(&b); b_putf(&b, "mochi_arr_box(%s)", expr); return b.s; }
    if (k == CT_OBJ) { Buf b; b_init(&b); b_putf(&b, "mochi_obj_box(%s)", expr); return b.s; }
    Buf b; b_init(&b); b_putf(&b, "mochi_num(%s)", expr); return b.s;
}
static const char *dk_unbox(CmcType k) {
    if (k == CT_MOCHI_VAL) return "v";   /* keep the tagged value as-is */
    if (k == CT_STR) return "mochi_val_str(v)";
    if (k == CT_BOOL) return "mochi_val_bool(v)";
    if (k == CT_ARR) return "v.p";   /* void* -> mochi_array* (implicit) */
    if (k == CT_OBJ) return "v.p";   /* void* -> struct X* (implicit) */
    return "mochi_val_num(v)";
}
/* A string formatter for a shape: the static fields first, then (for dk
 * shapes) the runtime map entries, so console.log(o) matches node. Object and
 * array fields print recursively (their own _str / mochi_array_inspect). */
void cmc_emit_str_helper(Emit *em, const char *sn) {
    Buf g; b_init(&g);
    b_putf(&g, "static const char *%s_str(const struct %s *o) {\n", sn, sn);
    b_putf(&g, "    static char buf[512];\n");
    b_putf(&g, "    int off = 0; int first = 1;\n");
    b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"{ \");\n");
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        const char *ft = cmc_struct_field_type_n(sn, f);
        CmcType fk = cmc_struct_field_kind(sn, fn);
        b_putf(&g, "    if (!first) off += snprintf(buf + off, sizeof buf - off, \", \"); first = 0;\n");
        b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%s: \");\n", fn);
        /* dispatch on the C type string first: a nested object field's KIND
         * may be unknown even though its type is `struct X *` */
        if (ft && strncmp(ft, "struct ", 7) == 0) {
            const char *st = ft + 7;
            char inner[64]; int ii = 0;
            while (st[ii] && st[ii] != ' ' && st[ii] != '*' && ii < 63) { inner[ii] = st[ii]; ii++; }
            inner[ii] = 0;
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", %s_str(o->%s));\n", inner, fn);
        }
        else if (ft && strcmp(ft, "mochi_array *") == 0)
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", mochi_array_inspect(o->%s));\n", fn);
        else if (ft && strcmp(ft, "const char *") == 0)
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"'%%s'\", o->%s);\n", fn);
        else if (ft && (strcmp(ft, "int") == 0 || strcmp(ft, "int64_t") == 0)) {
            if (fk == CT_BOOL)
                b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", o->%s ? \"true\" : \"false\");\n", fn);
            else
                b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%lld\", (long long)o->%s);\n", fn);
        }
        else if (ft && strcmp(ft, "double") == 0)
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%g\", o->%s);\n", fn);
        else
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", mochi_val_str_js(o->%s));\n", fn);
    }
    if (cmc_dk_is(sn)) {
        b_put(&g, "    for (MochiMapEntry *e = o->extra ? o->extra->head : NULL; e; e = e->next) {\n");
        b_put(&g, "        if (!first) off += snprintf(buf + off, sizeof buf - off, \", \"); first = 0;\n");
        b_put(&g, "        off += snprintf(buf + off, sizeof buf - off, \"%s: %s\", e->key, mochi_val_str_js(e->val));\n");
        b_put(&g, "    }\n");
    }
    b_put(&g, "    snprintf(buf + off, sizeof buf - off, \" }\");\n");
    b_put(&g, "    return buf;\n}\n\n");
    b_put(&em->fn_extra, g.s);
    free(g.s);
}
/* JSON.stringify form for a shape: {"k":v,...} with JSON quoting. Object
 * fields recurse through their own _json, array fields through
 * mochi_array_json, and the overflow map entries are included too. */
void cmc_emit_json_helper(Emit *em, const char *sn) {
    Buf g; b_init(&g);
    b_putf(&g, "static const char *%s_json(const struct %s *o) {\n", sn, sn);
    b_putf(&g, "    static char buf[1024];\n");
    b_putf(&g, "    int off = 0; int first = 1;\n");
    b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"{\");\n");
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        const char *ft = cmc_struct_field_type_n(sn, f);
        CmcType fk = cmc_struct_field_kind(sn, fn);
        b_putf(&g, "    if (!first) off += snprintf(buf + off, sizeof buf - off, \",\"); first = 0;\n");
        b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"\\\"%s\\\":\", \"%s\");\n", fn, fn);
        if (ft && strncmp(ft, "struct ", 7) == 0) {
            const char *st = ft + 7;
            char inner[64]; int ii = 0;
            while (st[ii] && st[ii] != ' ' && st[ii] != '*' && ii < 63) { inner[ii] = st[ii]; ii++; }
            inner[ii] = 0;
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", %s_json(o->%s));\n", inner, fn);
        }
        else if (ft && strcmp(ft, "mochi_array *") == 0)
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", mochi_array_json(o->%s));\n", fn);
        else if (ft && strcmp(ft, "const char *") == 0)
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", mochi_val_json(mochi_str(o->%s)));\n", fn);
        else if (ft && (strcmp(ft, "int") == 0 || strcmp(ft, "int64_t") == 0)) {
            if (fk == CT_BOOL)
                b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", o->%s ? \"true\" : \"false\");\n", fn);
            else
                b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%lld\", (long long)o->%s);\n", fn);
        }
        else if (ft && strcmp(ft, "double") == 0)
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%g\", o->%s);\n", fn);
        else
            b_putf(&g, "    off += snprintf(buf + off, sizeof buf - off, \"%%s\", mochi_val_json(o->%s));\n", fn);
    }
    if (cmc_dk_is(sn)) {
        b_put(&g, "    for (MochiMapEntry *e = o->extra ? o->extra->head : NULL; e; e = e->next) {\n");
        b_put(&g, "        if (!first) off += snprintf(buf + off, sizeof buf - off, \",\"); first = 0;\n");
        b_put(&g, "        off += snprintf(buf + off, sizeof buf - off, \"%%s:%%s\", e->key, mochi_val_json(e->val));\n");
        b_put(&g, "    }\n");
    }
    b_put(&g, "    snprintf(buf + off, sizeof buf - off, \"}\");\n");
    b_put(&g, "    return buf;\n}\n\n");
    b_put(&em->fn_extra, g.s);
    free(g.s);
}
/* Runtime key list for a shape: the static field names, then the overflow
 * map keys — exactly JS enumeration order. Powers for-in over an object and
 * Object.keys(obj); the static-field-only list the old for-in used missed
 * every runtime key. */
void ensure_keys_helpers(Emit *em, const char *sn) {
    if (!sn) return;
    for (int i = 0; i < em->keys_n; i++)
        if (strcmp(em->keys_shapes[i], sn) == 0) return;
    if (em->keys_n < 64) em->keys_shapes[em->keys_n++] = sn;
    Buf g; b_init(&g);
    b_putf(&g, "static mochi_array *%s_keys(const struct %s *o) {\n", sn, sn);
    b_putf(&g, "    mochi_array *k = mochi_array_new();\n");
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        b_putf(&g, "    mochi_array_push(k, mochi_str(\"%s\"));\n", fn);
    }
    if (cmc_dk_is(sn))
        b_put(&g, "    for (MochiMapEntry *e = o->extra ? o->extra->head : NULL; e; e = e->next)\n"
                  "        mochi_array_push(k, mochi_str(e->key));\n");
    b_put(&g, "    return k;\n}\n\n");
    /* membership test: `key in obj` */
    b_putf(&g, "static int %s_has(const struct %s *o, const char *k) {\n", sn, sn);
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        b_putf(&g, "    if (!strcmp(k, \"%s\")) return 1;\n", fn);
    }
    if (cmc_dk_is(sn)) b_put(&g, "    if (o->extra) return mochi_map_has(o->extra, k);\n");
    b_put(&g, "    return 0;\n}\n\n");
    /* values in key order: Object.values(obj) */
    b_putf(&g, "static mochi_array *%s_values(const struct %s *o) {\n", sn, sn);
    b_put(&g, "    mochi_array *v = mochi_array_new();\n");
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        Buf rhs; b_init(&rhs); b_putf(&rhs, "o->%s", fn);
        char *bx = dk_box(cmc_struct_field_kind(sn, fn), rhs.s);
        b_putf(&g, "    mochi_array_push(v, %s);\n", bx);
        free(bx); free(rhs.s);
    }
    if (cmc_dk_is(sn))
        b_put(&g, "    for (MochiMapEntry *e = o->extra ? o->extra->head : NULL; e; e = e->next)\n"
                  "        mochi_array_push(v, e->val);\n");
    b_put(&g, "    return v;\n}\n\n");
    /* [key, value] pairs: Object.entries(obj) */
    b_putf(&g, "static mochi_array *%s_entries(const struct %s *o) {\n", sn, sn);
    b_put(&g, "    mochi_array *e = mochi_array_new();\n");
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        Buf rhs; b_init(&rhs); b_putf(&rhs, "o->%s", fn);
        char *bx = dk_box(cmc_struct_field_kind(sn, fn), rhs.s);
        b_putf(&g, "    { mochi_array *pair = mochi_array_new(); mochi_array_push(pair, mochi_str(\"%s\")); "
                   "mochi_array_push(pair, %s); mochi_array_push(e, mochi_arr_box(pair)); }\n", fn, bx);
        free(bx); free(rhs.s);
    }
    if (cmc_dk_is(sn))
        b_put(&g, "    for (MochiMapEntry *m = o->extra ? o->extra->head : NULL; m; m = m->next) {\n"
                  "        mochi_array *pair = mochi_array_new();\n"
                  "        mochi_array_push(pair, mochi_str(m->key)); mochi_array_push(pair, m->val);\n"
                  "        mochi_array_push(e, mochi_arr_box(pair)); }\n");
    b_put(&g, "    return e;\n}\n\n");
    b_put(&em->fn_extra, g.s);
    free(g.s);
}
void ensure_dk_helpers(Emit *em, const char *sn) {
    if (!sn) return;
    for (int i = 0; i < em->dk_n; i++)
        if (strcmp(em->dk_shapes[i], sn) == 0) return;
    if (em->dk_n < 64) em->dk_shapes[em->dk_n++] = sn;
    cmc_dk_register(sn);
    Buf g; b_init(&g);
    b_putf(&g, "static mochi_val %s_get(const struct %s *o, const char *k) {\n", sn, sn);
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        Buf rhs; b_init(&rhs); b_putf(&rhs, "o->%s", fn);
        char *bx = dk_box(cmc_struct_field_kind(sn, fn), rhs.s);
        b_putf(&g, "    if (!strcmp(k, \"%s\")) return %s;\n", fn, bx);
        free(bx); free(rhs.s);
    }
    b_put(&g, "    if (o->extra) return mochi_map_get(o->extra, k);\n");
    b_put(&g, "    return mochi_undef();\n}\n");
    b_putf(&g, "static void %s_set(struct %s *o, const char *k, mochi_val v) {\n", sn, sn);
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(sn, f);
        if (!fn) break;
        const char *ub = dk_unbox(cmc_struct_field_kind(sn, fn));
        b_putf(&g, "    if (!strcmp(k, \"%s\")) { o->%s = %s; return; }\n", fn, fn, ub);
    }
    b_put(&g, "    if (!o->extra) o->extra = mochi_map_new();\n");
    b_put(&g, "    mochi_map_set(o->extra, k, v);\n}\n\n");
    b_put(&em->fn_extra, g.s);
    free(g.s);
}

EStr emit_member(Emit *em, CmcNode *n) {
    EStr o = emit_expr(em, n->obj);
    if (n->computed) {
        EStr idx = emit_expr(em, n->prop);
        if (o.type == CT_ARR) {
            Buf b; b_init(&b);
            b_putf(&b, "mochi_array_get(%s, %s)", o.code, idx.code);
            return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
        }
        if (o.type == CT_STR) {
            Buf b; b_init(&b);
            b_putf(&b, "mochi_char_at(%s, %s)", o.code, idx.code);
            return estr(b.s, CT_STR, 0, 1, NULL);
        }
        /* object + compile-time constant key -> static field access */
        if (o.type == CT_OBJ) {
            const char *k = kfold(em, n->prop);
            if (k) return emit_member_field(em, o, n->obj, k, n->optional);
            /* runtime key: dispatch via the shape's get helper (dynamic typing) */
            if (o.struct_name) {
                ensure_dk_helpers(em, o.struct_name);
                Buf ks; b_init(&ks); co_str(em, &ks, idx);
                Buf b; b_init(&b);
                b_putf(&b, "%s_get(%s, %s)", o.struct_name, o.code, ks.s);
                free(ks.s);
                return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
            }
            fprintf(stderr, "cmochi:%d:%d: warning: dynamic computed key on object unsupported (no static shape)\n", n->line, n->col);
            return estr("0", CT_UNKNOWN, 0, 0, NULL);
        }
        Buf b; b_init(&b);
        b_putf(&b, "%s", o.code);
        return estr(b.s, CT_UNKNOWN, o.escapes, o.ptr, NULL);
    }
    const char *prop = n->prop ? n->prop->name : "";
    return emit_member_field(em, o, n->obj, prop, n->optional);
}

EStr emit_bin(Emit *em, CmcNode *n) {
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
            /* a mochi_val operand is unboxed to a double at runtime, so the
             * result is a double too (printing it with %d reads the varargs
             * as an int -> garbage) */
            if (l.type == CT_MOCHI_VAL || r.type == CT_MOCHI_VAL) {
                b_putf(&b, "(%s + %s)", ln.s, rn.s);
                free(ln.s); free(rn.s);
                return estr(b.s, CT_DOUBLE, 0, 0, NULL);
            }
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
        case TK_IN: {
            /* `key in obj`: runtime membership over the shape's fields and
             * its overflow map */
            if (r.type == CT_OBJ && r.struct_name) {
                ensure_keys_helpers(em, r.struct_name);
                Buf ks; b_init(&ks); co_str(em, &ks, l);
                b_putf(&b, "%s_has(%s, %s)", r.struct_name, r.code, ks.s);
                free(ks.s);
                return estr(b.s, CT_BOOL, 0, 0, NULL);
            }
            fprintf(stderr, "cmochi: warning: the `in` operator needs an object on the right side\n");
            b_putf(&b, "(0)");
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

EStr emit_unary(Emit *em, CmcNode *n) {
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
        case TK_DELETE: {
            /* delete obj[k] / obj.key:
             *  - a runtime key lives in the overflow map -> mochi_map_del
             *  - a static field cannot vanish from the C struct, so it widens
             *    to mochi_val and is set to undefined (JS reads undefined) */
            CmcNode *lhs = n->lhs;
            if (lhs && lhs->kind == N_MEMBER && lhs->obj) {
                EStr o = emit_expr(em, lhs->obj);
                if (o.type == CT_OBJ && o.struct_name) {
                    const char *sn = o.struct_name;
                    const char *key = NULL;
                    if (lhs->computed) key = kfold(em, lhs->prop);
                    else if (lhs->prop) key = lhs->prop->name;
                    if (lhs->computed && !key) {
                        ensure_dk_helpers(em, sn);
                        EStr idx = emit_expr(em, lhs->prop);
                        Buf ks; b_init(&ks); co_str(em, &ks, idx);
                        b_putf(&b, "(mochi_map_del(%s->extra, %s), 1)", o.code, ks.s);
                        free(ks.s);
                        return estr(b.s, CT_BOOL, 0, 0, NULL);
                    }
                    if (key) {
                        const char *ft = cmc_struct_field_type(sn, key);
                        if (!ft) {
                            ensure_dk_helpers(em, sn);
                            b_putf(&b, "(mochi_map_del(%s->extra, \"%s\"), 1)", o.code, key);
                            return estr(b.s, CT_BOOL, 0, 0, NULL);
                        }
                        if (strcmp(ft, "mochi_val") != 0)
                            cmc_shape_widen_field(sn, key, CT_MOCHI_VAL);
                        b_putf(&b, "(%s->%s = mochi_undef(), 1)", o.code, key);
                        return estr(b.s, CT_BOOL, 0, 0, NULL);
                    }
                }
                fprintf(stderr, "cmochi: warning: delete on a non-object property is not supported\n");
            }
            b_putf(&b, "(1)");
            return estr(b.s, CT_BOOL, 0, 0, NULL);
        }
        case TK_VOID: b_putf(&b, "(0)"); return estr(b.s, CT_UNDEF, 0, 0, NULL);
        default: b_put(&b, a.code); return estr(b.s, a.type, a.escapes, a.ptr, NULL);
    }
}

/* The hybrid static/dynamic typing rule: a field's static type can only be
 * kept when the assigned value fits it faithfully. int -> double / int64 is
 * a static promotion (fast path kept); any genuinely incompatible pair (a
 * number field given a string, a string field given a number, etc.) widens
 * to CT_MOCHI_VAL — the dynamic tagged value. */
static CmcType widen_field_type(CmcType fk, CmcType rt) {
    if (fk == rt) return fk;
    if (rt == CT_UNKNOWN) return fk;
    if (fk == CT_INT && rt == CT_DOUBLE) return CT_DOUBLE;
    if (fk == CT_INT && rt == CT_INT64) return CT_INT64;
    if (fk == CT_BOOL && rt == CT_DOUBLE) return CT_DOUBLE;
    if (fk == CT_BOOL && rt == CT_INT) return CT_INT;
    if (fk == CT_BOOL && rt == CT_INT64) return CT_INT64;
    if (fk == CT_MOCHI_VAL) return CT_MOCHI_VAL;
    if (fk == CT_NULL || fk == CT_UNDEF) return fk;
    if (cmc_is_numeric(fk) && (rt == CT_STR || rt == CT_BOOL || rt == CT_ARR ||
        rt == CT_OBJ || rt == CT_NULL || rt == CT_UNDEF || rt == CT_MOCHI_VAL)) return CT_MOCHI_VAL;
    if (fk == CT_STR && (cmc_is_numeric(rt) || rt == CT_BOOL || rt == CT_ARR ||
        rt == CT_OBJ || rt == CT_NULL || rt == CT_UNDEF || rt == CT_MOCHI_VAL)) return CT_MOCHI_VAL;
    if ((fk == CT_ARR || fk == CT_OBJ) && !(rt == CT_ARR || rt == CT_OBJ || rt == CT_NULL || rt == CT_UNDEF))
        return CT_MOCHI_VAL;
    return fk;
}

static void emit_member_assign(Emit *em, Buf *b, EStr o, const char *prop, EStr r) {
    Buf base; b_init(&base); member_base(em, &base, o);
    Buf rv; b_init(&rv);
    const char *ft = o.struct_name ? cmc_struct_field_type(o.struct_name, prop) : NULL;
    if (!ft && o.struct_name && cmc_dk_is(o.struct_name)) {
        /* The shape has a dynamic overflow map: keep this key OUT of the
         * struct so the map preserves JS insertion order (and so a literal
         * key and a runtime key interleave the way JS does). */
        ensure_dk_helpers(em, o.struct_name);
        Buf v; b_init(&v); co_box(&v, r);
        b_putf(b, "%s_set(%s, \"%s\", %s)", o.struct_name, o.code, prop, v.s);
        free(v.s); free(base.s);
        return;
    }
    if (!ft && o.struct_name) {
        /* JS allows point.label = "P" on a shape that only had {x, y}:
         * merge the field into the registered shape so the struct def
         * (emitted at assembly) grows to include it. */
        CmcType vt = r.type;
        if (vt == CT_UNKNOWN) vt = CT_DOUBLE;
        cmc_shape_add_field(o.struct_name, prop, vt);
        cmc_shape_mark_promoted(o.struct_name, prop);
        ft = cmc_struct_field_type(o.struct_name, prop);
    }
    if (o.struct_name && ft && strcmp(ft, "mochi_val") != 0) {
        CmcType fk = cmc_struct_field_kind(o.struct_name, prop);
        CmcType target = widen_field_type(fk, r.type);
        if (target != fk) {
            cmc_shape_widen_field(o.struct_name, prop, target);
            ft = cmc_struct_field_type(o.struct_name, prop);
        }
    }
    if (ft && strcmp(ft, "mochi_val") == 0) co_box(&rv, r);
    else if (ft && strcmp(ft, "const char *") == 0) co_str(em, &rv, r);
    else if (ft && strcmp(ft, "int") == 0) co_int(&rv, r);
    else if (ft && strcmp(ft, "int64_t") == 0) { if (r.type == CT_INT64) b_put(&rv, r.code); else b_putf(&rv, "(int64_t)(%s)", r.code); }
    else if (ft && strcmp(ft, "double") == 0) co_num(em, &rv, r);
    else if (r.type == CT_STR) co_str(em, &rv, r);
    else if (cmc_is_numeric(r.type)) co_num(em, &rv, r);
    else b_put(&rv, r.code);
    b_putf(b, "%s%s = %s", base.s, prop, rv.s);
    free(base.s); free(rv.s);
}

EStr emit_assign(Emit *em, CmcNode *n) {
    EStr r = emit_expr(em, n->rhs);
    Buf b; b_init(&b);
    if (n->lhs->kind == N_IDENT) {
        const char *cap = capt_lvalue(em, n->lhs->name);
        if (cap) {
            char cb2[128];
            if (capt_is_ref(em, n->lhs->name))
                snprintf(cb2, sizeof cb2, "*((struct %s *)__env)->%s", cap, n->lhs->name);
            else
                snprintf(cb2, sizeof cb2, "((struct %s *)__env)->%s", cap, n->lhs->name);
            if (n->op == TK_EQ) {
                b_putf(&b, "%s = ", cb2);
                CmcType ct = CT_UNKNOWN;
                for (int i = 0; i < em->cur_ncapt; i++)
                    if (!strcmp(em->cur_capt[i], n->lhs->name)) ct = em->cur_capt_type[i];
                Buf rv; b_init(&rv);
                if (ct == CT_STR) co_str(em, &rv, r);
                else if (cmc_is_numeric(ct)) co_num(em, &rv, r);
                else b_put(&rv, r.code);
                b_put(&b, rv.s);
                free(rv.s);
            } else {
                b_putf(&b, "%s = %s", cb2, cb2);
                Buf rv; b_init(&rv);
                if (cmc_is_numeric(r.type)) co_num(em, &rv, r); else b_put(&rv, r.code);
                b_putf(&b, " + %s", rv.s);
                free(rv.s);
            }
            CmcType ct2 = CT_UNKNOWN;
            for (int i = 0; i < em->cur_ncapt; i++)
                if (!strcmp(em->cur_capt[i], n->lhs->name)) ct2 = em->cur_capt_type[i];
            return estr(b.s, ct2 == CT_UNKNOWN ? CT_DOUBLE : ct2, 0, 0, NULL);
        }
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
            /* object + compile-time constant key -> static field assignment */
            if (o.type == CT_OBJ) {
                const char *k = kfold(em, m->prop);
                if (k) {
                    emit_member_assign(em, &b, o, k, r);
                    return estr(b.s, CT_UNKNOWN, 0, 0, NULL);
                }
                /* runtime key: dispatch via the shape's set helper (dynamic typing) */
                if (o.struct_name) {
                    ensure_dk_helpers(em, o.struct_name);
                    Buf ks; b_init(&ks); co_str(em, &ks, idx);
                    Buf v; b_init(&v); co_box(&v, r);
                    Buf b2; b_init(&b2);
                    b_putf(&b2, "%s_set(%s, %s, %s)", o.struct_name, o.code, ks.s, v.s);
                    free(ks.s); free(v.s);
                    return estr(b2.s, CT_MOCHI_VAL, 0, 0, NULL);
                }
                fprintf(stderr, "cmochi:%d:%d: warning: dynamic computed key on object unsupported (no static shape)\n", m->line, m->col);
                return estr("0", CT_UNKNOWN, 0, 0, NULL);
            }
            Buf v; b_init(&v); co_box(&v, r);
            b_putf(&b, "mochi_array_set(%s, %s, %s)", o.code, idx.code, v.s);
            free(v.s);
            return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
        }
        emit_member_assign(em, &b, o, m->prop->name, r);
        return estr(b.s, CT_UNKNOWN, 0, 0, NULL);
    }
    b_put(&b, r.code);
    return estr(b.s, r.type, r.escapes, r.ptr, NULL);
}

EStr emit_update(Emit *em, CmcNode *n) {
    EStr a = emit_expr(em, n->lhs);
    Buf b; b_init(&b);
    if (n->lhs->kind == N_IDENT) {
        const char *capu = capt_lvalue(em, n->lhs->name);
        if (capu) {
            char cb3[128];
            if (capt_is_ref(em, n->lhs->name))
                snprintf(cb3, sizeof cb3, "*((struct %s *)__env)->%s", capu, n->lhs->name);
            else
                snprintf(cb3, sizeof cb3, "((struct %s *)__env)->%s", capu, n->lhs->name);
            b_putf(&b, "(%s %s= 1)", cb3, n->op > 0 ? "+" : "-");
            return estr(b.s, CT_INT, 0, 0, NULL);
        }
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

EStr emit_cond(Emit *em, CmcNode *n) {
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

EStr emit_array(Emit *em, CmcNode *n) {
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

EStr emit_object(Emit *em, CmcNode *n) {
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
        if (!ss) {
            /* unknown source type: its fields cannot be enumerated statically.
               Warn loudly instead of silently dropping the spread. */
            fprintf(stderr, "cmochi:%d:%d: warning: object spread source has no static shape "
                            "(fields not copied); give the source a literal/object type\n",
                    n->line, n->col);
            continue;
        }
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
                if (!ss) continue;   /* already warned in the merge loop */
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
    if (cmc_dk_is(sn)) { Buf xinit; b_init(&xinit); b_putf(&xinit, "%s->extra = NULL;", obj);
                         e_preline(em, xinit.s); free(xinit.s); }
    /* flush pending pre-lines now: a lifted method fn must not capture them */
    flush_pre(em);
    /* copy spread source fields, then literal props (later wins) */
    for (int i = 0; i < np; i++) {
        CmcProp *pr = &n->props[i];
        if (pr->spread) {
            const char *ss = vals[i].struct_name;
            if (!ss) continue;   /* already warned in the merge loop */
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
            /* shorthand method: lift to a static fn, record the inline mapping.
             * Flush first: the lifted fn must not capture this literal's
             * pending field assignments. */
            flush_pre(em);
            EStr fe = emit_expr(em, pr->value);
            /* emit_arrow returns a box expression `mochi_fn_box_n(__shim_FN,
             * NULL, "FN")` — extract FN so call sites invoke the C function
             * directly rather than calling the box value. */
            const char *feC = fe.code;
            {
                const char *sh = strstr(fe.code, "__shim_");
                if (sh) {
                    sh += 7;
                    const char *q = strchr(sh, ',');
                    if (q) {
                        size_t fl = (size_t)(q - sh);
                        char *fnn = (char *)malloc(fl + 1);
                        memcpy(fnn, sh, fl); fnn[fl] = 0;
                        feC = fnn;
                    }
                }
            }
            if (em->m_n < 64) {
                em->m_shape[em->m_n] = sn;
                em->m_field[em->m_n] = pr->key;
                em->m_fn[em->m_n] = feC;
                em->m_node[em->m_n] = pr->value;
                em->m_guarded[em->m_n] = 1; /* arrow lifted w/ has_X guards */
                em->m_nparams[em->m_n] = pr->value->nparams;
                em->m_ret[em->m_n] = CT_INT;
                em->m_n++;
            }
            continue; /* no struct field assignment; calls are inlined */
        }
        Buf v; b_init(&v);
        const char *ft = cmc_struct_field_type(sn, pr->key);
        if (!ft) ft = "double";
        if (strcmp(ft, "mochi_val") == 0) co_box(&v, vals[i]);
        else if (strcmp(ft, "const char *") == 0) co_str(em, &v, vals[i]);
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
    /* The field assignments are pre-lines: flush them HERE so they stay at
     * the literal's site (a returned literal must not leak its assignments
     * into the caller). */
    flush_pre(em);
    return estr(obj, CT_OBJ, 1, 1, sn);
}

EStr emit_template(Emit *em, CmcNode *n) {
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

EStr emit_arrow(Emit *em, CmcNode *n);

/* ---------- closure capture ----------
 * A nested arrow that references a name declared in an enclosing function
 * (a param or a local, NOT a global) must capture it. We use snapshot
 * semantics: the value is copied into a heap env struct when the function
 * value is created, and the arrow reads/writes its own copy through env.
 * (JS captures by reference; snapshot is a documented approximation that
 * matches `let`-style iteration capture.) */
typedef struct { const char *name; CmcType type; const char *sn; } CmcFreeVar;
static void fv_walk(CmcNode *nd, CmcFreeVar *out, int *cnt);
static int fv_is_param(CmcNode *n, const char *name);
static int fv_is_declared(CmcNode *body, const char *name);

/* the shared cell for a by-reference captured name (NULL if snapshot) */
const char *ref_cell_for(Emit *em, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < em->ref_n; i++)
        if (em->ref_name[i] && !strcmp(em->ref_name[i], name)) return em->ref_cell[i];
    return NULL;
}

/* does any closure in this body capture `name` from the enclosing scope? */
int body_captures_name(Emit *em, CmcNode *body, const char *name) {
    if (!body || !name) return 0;
    CmcNode *stack[512]; int sp = 0; stack[sp++] = body;
    while (sp) {
        CmcNode *cur = stack[--sp];
        if (!cur) continue;
        if (cur->kind == N_ARROW || cur->kind == N_FUNC) {
            CmcFreeVar fv[16]; int nfv = 0;
            fv_walk(cur->body, fv, &nfv);
            for (int i = 0; i < nfv; i++) {
                if (!fv[i].name || strcmp(fv[i].name, name) != 0) continue;
                if (fv_is_param(cur, name)) continue;
                if (fv_is_declared(cur->body, name)) continue;
                CmcSym *cs = cmc_env_lookup(&em->env, name);
                if (cs && cs->kind != SYM_FUNC && cs->kind != SYM_GLOBAL) return 1;
            }
            continue; /* the arrow's own body is a separate scope */
        }
        if (cur->lhs) stack[sp++] = cur->lhs;
        if (cur->rhs) stack[sp++] = cur->rhs;
        if (cur->obj) stack[sp++] = cur->obj;
        if (cur->prop) stack[sp++] = cur->prop;
        if (cur->cons) stack[sp++] = cur->cons;
        if (cur->alt) stack[sp++] = cur->alt;
        if (cur->third) stack[sp++] = cur->third;
        if (cur->init) stack[sp++] = cur->init;
        if (cur->test) stack[sp++] = cur->test;
        if (cur->update) stack[sp++] = cur->update;
        if (cur->stmt) stack[sp++] = cur->stmt;
        if (cur->body) stack[sp++] = cur->body;
        if (cur->left) stack[sp++] = cur->left;
        for (int i = 0; i < cur->nargs; i++) stack[sp++] = cur->args[i];
        for (int i = 0; i < cur->nstmts; i++) stack[sp++] = cur->stmts[i];
        for (int i = 0; i < cur->nelems; i++) stack[sp++] = cur->elems[i];
        for (int i = 0; i < cur->nprops; i++) if (cur->props[i].value) stack[sp++] = cur->props[i].value;
        for (int i = 0; i < cur->nexprs; i++) stack[sp++] = cur->exprs[i];
    }
    return 0;
}

int arrow_captures(Emit *em, CmcNode *arrow) {
    if (!arrow || (arrow->kind != N_ARROW && arrow->kind != N_FUNC)) return 0;
    CmcFreeVar fv[16]; int nfv = 0;
    fv_walk(arrow->body, fv, &nfv);
    for (int i = 0; i < nfv; i++) {
        if (fv_is_param(arrow, fv[i].name)) continue;
        if (fv_is_declared(arrow->body, fv[i].name)) continue;
        CmcSym *cs = cmc_env_lookup(&em->env, fv[i].name);
        if (cs && cs->kind != SYM_FUNC && cs->kind != SYM_GLOBAL) return 1;
    }
    return 0;
}

static void fv_walk(CmcNode *nd, CmcFreeVar *out, int *cnt) {
    if (!nd || *cnt >= 16) return;
    switch (nd->kind) {
        case N_IDENT:
            if (nd->name) { out[*cnt].name = nd->name; out[*cnt].type = CT_UNKNOWN; out[*cnt].sn = NULL; (*cnt)++; }
            return;
        case N_MEMBER: fv_walk(nd->obj, out, cnt); if (!nd->computed) { } else fv_walk(nd->prop, out, cnt); return;
        case N_CALL: fv_walk(nd->obj, out, cnt); for (int i = 0; i < nd->nargs; i++) fv_walk(nd->args[i], out, cnt); return;
        case N_BIN: case N_ASSIGN: case N_NULLISH:
            fv_walk(nd->lhs, out, cnt); fv_walk(nd->rhs, out, cnt); return;
        case N_UNARY: case N_UPDATE: case N_THROW: fv_walk(nd->lhs, out, cnt); return;
        case N_COND: fv_walk(nd->cons, out, cnt); fv_walk(nd->alt, out, cnt); fv_walk(nd->third, out, cnt); return;
        case N_ARR: for (int i = 0; i < nd->nelems; i++) fv_walk(nd->elems[i], out, cnt); return;
        case N_OBJ: for (int i = 0; i < nd->nprops; i++) { if (nd->props[i].value) fv_walk(nd->props[i].value, out, cnt); } return;
        case N_TEMPLATE: for (int i = 0; i < nd->nexprs; i++) fv_walk(nd->exprs[i], out, cnt); return;
        case N_SEQ: for (int i = 0; i < nd->nstmts; i++) fv_walk(nd->stmts[i], out, cnt); return;
        case N_RETURN: fv_walk(nd->lhs, out, cnt); return;
        case N_IF: fv_walk(nd->cons, out, cnt); fv_walk(nd->alt, out, cnt); return;
        case N_WHILE: fv_walk(nd->test, out, cnt); fv_walk(nd->stmt, out, cnt); return;
        case N_FOR: fv_walk(nd->init, out, cnt); fv_walk(nd->test, out, cnt); fv_walk(nd->update, out, cnt); fv_walk(nd->stmt, out, cnt); return;
        case N_FOROF: case N_FORIN: fv_walk(nd->left, out, cnt); fv_walk(nd->obj, out, cnt); fv_walk(nd->stmt, out, cnt); return;
        case N_BLOCK: for (int i = 0; i < nd->nstmts; i++) fv_walk(nd->stmts[i], out, cnt); return;
        case N_VAR: for (int i = 0; i < nd->nstmts; i++) fv_walk(nd->stmts[i], out, cnt); return;
        case N_EXPRSTMT: fv_walk(nd->lhs, out, cnt); return;
        case N_ARROW: case N_FUNC:
            /* descend: a nested closure's free vars that resolve to THIS
             * function's scope must be captured transitively. */
            fv_walk(nd->body, out, cnt); return;
        case N_CLASS: return; /* methods have their own this/scope */
        default: return;
    }
}
static int fv_is_param(CmcNode *n, const char *name) {
    for (int i = 0; i < n->nparams; i++) {
        const char *pn = n->params[i]->name;
        if (pn && !strcmp(pn, name)) return 1;
    }
    return 0;
}
static int fv_is_declared(CmcNode *body, const char *name) {
    /* var/let/const declared directly in the arrow body */
    if (!body) return 0;
    if (body->kind == N_BLOCK) {
        for (int i = 0; i < body->nstmts; i++) {
            CmcNode *st = body->stmts[i];
            if (st->kind == N_VAR)
                for (int j = 0; j < st->nstmts; j++) {
                    const char *vn = st->stmts[j]->name;
                    if (vn && !strcmp(vn, name)) return 1;
                }
        }
    } else if (body->kind == N_VAR) {
        for (int j = 0; j < body->nstmts; j++) {
            const char *vn = body->stmts[j]->name;
            if (vn && !strcmp(vn, name)) return 1;
        }
    }
    return 0;
}
EStr emit_stored_fn_call(Emit *em, CmcNode *n, EStr o);
EStr emit_call(Emit *em, CmcNode *n);

/* a captured name read/written as an lvalue inside the arrow body */
static int capt_is_ref(Emit *em, const char *name) {
    if (!em->cur_up || em->cur_ncapt <= 0 || !name) return 0;
    for (int i = 0; i < em->cur_ncapt; i++)
        if (em->cur_capt[i] && !strcmp(em->cur_capt[i], name)) return em->cur_capt_ref[i];
    return 0;
}
static const char *capt_lvalue(Emit *em, const char *name) {
    if (!em->cur_up || em->cur_ncapt <= 0 || !name) return NULL;
    for (int i = 0; i < em->cur_ncapt; i++)
        if (em->cur_capt[i] && !strcmp(em->cur_capt[i], name)) return em->cur_up;
    return NULL;
}

EStr emit_arrow(Emit *em, CmcNode *n) {
    /* the enclosing function's capture set: a name it captured must be read
     * through ITS env at this boxing site (nested closures). */
    const char *enc_up = em->cur_up;
    const char *enc_capt[16];
    for (int i = 0; i < em->cur_ncapt && i < 16; i++) enc_capt[i] = em->cur_capt[i];
    int enc_ncap = em->cur_ncapt;
    char fnbuf[64];
    snprintf(fnbuf, sizeof fnbuf, "__cmc_fn_%d", em->fnid++);
    const char *fnname = cmc_arena_strndup(em->ctx->arena, fnbuf, strlen(fnbuf));
    int np = n->nparams ? n->nparams : 0;
    CmcType *pt = (CmcType *)malloc(sizeof(CmcType) * (np ? np : 1));
    int *prest = (int *)malloc(sizeof(int) * (np ? np : 1));
    /* resolve each param's name + whether it has a default */
    const char **pname = (const char **)malloc(sizeof(char *) * (np ? np : 1));
    CmcNode **pdef = (CmcNode **)malloc(sizeof(CmcNode *) * (np ? np : 1));
    for (int i = 0; i < np; i++) {
        prest[i] = n->params[i]->is_rest ? 1 : 0;
        if (n->params[i]->kind == N_IDENT) {
            pname[i] = n->params[i]->name; pdef[i] = NULL;
            pt[i] = infer_param_type_lite(em, n, pname[i]);
        } else if (n->params[i]->kind == N_ASSIGN && n->params[i]->lhs->kind == N_IDENT) {
            pname[i] = n->params[i]->lhs->name; pdef[i] = n->params[i]->rhs;
            pt[i] = cmc_type_of(em->ctx, &em->env, pdef[i]);
            if (pt[i] == CT_UNKNOWN || pt[i] == CT_VOID) pt[i] = CT_DOUBLE;
        } else { pname[i] = "p"; pdef[i] = NULL; pt[i] = CT_DOUBLE; }
    }
    /* block-bodied arrows: infer the return type from their return statements
     * (fn_return_type knows that returning a function means CT_MOCHI_VAL) */
    CmcType ret = n->is_expr_body ? cmc_type_of(em->ctx, &em->env, n->body) : fn_return_type(n);
    if (ret == CT_UNKNOWN || ret == CT_VOID) ret = CT_INT;

    /* closure capture: names used in the body that come from an enclosing
     * function scope (params/locals), not declared here, not global. */
    CmcFreeVar fv[16]; int nfv = 0;
    fv_walk(n->body, fv, &nfv);
    /* resolve each candidate's type from the CURRENT (enclosing) env */
    {
        CmcFreeVar cap[16]; int ncap = 0;
        for (int i = 0; i < nfv; i++) {
            if (fv_is_param(n, fv[i].name)) continue;
            if (fv_is_declared(n->body, fv[i].name)) continue;
            int dup = 0;
            for (int j = 0; j < ncap; j++) if (!strcmp(cap[j].name, fv[i].name)) { dup = 1; break; }
            if (dup) continue;
            CmcSym *cs = cmc_env_lookup(&em->env, fv[i].name);
            if (!cs || cs->kind == SYM_FUNC || cs->kind == SYM_GLOBAL) continue;
            cap[ncap].name = fv[i].name; cap[ncap].type = cs->type; cap[ncap].sn = cs->struct_name;
            ncap++;
        }
        nfv = ncap;
        for (int i = 0; i < ncap; i++) fv[i] = cap[i];
    }
    const char *upstruct = NULL;
    int capref[16];
    if (nfv > 0) {
        for (int i = 0; i < nfv; i++) capref[i] = 0;
        upstruct = (char *)0;
        Buf us; b_init(&us);
        b_putf(&us, "struct __up_%s {\n", fnname);
        for (int i = 0; i < nfv; i++) {
            CmcSym *cs = cmc_env_lookup(&em->env, fv[i].name);
            CmcType t = cs ? cs->type : CT_DOUBLE;
            const char *tn = cmc_type_name(t);
            if (t == CT_STR) tn = "const char *";
            if (t == CT_OBJ) { const char *sh = cs ? cs->struct_name : NULL;
                if (sh) { Buf tnb; b_init(&tnb); b_putf(&tnb, "struct %s *", sh); tn = cmc_arena_strndup(em->ctx->arena, tnb.s, tnb.len); free(tnb.s); } }
            /* by-reference capture: the field holds the shared cell pointer */
            if (ref_cell_for(em, fv[i].name)) {
                Buf pb; b_init(&pb);
                b_putf(&pb, "%s *", tn);
                tn = cmc_arena_strndup(em->ctx->arena, pb.s, pb.len);
                free(pb.s);
                capref[i] = 1;
            }
            b_putf(&us, "    %s %s;\n", tn, fv[i].name);
        }
        b_put(&us, "};\n");
        b_put(&em->fn_extra, us.s);
        free(us.s);
        upstruct = cmc_arena_strndup(em->ctx->arena, (char[]){0}, 0);
        /* store the struct name as a string */
        char un[64]; snprintf(un, sizeof un, "__up_%s", fnname);
        upstruct = cmc_arena_strndup(em->ctx->arena, un, strlen(un));
    }

    vol_pass(em, n->body);
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
        if (prest[i]) {
            /* rest param: a real mochi_array* — args[]/args.length resolve like an array */
            b_putf(&sig, "mochi_array *%s", pname[i]);
            cmc_env_decl(&em->env, pname[i], CT_ARR, SYM_PARAM);
            continue;
        }
        b_putf(&sig, "int %s, %s %s", hasname, cmc_type_name(pt[i]), pname[i]);
        CmcSym *ps = cmc_env_decl(&em->env, pname[i], pt[i], SYM_PARAM);
        ps->has_name = hasname;
        ps->func_node = n;
        CmcSym *hs = cmc_env_decl(&em->env, hasname, CT_BOOL, SYM_PARAM);
        hs->has_name = hasname;
    }
    if (nfv > 0) { if (np > 0) b_put(&sig, ", "); b_put(&sig, "void *__env"); }
    b_put(&sig, ") {");
    /* set the current function's capture set for emit_ident to rewrite */
    em->cur_up = upstruct; em->cur_ncapt = nfv;
    for (int i = 0; i < nfv; i++) { em->cur_capt[i] = fv[i].name; em->cur_capt_type[i] = fv[i].type; }
    if (upstruct) for (int i = 0; i < nfv; i++) em->cur_capt_ref[i] = capref[i];
    e_line(em, sig.s);
    free(sig.s);
    em->indent++;
    /* default-value resets: if (!has_X) X = default; */
    for (int i = 0; i < np; i++) {
        if (!pdef[i] || prest[i]) continue;
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
    em->cur_up = NULL; em->cur_ncapt = 0;
    /* re-read em->out: a realloc during emission left body.s stale */
    body = em->out;
    em->out = save;
    if (!em->fn_extra.s) b_init(&em->fn_extra);
    b_put(&em->fn_extra, body.s);
    b_put(&em->fn_extra, "\n");
    free(body.s);
    /* first-class shim for this arrow (nullish-guarded signature) */
    {
        Buf sh; b_init(&sh);
        b_putf(&sh, "static %s %s(", cmc_type_name(ret), fnname);
        for (int i = 0; i < np; i++) {
            if (i) b_put(&sh, ", ");
            if (prest[i]) b_putf(&sh, "mochi_array *%s", pname[i]);
            else b_putf(&sh, "int has_%s, %s %s", pname[i], cmc_type_name(pt[i]), pname[i]);
        }
        if (nfv > 0) { if (np > 0) b_put(&sh, ", "); b_put(&sh, "void *__env"); }
        b_put(&sh, ");\n");
        b_putf(&sh, "static mochi_val __shim_%s(int argc, mochi_val *argv, void *env) {\n", fnname);
        b_put(&sh, "    (void)argc; (void)env;\n");
        b_put(&sh, "    ");
        b_putf(&sh, "%s __r = %s(", cmc_type_name(ret), fnname);
        for (int i = 0; i < np; i++) {
            if (i) b_put(&sh, ", ");
            if (prest[i]) {
                b_putf(&sh, "(mochi_array_from_argv(argv, %d, argc))", i);
            } else if (pt[i] == CT_STR) {
                b_putf(&sh, "(argc > %d ? 1 : 0), (argc > %d ? mochi_str_or_empty(argv[%d]) : \"\")", i, i, i);
            } else if (pt[i] == CT_ARR) {
                b_putf(&sh, "(argc > %d ? 1 : 0), (argc > %d ? mochi_arr_or_empty(argv[%d]) : NULL)", i, i, i);
            } else if (pt[i] == CT_OBJ) {
                b_putf(&sh, "(argc > %d ? 1 : 0), (argc > %d && argv[%d].p ? (struct %s *)argv[%d].p : NULL)",
                       i, i, i, pname[i] ? pname[i] : "CmcStruct_1", i);
            } else if (pt[i] == CT_MOCHI_VAL) {
                b_putf(&sh, "(argc > %d ? 1 : 0), (argc > %d ? argv[%d] : mochi_undef())", i, i, i);
            } else if (pt[i] == CT_BOOL) {
                b_putf(&sh, "(argc > %d ? 1 : 0), (argc > %d ? mochi_val_bool(argv[%d]) : 0)", i, i, i);
            } else {
                b_putf(&sh, "(argc > %d ? 1 : 0), (argc > %d ? mochi_val_num(argv[%d]) : 0)", i, i, i);
            }
        }
        if (nfv > 0) { if (np > 0) b_put(&sh, ", "); b_put(&sh, "env"); }
        b_put(&sh, ");\n");
        b_put(&sh, "    ");
        if (ret == CT_STR) b_put(&sh, "return mochi_str(__r);\n");
        else if (ret == CT_BOOL) b_put(&sh, "return mochi_bool(__r);\n");
        else if (ret == CT_ARR) b_put(&sh, "return mochi_arr_box(__r);\n");
        else if (ret == CT_OBJ) b_put(&sh, "return mochi_obj_box(__r);\n");
        else if (ret == CT_MOCHI_VAL) b_put(&sh, "return __r;\n");
        else b_put(&sh, "return mochi_num((double)__r);\n");
        b_put(&sh, "}\n");
        b_put(&em->fn_extra, sh.s);
        free(sh.s);
    }
    free(pt); free(pname); free(pdef);
    char boxbuf[96];
    if (nfv > 0) {
        /* allocate the env struct and snapshot the captured locals now */
        const char *up = tmp_name(em, "up");
        Buf mk; b_init(&mk);
        b_putf(&mk, "struct %s *%s = (struct %s *)CMC_ALLOC(sizeof(struct %s));", upstruct, up, upstruct, upstruct);
        /* emitted straight into the enclosing body: em->out is already restored */
        e_line(em, mk.s); free(mk.s);
        for (int i = 0; i < nfv; i++) {
            Buf st; b_init(&st);
            /* the captured name is in scope at the boxing site (enclosing fn) —
             * unless THIS arrow captured it too, in which case read it from
             * our own env (nested closure). */
            const char *src = fv[i].name;
            char nb2[128];
            for (int j = 0; j < enc_ncap; j++) {
                if (enc_capt[j] && !strcmp(enc_capt[j], fv[i].name) && enc_up) {
                    snprintf(nb2, sizeof nb2, "((struct %s *)__env)->%s", enc_up, fv[i].name);
                    src = cmc_arena_strndup(em->ctx->arena, nb2, strlen(nb2));
                    break;
                }
            }
            if (capref[i]) {
                const char *rc = ref_cell_for(em, fv[i].name);
                Buf pp; b_init(&pp);
                b_putf(&pp, "%s->%s = %s;", up, fv[i].name, rc);
                e_line(em, pp.s); free(pp.s);
                continue;
            }
            b_putf(&st, "%s->%s = %s;", up, fv[i].name, src);
            e_line(em, st.s); free(st.s);
        }
        snprintf(boxbuf, sizeof boxbuf, "mochi_fn_box_n(__shim_%s, %s, \"%s\")", fnname, up, fnname);
    } else {
        snprintf(boxbuf, sizeof boxbuf, "mochi_fn_box_n(__shim_%s, NULL, \"%s\")", fnname, fnname);
    }
    const char *box = cmc_arena_strndup(em->ctx->arena, boxbuf, strlen(boxbuf));
    return estr(box, CT_MOCHI_VAL, 0, 0, NULL);
}

/* nullish-ness guard for a call argument: writes a C int expression that is
 * 1 when the argument is definitely non-nullish, 0 when null/undefined. */

void arg_has(Emit *em, CmcNode *arg, Buf *out)
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

EStr emit_arr_higher(Emit *em, CmcNode *n, const char *prop, EStr o) {
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
    int result_is_bool = 0;

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
    } else if (strcmp(prop, "some") == 0 || strcmp(prop, "every") == 0 || strcmp(prop, "find") == 0) {
        char res[32]; snprintf(res, sizeof res, "__r%d", em->tmp); em->tmp++;
        b_putf(&out_loop, "%sdouble %s = %s;\n", ind, res, (strcmp(prop, "every") == 0) ? "1" : "0");
        b_putf(&out_loop, "%sfor (size_t %s = 0; %s < (%s)->len; %s++) {\n", ind, li, li, o.code, li);
        b_putf(&out_loop, "%s    double %s = mochi_val_num((%s)->items[%s]);\n", ind, pname, o.code, li);
        Buf bodyc; b_init(&bodyc);
        if (body_node) {
            Buf saved = em->out; em->out = bodyc;
            EStr be = emit_expr(em, body_node);
            em->out = saved;
            b_put(&bodyc, be.code);
        }
        const char *bc = bodyc.len ? bodyc.s : "0";
        if (strcmp(prop, "some") == 0)
            b_putf(&out_loop, "%s    if (%s) { %s = 1; break; }\n", ind, bc, res);
        else if (strcmp(prop, "every") == 0)
            b_putf(&out_loop, "%s    if (!(%s)) { %s = 0; break; }\n", ind, bc, res);
        else
            b_putf(&out_loop, "%s    if (%s) { %s = %s; break; }\n", ind, bc, res, pname);
        b_putf(&out_loop, "%s}\n", ind);
        free(bodyc.s);
        result_name = cmc_arena_strndup(em->ctx->arena, res, strlen(res));
        if (strcmp(prop, "some") == 0 || strcmp(prop, "every") == 0) result_is_bool = 1;
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
        if (result_is_bool) return estr(result_name, CT_BOOL, 0, 0, NULL);
        return estr(result_name, CT_DOUBLE, 0, 0, NULL);
    }
    return estr("0", CT_INT, 0, 0, NULL);
}

/* pack args[ri..] into a fresh mochi_array (emitted as pre-lines); returns the name */

const char *build_rest_array(Emit *em, EStr *args, int ri, int nargs) {
    const char *name = tmp_name(em, "rest");
    Buf l; b_init(&l);
    b_putf(&l, "mochi_array *%s = mochi_array_new();", name);
    e_preline(em, l.s);
    free(l.s);
    for (int i = ri; i < nargs; i++) {
        Buf pl; b_init(&pl);
        if (args[i].type == CT_STR)
            b_putf(&pl, "mochi_array_push(%s, mochi_str(%s));", name, args[i].code);
        else
            b_putf(&pl, "mochi_array_push(%s, mochi_num(%s));", name, args[i].code);
        e_preline(em, pl.s);
        free(pl.s);
    }
    return name;
}

/* ---- call spread: dynamic-arity argument materialization ----
 * A call with a `...expr` arg has a runtime-dependent arity, which cannot be
 * expressed as a fixed C argument list. We materialize the entire argument
 * sequence into a fresh mochi_array* (spread args expanded element-by-element),
 * then read each callee parameter position out of it with a bounds guard.
 * This is the dynamic-typing path: static calls (no spread) keep the fast
 * fixed-arity lowering above. */
int has_call_spread(CmcNode *n) {
    for (int i = 0; i < n->nargs; i++)
        if (n->args[i] && n->args[i]->is_spread) return 1;
    return 0;
}
const char *build_call_arg_array(Emit *em, CmcNode *n, EStr *args) {
    const char *name = tmp_name(em, "cargs");
    Buf l; b_init(&l);
    b_putf(&l, "mochi_array *%s = mochi_array_new();", name);
    e_preline(em, l.s);
    free(l.s);
    for (int i = 0; i < n->nargs; i++) {
        Buf pl; b_init(&pl);
        if (n->args[i]->is_spread)
            /* expand the runtime array's elements (mochi_val boxed) */
            b_putf(&pl, "for (int __s%d = 0; __s%d < (%s)->len; __s%d++) mochi_array_push(%s, (%s)->items[__s%d]);",
                   i, i, args[i].code, i, name, args[i].code, i);
        else if (args[i].type == CT_STR)
            b_putf(&pl, "mochi_array_push(%s, mochi_str(%s));", name, args[i].code);
        else
            b_putf(&pl, "mochi_array_push(%s, mochi_num(%s));", name, args[i].code);
        e_preline(em, pl.s);
        free(pl.s);
    }
    return name;
}
/* bounds-guarded read of parameter position `pos` from the materialized args */
void emit_cargs_arg(Buf *b, const char *cargs, int pos, CmcType pt) {
    if (pt == CT_STR)
        b_putf(b, "((%s)->len > %d ? mochi_val_str(mochi_array_get(%s, %d)) : (const char *)0)",
               cargs, pos, cargs, pos);
    else if (pt == CT_BOOL)
        b_putf(b, "((%s)->len > %d ? mochi_val_bool(mochi_array_get(%s, %d)) : 0)",
               cargs, pos, cargs, pos);
    else
        b_putf(b, "((%s)->len > %d ? mochi_val_num(mochi_array_get(%s, %d)) : 0)",
               cargs, pos, cargs, pos);
}
void emit_cargs_has(Buf *b, const char *cargs, int pos) {
    b_putf(b, "((%s)->len > %d)", cargs, pos);
}

/* ---------------------- classes ---------------------- */

/* env-free literal type inference for class fields / returns */
/* collect field names used in a string context (`"..." + this.f` etc.).
   These fields must be const char * even if their init RHS is a bare param. */

/* Object.assign(tgt, src): a per-shape-pair helper copies src's fields into
 * tgt (shared fields directly, extra fields via the overflow map), so the
 * assignment follows JS semantics for both static and dynamic keys. */
static void emit_assign_helper(Emit *em, const char *ss, const char *ts) {
    char pair[192]; snprintf(pair, sizeof pair, "%s|%s", ss, ts);
    for (int i = 0; i < em->asg_n; i++)
        if (strcmp(em->asg_pairs[i], pair) == 0) return;
    if (em->asg_n < 64) em->asg_pairs[em->asg_n++] = cmc_arena_strndup(em->ctx->arena, pair, strlen(pair));
    int tgt_dk = cmc_dk_is(ts);
    if (tgt_dk) ensure_dk_helpers(em, ts);
    Buf g; b_init(&g);
    b_putf(&g, "static void %s_into_%s(struct %s *t, const struct %s *s) {\n", ss, ts, ts, ss);
    for (int f = 0; f < 64; f++) {
        const char *fn = cmc_struct_field_name_n(ss, f);
        if (!fn) break;
        const char *tft = cmc_struct_field_type(ts, fn);
        CmcType sk = cmc_struct_field_kind(ss, fn);
        Buf rhs; b_init(&rhs); b_putf(&rhs, "s->%s", fn);
        if (tft) {
            CmcType tk = cmc_struct_field_kind(ts, fn);
            if (tk == CT_MOCHI_VAL) { char *bx = dk_box(sk, rhs.s); b_putf(&g, "    t->%s = %s;\n", fn, bx); free(bx); }
            else if (sk == CT_MOCHI_VAL) { b_putf(&g, "    t->%s = %s;\n", fn, dk_unbox(tk)); }
            else if (tk == CT_STR && sk == CT_STR) b_putf(&g, "    t->%s = s->%s;\n", fn, fn);
            else if (cmc_is_numeric(tk) && cmc_is_numeric(sk)) b_putf(&g, "    t->%s = (%s)s->%s;\n", fn, cmc_type_name(tk), fn);
            else if (tk == CT_BOOL) b_putf(&g, "    t->%s = s->%s ? 1 : 0;\n", fn, fn);
            else b_putf(&g, "    t->%s = s->%s;\n", fn, fn);
        } else if (tgt_dk) {
            char *bx = dk_box(sk, rhs.s);
            b_putf(&g, "    %s_set(t, \"%s\", %s);\n", ts, fn, bx);
            free(bx);
        }
        free(rhs.s);
    }
    if (cmc_dk_is(ss) && tgt_dk)
        b_putf(&g, "    if (s->extra) for (MochiMapEntry *e = s->extra->head; e; e = e->next)\n        %s_set(t, e->key, e->val);\n", ts);
    b_put(&g, "}\n\n");
    b_put(&em->fn_extra, g.s);
    free(g.s);
}

/* call a boxed function value: build a mochi_val argv[] pre-line and
 * dispatch through mochi_call (runtime tag t=5). */
void emit_mochi_call(Emit *em, Buf *b, const char *fnv, CmcNode *n, EStr *args) {
    const char *av = tmp_name(em, "argv");
    Buf mk; b_init(&mk);
    b_putf(&mk, "mochi_val %s[%d];", av, n->nargs ? n->nargs : 1);
    e_preline(em, mk.s); free(mk.s);
    for (int i = 0; i < n->nargs; i++) {
        Buf v; b_init(&v); co_box(&v, args[i]);
        Buf st; b_init(&st);
        b_putf(&st, "%s[%d] = %s;", av, i, v.s);
        e_preline(em, st.s); free(st.s); free(v.s);
    }
    b_putf(b, "mochi_call(%s, %d, %s)", fnv, n->nargs, av);
}

/* call through a stored function value (arr[i](...), obj.fn(...) where the
 * value is a boxed function): dispatch via mochi_call. */
EStr emit_stored_fn_call(Emit *em, CmcNode *n, EStr o) {
    EStr *args = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
    for (int i = 0; i < n->nargs; i++) args[i] = emit_expr(em, n->args[i]);
    Buf b; b_init(&b);
    emit_mochi_call(em, &b, o.code, n, args);
    free(args);
    return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
}

EStr emit_call(Emit *em, CmcNode *n) {
    CmcNode *callee = n->obj;
    if (n->is_new) return emit_new(em, n); /* `new Ctor(...)` */
    /* the callee is itself a call returning a boxed function: (f())() */
    if (callee->kind == N_CALL) {
        EStr fnv = emit_call(em, callee);
        return emit_stored_fn_call(em, n, fnv);
    }
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
        const char *rsh = NULL;   /* shape of the returned object literal */
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
                /* dynamic-arity call: materialize all args into a mochi_array */
                int spread = has_call_spread(n);
                const char *cargs = spread ? build_call_arg_array(em, n, args) : NULL;
                if (fn && fn->kind == N_ARROW && fn->nparams > 0) {
                    /* nullish-guarded arrow: pass has_X, value for each param */
                    int np = fn->nparams;
                    for (int i = 0; i < np; i++) {
                        if (i) b_put(&b, ", ");
                        if (fn->params[i]->is_rest) {
                            if (spread)
                                b_putf(&b, "mochi_array_slice(%s, %d, (%s)->len)", cargs, i, cargs);
                            else
                                b_putf(&b, "%s", build_rest_array(em, args, i, n->nargs));
                            continue;
                        }
                        CmcType pt = CT_DOUBLE;
                        CmcNode *pn = fn->params[i];
                        if (pn && pn->kind == N_IDENT) pt = infer_param_type_lite(em, fn, pn->name);
                        else if (pn && pn->kind == N_ASSIGN && pn->lhs) pt = cmc_type_of(em->ctx, &em->env, pn->rhs);
                        if (spread) {
                            emit_cargs_has(&b, cargs, i);
                            b_put(&b, ", ");
                            emit_cargs_arg(&b, cargs, i, pt);
                        } else if (i < n->nargs) {
                            Buf h; b_init(&h);
                            arg_has(em, n->args[i], &h);
                            b_putf(&b, "%s, ", h.s);
                            free(h.s);
                            if (pt == CT_MOCHI_VAL) {
                                if (args[i].type == CT_MOCHI_VAL) b_put(&b, args[i].code);
                                else { Buf bx; b_init(&bx); co_box(&bx, args[i]); b_put(&b, bx.s); free(bx.s); }
                            }
                            else if (args[i].type == CT_STR) co_str(em, &b, args[i]);
                            else co_num(em, &b, args[i]);
                        } else {
                            /* omitted arg: not present -> has=0, value 0/NULL */
                            b_put(&b, pt == CT_STR ? "0, NULL" : "0, 0");
                        }
                    }
                } else {
                    int np = fn && fn->nparams > 0 ? fn->nparams : n->nargs;
                    for (int i = 0; i < np; i++) {
                        if (i) b_put(&b, ", ");
                        if (fn && fn->params[i]->is_rest) {
                            if (spread)
                                b_putf(&b, "mochi_array_slice(%s, %d, (%s)->len)", cargs, i, cargs);
                            else
                                b_putf(&b, "%s", build_rest_array(em, args, i, n->nargs));
                        } else if (spread) {
                            /* arity is runtime: read position i, bounds-guarded */
                            CmcType pt = CT_DOUBLE;
                            if (fn && i < fn->nparams && fn->params[i] && fn->params[i]->kind == N_IDENT)
                                pt = infer_param_type_lite(em, fn, fn->params[i]->name);
                            emit_cargs_arg(&b, cargs, i, pt);
                        } else if (i < n->nargs) {
                            const char *psh2 = (fn && i < fn->nparams && fn->params[i] && fn->params[i]->kind == N_IDENT)
                                ? cmc_param_shape(fn->fname, fn->params[i]->name) : NULL;
                            CmcType pt2 = CT_DOUBLE;
                            if (fn && i < fn->nparams && fn->params[i] && fn->params[i]->kind == N_IDENT)
                                pt2 = infer_param_type_lite(em, fn, fn->params[i]->name);
                            if (pt2 == CT_MOCHI_VAL) {
                                if (args[i].type == CT_MOCHI_VAL) b_put(&b, args[i].code);
                                else { Buf bx; b_init(&bx); co_box(&bx, args[i]); b_put(&b, bx.s); free(bx.s); }
                            }
                            else if (psh2) b_putf(&b, "%s", args[i].code);
                            else if (args[i].type == CT_STR) co_str(em, &b, args[i]);
                            else co_num(em, &b, args[i]);
                        } else b_put(&b, "0");
                    }
                }
                /* record call-site types so later passes type params from use */
                if (fn) for (int i = 0; i < n->nargs && i < fn->nparams; i++) {
                    if (!fn->params[i] || fn->params[i]->kind != N_IDENT) continue;
                    if (args[i].type == CT_OBJ && args[i].struct_name)
                        cmc_record_param_shape(fn->fname, fn->params[i]->name, args[i].struct_name);
                    else
                        cmc_record_param_type(fn->fname, fn->params[i]->name, args[i].type);
                }
                b_put(&b, ")");
                t = s->type;
                rsh = fn_return_shape(em, fn);
            } else {
                /* a variable/param holding a boxed function value */
                if (s && s->type == CT_MOCHI_VAL) {
                    Buf cv; b_init(&cv); b_putf(&cv, "%s", s->cname);
                    emit_mochi_call(em, &b, cv.s, n, args);
                    free(cv.s);
                    free(args);
                    return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
                }
                b_putf(&b, "%s(", nm);
                for (int i = 0; i < n->nargs; i++) { if (i) b_put(&b, ", "); co_num(em, &b, args[i]); }
                b_put(&b, ")");
                t = CT_UNKNOWN;
            }
        }
        free(args);
        return estr(b.s, t, 0, t == CT_STR ? 1 : 0, rsh);
    }
    if (callee->kind == N_MEMBER) {
        CmcNode *m = callee;
        const char *prop = (m->prop && m->prop->name) ? m->prop->name : "";
        if (m->obj && m->obj->kind == N_IDENT && m->obj->name &&
                strcmp(m->obj->name, "Date") == 0 && strcmp(prop, "now") == 0 &&
                n->nargs == 0) {
            return estr("mochi_now_ms()", CT_DOUBLE, 0, 0, NULL);
        }
        EStr o = emit_expr(em, m->obj);
        /* a computed member call (arr[i](...)) is a call through the element
         * value, not a method call — handled as a stored-function call below */
        if (m->computed || !m->prop || !m->prop->name) {
            EStr elem = emit_member(em, m);
            return emit_stored_fn_call(em, n, elem);
        }
        if (m->obj && m->obj->kind == N_IDENT && m->obj->name &&
                strcmp(m->obj->name, "JSON") == 0 && strcmp(prop, "stringify") == 0 &&
                n->nargs == 1) {
                EStr a = emit_expr(em, n->args[0]);
                if (a.type == CT_OBJ && a.struct_name) {
                    Buf jb; b_init(&jb);
                    b_putf(&jb, "%s_json(%s)", a.struct_name, a.code);
                    return estr(jb.s, CT_STR, 1, 1, NULL);
                }
                fprintf(stderr, "cmochi: warning: JSON.stringify needs an object\n");
                return estr("\"\"", CT_STR, 1, 1, NULL);
            }
        /* Object.keys(obj) / Object.values(obj): the shape's runtime key list */
        if (m->obj && m->obj->kind == N_IDENT && m->obj->name &&
            strcmp(m->obj->name, "Object") == 0 && n->nargs >= 1) {
            if (strcmp(prop, "assign") == 0 && n->nargs == 2) {
                EStr tgt = emit_expr(em, n->args[0]);
                EStr src = emit_expr(em, n->args[1]);
                if (tgt.type == CT_OBJ && src.type == CT_OBJ && tgt.struct_name && src.struct_name) {
                    emit_assign_helper(em, src.struct_name, tgt.struct_name);
                    Buf ab; b_init(&ab);
                    b_putf(&ab, "%s_into_%s(%s, %s)", src.struct_name, tgt.struct_name, tgt.code, src.code);
                    return estr(ab.s, CT_ARR, 0, 1, NULL);
                }
                fprintf(stderr, "cmochi: warning: Object.assign needs two shaped objects\n");
                return estr("0", CT_UNKNOWN, 0, 0, NULL);
            }
            if (strcmp(prop, "keys") == 0 || strcmp(prop, "values") == 0 ||
                strcmp(prop, "entries") == 0) {
                EStr a = emit_expr(em, n->args[0]);
                if (a.struct_name) {
                    ensure_keys_helpers(em, a.struct_name);
                    Buf kb; b_init(&kb);
                    b_putf(&kb, "%s_%s(%s)", a.struct_name, prop, a.code);
                    return estr(kb.s, CT_ARR, 0, 1, NULL);
                }
                if (a.type == CT_ARR) {
                    Buf kb; b_init(&kb);
                    b_putf(&kb, "mochi_array_entries(%s)", a.code);
                    return estr(kb.s, CT_ARR, 0, 1, NULL);
                }
            }
        }
        /* object shorthand method call: inline the lifted static fn */
        if (o.struct_name) {
            for (int mi = 0; mi < em->m_n; mi++) {
                if (strcmp(em->m_shape[mi], o.struct_name) == 0 && strcmp(em->m_field[mi], prop) == 0) {
                    Buf mb; b_init(&mb);
                    if (!em->m_guarded[mi]) {
                        /* class method: `this` (the object) is the first argument */
                        b_putf(&mb, "%s(%s", em->m_fn[mi], o.code);
                        int mspread = has_call_spread(n);
                        const char *mcargs = NULL;
                        if (mspread) {
                            EStr *margs = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
                            for (int i = 0; i < n->nargs; i++) margs[i] = emit_expr(em, n->args[i]);
                            mcargs = build_call_arg_array(em, n, margs);
                            free(margs);
                        }
                        int np_ = em->m_nparams[mi];
                        for (int i = 0; i < np_; i++) {
                            b_put(&mb, ", ");
                            if (mspread) {
                                CmcType pt = CT_DOUBLE;
                                CmcNode *mn = em->m_node[mi];
                                if (mn && i < mn->nparams && mn->params[i])
                                    pt = infer_param_type_lite(em, mn, mn->params[i]->name);
                                emit_cargs_arg(&mb, mcargs, i, pt);
                            } else if (i < n->nargs) {
                                EStr ae = emit_expr(em, n->args[i]);
                                if (ae.type == CT_STR) co_str(em, &mb, ae);
                                else co_num(em, &mb, ae);
                            } else b_put(&mb, "0");
                        }
                        b_put(&mb, ")");
                        EStr r = estr(mb.s, em->m_ret[mi], 0, 0,
                                      em->m_ret[mi] == CT_OBJ ? em->m_shape[mi] : NULL);
                        return r;
                    }
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
        if (m->obj->kind == N_IDENT && strcmp(m->obj->name, "Array") == 0) {
            if (strcmp(prop, "isArray") == 0) {
                EStr a = emit_expr(em, n->args[0]);
                Buf b; b_init(&b);
                if (a.type == CT_ARR) b_put(&b, "1");
                else if (a.type == CT_MOCHI_VAL) b_putf(&b, "((%s).t == 4)", a.code);
                else b_put(&b, "0");
                return estr(b.s, CT_BOOL, 0, 0, NULL);
            }
            if (strcmp(prop, "from") == 0) {
                EStr a = emit_expr(em, n->args[0]);
                if (a.type == CT_ARR) {
                    Buf b; b_init(&b);
                    b_putf(&b, "mochi_array_slice(%s, 0, (%s)->len)", a.code, a.code);
                    return estr(b.s, CT_ARR, 0, 1, NULL);
                }
                fprintf(stderr, "cmochi: warning: Array.from over this value is unsupported\n");
                return estr("0", CT_UNKNOWN, 0, 0, NULL);
            }
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
        /* process.stdout.write(x): print without trailing newline */
        if (m->obj->kind == N_MEMBER && m->obj->obj &&
            m->obj->obj->kind == N_IDENT && strcmp(m->obj->obj->name, "process") == 0 &&
            m->obj->prop && m->obj->prop->kind == N_IDENT && strcmp(m->obj->prop->name, "stdout") == 0 &&
            strcmp(prop, "write") == 0) {
            EStr *args = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
            for (int i = 0; i < n->nargs; i++) args[i] = emit_expr(em, n->args[i]);
            Buf b; b_init(&b);
            b_put(&b, "printf(\"%s\"");
            for (int i = 0; i < n->nargs; i++) {
                if (args[i].type == CT_STR) { b_put(&b, ", "); b_put(&b, args[i].code); }
                else if (args[i].type == CT_DOUBLE || args[i].type == CT_INT) { b_put(&b, ", \"%g\""); b_put(&b, ", "); b_put(&b, args[i].code); }
                else b_putf(&b, ", mochi_val_str_js(%s)", args[i].code);
            }
            b_put(&b, ")");
            return estr(b.s, CT_INT, 0, 0, NULL);
        }
        if (o.type == CT_MOCHI_VAL) {
            /* A dynamic value (array element, map value) can be a string or an
             * array at runtime — pick the family by method name. */
            static const char *arr_methods[] = {"push","pop","shift","unshift","splice",
                "sort","fill","flat","reverse","map","filter","reduce","forEach",
                "some","every","find","findIndex","join","toString",NULL};
            static const char *str_methods[] = {"trim","toUpperCase","toLowerCase",
                "charAt","charCodeAt","substring","substr","replace","replaceAll",
                "split","padStart","padEnd","repeat","startsWith","endsWith",NULL};
            static const char *ambig[] = {"slice","indexOf","lastIndexOf","includes",
                "concat","at",NULL};
            const char *o_code = o.code;
            int is_arr = 0, is_str = 0, is_amb = 0;
            for (int i = 0; arr_methods[i]; i++) if (!strcmp(prop, arr_methods[i])) is_arr = 1;
            for (int i = 0; str_methods[i]; i++) if (!strcmp(prop, str_methods[i])) is_str = 1;
            for (int i = 0; ambig[i]; i++) if (!strcmp(prop, ambig[i])) is_amb = 1;
            if (is_arr || (is_amb && 0)) {
                char *ub = (char *)malloc(strlen(o_code) + 32);
                sprintf(ub, "mochi_arr_or_empty(%s)", o_code);
                o.code = ub; o.type = CT_ARR;
            } else if (is_str) {
                char *ub = (char *)malloc(strlen(o_code) + 32);
                sprintf(ub, "mochi_str_or_empty(%s)", o_code);
                o.code = ub; o.type = CT_STR;
            } else if (is_amb) {
                /* runtime dispatch: the receiver decides string vs array */
                Buf b; b_init(&b);
                EStr *a = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
                for (int i = 0; i < n->nargs; i++) a[i] = emit_expr(em, n->args[i]);
                if (!strcmp(prop, "slice")) {
                    Buf s0, e0; b_init(&s0); b_init(&e0);
                    co_num(em, &s0, n->nargs > 0 ? a[0] : estr("0", CT_INT, 0, 0, NULL));
                    b_putf(&e0, "mochi_val_length(%s)", o_code);
                    b_putf(&b, "mochi_dyn_slice(%s, %s, %s)", o_code, s0.s, e0.s);
                    free(s0.s); free(e0.s);
                } else if (!strcmp(prop, "indexOf")) {
                    Buf s0; b_init(&s0); co_str(em, &s0, a[0]);
                    b_putf(&b, "mochi_dyn_index_of(%s, %s)", o_code, s0.s); free(s0.s);
                    free(a);
                    return estr(b.s, CT_INT, 0, 0, NULL);
                } else if (!strcmp(prop, "lastIndexOf")) {
                    Buf s0; b_init(&s0); co_str(em, &s0, a[0]);
                    b_putf(&b, "mochi_dyn_last_index_of(%s, %s)", o_code, s0.s); free(s0.s);
                    free(a);
                    return estr(b.s, CT_INT, 0, 0, NULL);
                } else if (!strcmp(prop, "includes")) {
                    Buf s0; b_init(&s0); co_str(em, &s0, a[0]);
                    b_putf(&b, "mochi_dyn_includes(%s, %s)", o_code, s0.s); free(s0.s);
                    free(a);
                    return estr(b.s, CT_BOOL, 0, 0, NULL);
                } else if (!strcmp(prop, "at")) {
                    Buf i0; b_init(&i0); co_num(em, &i0, a[0]);
                    b_putf(&b, "mochi_dyn_at(%s, %s)", o_code, i0.s); free(i0.s);
                } else if (!strcmp(prop, "concat")) {
                    Buf oth; b_init(&oth); co_box(&oth, a[0]);
                    b_putf(&b, "mochi_dyn_concat(%s, %s)", o_code, oth.s); free(oth.s);
                }
                free(a);
                return estr(b.s, CT_MOCHI_VAL, 0, 0, NULL);
            }
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
            } else if (strcmp(prop, "indexOf") == 0) {
                Buf v; b_init(&v); co_num(em, &v, args[0]);
                b_putf(&b, "mochi_array_index_of(%s, %s)", o.code, v.s);
                free(v.s);
            } else if (strcmp(prop, "includes") == 0) {
                Buf v; b_init(&v); co_num(em, &v, args[0]);
                b_putf(&b, "mochi_array_includes(%s, %s)", o.code, v.s);
                free(v.s);
                t = CT_BOOL;
            } else if (strcmp(prop, "slice") == 0) {
                Buf stv; b_init(&stv);
                if (n->nargs > 0) co_int(&stv, args[0]); else b_put(&stv, "0");
                Buf endv; b_init(&endv);
                if (n->nargs > 1) b_putf(&endv, "%s", args[1].code);
                else b_putf(&endv, "(%s)->len", o.code);
                b_putf(&b, "mochi_array_slice(%s, %s, %s)", o.code, stv.s, endv.s);
                free(stv.s); free(endv.s);
                t = CT_ARR;
            } else if (strcmp(prop, "splice") == 0) {
                Buf stv; b_init(&stv);
                if (n->nargs > 0) co_int(&stv, args[0]); else b_put(&stv, "0");
                Buf delv; b_init(&delv);
                if (n->nargs > 1) co_int(&delv, args[1]);
                else b_putf(&delv, "(%s)->len - (%s)", o.code, stv.s);
                if (n->nargs > 2) {
                    const char *ins = tmp_name(em, "ins");
                    Buf mk; b_init(&mk);
                    b_putf(&mk, "mochi_array *%s = mochi_array_new();", ins);
                    e_preline(em, mk.s); free(mk.s);
                    for (int i = 2; i < n->nargs; i++) {
                        Buf v; b_init(&v); co_box(&v, args[i]);
                        Buf push; b_init(&push);
                        b_putf(&push, "mochi_array_push(%s, %s);", ins, v.s);
                        e_preline(em, push.s); free(push.s); free(v.s);
                    }
                    b_putf(&b, "mochi_array_splice(%s, %s, %s, %s)", o.code, stv.s, delv.s, ins);
                } else {
                    b_putf(&b, "mochi_array_splice(%s, %s, %s, NULL)", o.code, stv.s, delv.s);
                }
                free(stv.s); free(delv.s);
                t = CT_ARR;
            } else if (strcmp(prop, "shift") == 0) {
                b_putf(&b, "mochi_array_shift(%s)", o.code);
                t = CT_MOCHI_VAL;
            } else if (strcmp(prop, "unshift") == 0 && n->nargs == 1) {
                Buf v; b_init(&v); co_box(&v, args[0]);
                b_putf(&b, "mochi_array_unshift(%s, %s)", o.code, v.s);
                free(v.s);
                t = CT_ARR;
            } else if (strcmp(prop, "concat") == 0) {
                b_putf(&b, "mochi_array_concat(%s, %s)", o.code, args[0].code);
                t = CT_ARR;
            } else if (strcmp(prop, "sort") == 0) {
                b_putf(&b, "mochi_array_sort(%s)", o.code);
                t = CT_ARR;
            } else if (strcmp(prop, "fill") == 0) {
                Buf v; b_init(&v); co_box(&v, args[0]);
                Buf st; b_init(&st);
                if (n->nargs > 1) { co_int(&st, args[1]); } else b_put(&st, "0");
                Buf en; b_init(&en);
                if (n->nargs > 2) { co_int(&en, args[2]); } else b_putf(&en, "(%s)->len", o.code);
                b_putf(&b, "mochi_array_fill(%s, %s, %s, %s)", o.code, v.s, st.s, en.s);
                free(v.s); free(st.s); free(en.s);
                t = CT_ARR;
            } else if (strcmp(prop, "at") == 0) {
                Buf i; b_init(&i); co_int(&i, args[0]);
                b_putf(&b, "mochi_array_at(%s, %s)", o.code, i.s);
                free(i.s);
                t = CT_MOCHI_VAL;
            } else if (strcmp(prop, "flat") == 0) {
                b_putf(&b, "mochi_array_flat(%s)", o.code);
                t = CT_ARR;
            } else if (strcmp(prop, "lastIndexOf") == 0) {
                Buf v; b_init(&v); co_num(em, &v, args[0]);
                b_putf(&b, "mochi_array_last_index_of(%s, %s)", o.code, v.s);
                free(v.s);
                t = CT_INT;
            } else if (strcmp(prop, "reverse") == 0) {
                b_putf(&b, "mochi_array_reverse(%s)", o.code);
                t = CT_ARR;
            } else if (strcmp(prop, "map") == 0 || strcmp(prop, "filter") == 0 ||
                       strcmp(prop, "reduce") == 0 || strcmp(prop, "forEach") == 0 ||
                       strcmp(prop, "some") == 0 || strcmp(prop, "every") == 0 ||
                       strcmp(prop, "find") == 0) {
                EStr r = emit_arr_higher(em, n, prop, o);
                free(args);
                return r;
            } else b_putf(&b, "0");
            free(args);
            return estr(b.s, t, 0, t == CT_STR ? 1 : 0, NULL);
        }
        if (o.type == CT_MOCHI_VAL) {
            /* a string boxed in a dynamic value (array element, map value) */
            char *ub = (char *)malloc(strlen(o.code) + 32);
            sprintf(ub, "mochi_str_or_empty(%s)", o.code);
            o.code = ub; o.type = CT_STR;
        }
        if (o.type == CT_STR) {
            EStr *args = (EStr *)malloc(sizeof(EStr) * (n->nargs ? n->nargs : 1));
            for (int i = 0; i < n->nargs; i++) args[i] = emit_expr(em, n->args[i]);
            Buf b; b_init(&b);
            CmcType t = CT_STR;
            if (strcmp(prop, "toUpperCase") == 0) b_putf(&b, "mochi_toupper(%s)", o.code);
            else if (strcmp(prop, "toLowerCase") == 0) b_putf(&b, "mochi_tolower(%s)", o.code);
            else if (strcmp(prop, "charAt") == 0) b_putf(&b, "mochi_char_at(%s, %s)", o.code, args[0].code);
            else if (strcmp(prop, "substring") == 0)
                b_putf(&b, "mochi_substring(%s, %s, %s)", o.code, args[0].code,
                        n->nargs > 1 ? args[1].code : "(int)strlen(%s)");
            else if (strcmp(prop, "substr") == 0)
                b_putf(&b, "mochi_substr(%s, %s, %s)", o.code, args[0].code, n->nargs > 1 ? args[1].code : "-1");
            else if (strcmp(prop, "replaceAll") == 0)
                b_putf(&b, "mochi_replace_all(%s, %s, %s)", o.code, args[0].code, args[1].code);
            else if (strcmp(prop, "lastIndexOf") == 0) {
                b_putf(&b, "mochi_str_last_index_of(%s, %s)", o.code, args[0].code);
                t = CT_INT;
            } else if (strcmp(prop, "charCodeAt") == 0) {
                b_putf(&b, "mochi_char_code_at(%s, %s)", o.code, args[0].code);
                t = CT_INT;
            } else if (strcmp(prop, "at") == 0) {
                b_putf(&b, "mochi_str_at(%s, %s)", o.code, args[0].code);
            } else if (strcmp(prop, "concat") == 0) {
                /* variadic: fold left with concat2 */
                Buf acc; b_init(&acc); b_put(&acc, o.code);
                for (int i = 0; i < n->nargs; i++) {
                    Buf av; b_init(&av); co_str(em, &av, args[i]);
                    Buf nx; b_init(&nx);
                    b_putf(&nx, "mochi_str_concat2(%s, %s)", acc.s, av.s);
                    free(acc.s); acc = nx; free(av.s);
                }
                b_putf(&b, "%s", acc.s); free(acc.s);
            }
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
            else if (strcmp(prop, "split") == 0) {
                if (n->nargs > 1) {
                    Buf lim; b_init(&lim); co_num(em, &lim, args[1]);
                    b_putf(&b, "mochi_split_limit(%s, %s, (int)%s)", o.code, args[0].code, lim.s);
                    free(lim.s);
                } else b_putf(&b, "mochi_split(%s, %s)", o.code, args[0].code);
                t = CT_ARR;
            }
            else if (strcmp(prop, "padStart") == 0) {
            Buf ln, fl; b_init(&ln); b_init(&fl);
            co_num(em, &ln, args[0]);
            co_str(em, &fl, n->nargs > 1 ? args[1] : estr("0", CT_STR, 0, 1, NULL));
            b_putf(&b, "mochi_pad_start(%s, (int)%s, %s)", o.code, ln.s, fl.s);
            free(ln.s); free(fl.s);
            } else if (strcmp(prop, "padEnd") == 0) {
            Buf ln, fl; b_init(&ln); b_init(&fl);
            co_num(em, &ln, args[0]);
            co_str(em, &fl, n->nargs > 1 ? args[1] : estr("0", CT_STR, 0, 1, NULL));
            b_putf(&b, "mochi_pad_end(%s, (int)%s, %s)", o.code, ln.s, fl.s);
            free(ln.s); free(fl.s);
            } else b_putf(&b, "%s", o.code);
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

EStr emit_expr(Emit *em, CmcNode *n) {
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
        case N_THIS: {
        /* inside a lifted class method: the enclosing struct pointer */
        CmcSym *s = cmc_env_lookup(&em->env, "this");
        if (em->this_name && s) return estr(em->this_name, CT_OBJ, 0, 1, s->struct_name);
        if (em->this_name) return estr(em->this_name, CT_OBJ, 0, 1, NULL);
        return estr("0", CT_UNKNOWN, 0, 0, NULL);
    }
        default: return estr("0", CT_UNKNOWN, 0, 0, NULL);
    }
}

/* ---------------------- deferred-type pre-pass ----------------------
   `var x;` (declarator with no init) gets its C type from the FIRST
   assignment `x = <expr>` in the program. JS hoists `var` to function
   scope, so we record across the whole main body, then resolve from the
   first assignment we meet (string -> const char*, number -> double). */

/* env-free: is this expression a string (literal, template, or concat)? */
static int is_str_expr(CmcNode *a) {
    if (!a) return 0;
    if (a->kind == N_STR || a->kind == N_TEMPLATE) return 1;
    if (a->kind == N_BIN && a->op == TK_PLUS)
        return is_str_expr(a->lhs) || is_str_expr(a->rhs);
    return 0;
}
/* The shape of the object literal a function returns. Lets object spread and
 * polymorphic dispatch know the source's real fields instead of guessing
 * (a call used to lose the shape, so `{...mk()}` could not copy anything). */
const char *fn_return_shape(Emit *em, CmcNode *fn) {
    if (!fn || !fn->body) return NULL;
    CmcNode *stack[512]; int sp = 0;
    stack[sp++] = fn->body;
    while (sp) {
        CmcNode *cur = stack[--sp];
        if (!cur) continue;
        if (cur->kind == N_RETURN && cur->lhs && cur->lhs->kind == N_OBJ)
            return cmc_register_shape(em->ctx->arena, cur->lhs);
        if (cur->kind == N_BLOCK) for (int i = 0; i < cur->nstmts; i++) stack[sp++] = cur->stmts[i];
        else if (cur->kind == N_IF) { stack[sp++] = cur->test; stack[sp++] = cur->cons; stack[sp++] = cur->alt; }
        else if (cur->kind == N_WHILE || cur->kind == N_DOWHILE || cur->kind == N_FOROF) { stack[sp++] = cur->test; stack[sp++] = cur->stmt; }
        else if (cur->kind == N_FOR) { stack[sp++] = cur->init; stack[sp++] = cur->test; stack[sp++] = cur->update; stack[sp++] = cur->stmt; }
        else if (cur->kind == N_VAR) for (int i = 0; i < cur->nstmts; i++) stack[sp++] = cur->stmts[i];
        else if (cur->kind == N_EXPRSTMT) stack[sp++] = cur->lhs;
        else if (cur->kind == N_TRY) { stack[sp++] = cur->stmt; stack[sp++] = cur->alt; }
        else if (cur->kind == N_THROW) stack[sp++] = cur->lhs;
        else if (cur->kind == N_FUNC || cur->kind == N_CLASS) { /* nested scope */ }
        else if (cur->lhs) stack[sp++] = cur->lhs;
    }
    return NULL;
}
CmcType fn_return_type(CmcNode *fn) {
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
            else if (a->kind == N_ARROW || a->kind == N_FUNC) t = CT_MOCHI_VAL;
            else if (a->kind == N_CALL && a->obj && a->obj->kind == N_IDENT) {
                /* a call through a boxed callable (var/param callee) */
                const char *cn = a->obj->name;
                int isparam = 0;
                for (int i = 0; i < fn->nparams; i++)
                    if (fn->params[i]->kind == N_IDENT && fn->params[i]->name && !strcmp(fn->params[i]->name, cn)) { isparam = 1; break; }
                if (isparam) t = CT_MOCHI_VAL;
            }
            else if (is_str_expr(a)) t = CT_STR; /* concat / template */
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
        else if (cur->kind == N_TRY) { stack[sp++] = cur->stmt; stack[sp++] = cur->alt; }
        else if (cur->kind == N_THROW) stack[sp++] = cur->lhs;
        else if (cur->kind == N_FUNC || cur->kind == N_CLASS) { /* nested: separate scope */ }
        else if (cur->lhs) stack[sp++] = cur->lhs;
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
        if (b->kind == N_ARROW || b->kind == N_FUNC) return CT_MOCHI_VAL;
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

CmcType infer_param_type_lite(Emit *em, CmcNode *fn, const char *pname) {
    int has_double = 0, has_str = 0;
    CmcNode *stack[512]; int sp = 0;
    stack[sp++] = fn->body ? fn->body : fn;
    while (sp) {
        CmcNode *cur = stack[--sp];
        if (!cur) continue;
        if (cur->kind == N_BIN) {
            if (cur->op == TK_SLASH) has_double = 1;
            if (cur->lhs->kind == N_IDENT && strcmp(cur->lhs->name, pname) == 0) {
                if (is_str_expr(cur->rhs)) has_str = 1;
                else if (cur->rhs->kind == N_NUM && cur->rhs->num != (long long)cur->rhs->num) has_double = 1;
            }
            if (cur->rhs->kind == N_IDENT && strcmp(cur->rhs->name, pname) == 0) {
                if (is_str_expr(cur->lhs)) has_str = 1;
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
        else if (cur->kind == N_CALL) {
            /* a param used as the callee of a call is a boxed callable */
            if (cur->obj && cur->obj->kind == N_IDENT && cur->obj->name &&
                !strcmp(cur->obj->name, pname)) return CT_MOCHI_VAL;
            stack[sp++] = cur->obj; for (int i = 0; i < cur->nargs; i++) stack[sp++] = cur->args[i];
        }
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

void emit_function(Emit *em, CmcNode *fn) {
    CmcType ret = fn_return_type(fn);
    if (ret == CT_VOID) ret = CT_INT;
    const char *rshape = (ret == CT_OBJ) ? fn_return_shape(em, fn) : NULL;
    CmcType *pt = (CmcType *)malloc(sizeof(CmcType) * (fn->nparams ? fn->nparams : 1));
    for (int i = 0; i < fn->nparams; i++) {
        if (fn->params[i]->kind == N_IDENT) {
            const char *psh = cmc_param_shape(fn->fname, fn->params[i]->name);
            CmcType ct = cmc_param_type(fn->fname, fn->params[i]->name);
            pt[i] = psh ? CT_OBJ : (ct != CT_UNKNOWN ? ct : infer_param_type_lite(em, fn, fn->params[i]->name));
        } else pt[i] = CT_DOUBLE;
    }
    vol_pass(em, fn->body);
    cmc_env_push(&em->env, em->ctx->arena);
    Buf sig; b_init(&sig);
    if (rshape) b_putf(&sig, "static struct %s * %s(", rshape, fn->fname);
    else b_putf(&sig, "static %s %s(", cmc_type_name(ret), fn->fname);
    for (int i = 0; i < fn->nparams; i++) {
        if (i) b_put(&sig, ", ");
        if (fn->params[i]->is_rest && fn->params[i]->kind == N_IDENT) {
            b_putf(&sig, "mochi_array *%s", fn->params[i]->name);
            cmc_env_decl(&em->env, fn->params[i]->name, CT_ARR, SYM_PARAM);
        } else if (fn->params[i]->kind == N_IDENT) {
            const char *psh = cmc_param_shape(fn->fname, fn->params[i]->name);
            if (psh) {
                b_putf(&sig, "struct %s * %s", psh, fn->params[i]->name);
                CmcSym *ps = cmc_env_decl(&em->env, fn->params[i]->name, CT_OBJ, SYM_PARAM);
                ps->struct_name = psh;
            } else {
                b_putf(&sig, "%s %s", cmc_type_name(pt[i]), fn->params[i]->name);
                cmc_env_decl(&em->env, fn->params[i]->name, pt[i], SYM_PARAM);
            }
        } else b_put(&sig, "double p");
    }
    b_put(&sig, ") {");
    e_line(em, sig.s);
    free(sig.s);
    em->indent++;
    emit_block(em, fn->body);
    em->indent--;
    e_line(em, "}");
    /* first-class shim: uniform callable ABI for this function */
    {
        Buf sh; b_init(&sh);
        /* forward decl so the shim can call the (later-defined) function */
        if (rshape) b_putf(&sh, "static struct %s * %s(", rshape, fn->fname);
        else b_putf(&sh, "static %s %s(", cmc_type_name(ret == CT_VOID ? CT_INT : ret), fn->fname);
        for (int i = 0; i < fn->nparams; i++) {
            if (i) b_put(&sh, ", ");
            if (fn->params[i]->is_rest && fn->params[i]->kind == N_IDENT)
                b_putf(&sh, "mochi_array *%s", fn->params[i]->name);
            else if (fn->params[i]->kind == N_IDENT) {
                const char *psh = cmc_param_shape(fn->fname, fn->params[i]->name);
                if (psh) b_putf(&sh, "struct %s * %s", psh, fn->params[i]->name);
                else b_putf(&sh, "%s %s", cmc_type_name(pt[i]), fn->params[i]->name);
            } else b_put(&sh, "double p");
        }
        b_put(&sh, ");\n");
        b_putf(&sh, "static mochi_val __shim_%s(int argc, mochi_val *argv, void *env) {\n", fn->fname);
        b_put(&sh, "    (void)argc; (void)env;\n");
        b_put(&sh, "    ");
        if (rshape) b_putf(&sh, "struct %s *", rshape);
        else if (ret == CT_VOID) b_putf(&sh, "int");
        else b_putf(&sh, "%s", cmc_type_name(ret));
        b_putf(&sh, " __r = %s(", fn->fname);
        for (int i = 0; i < fn->nparams; i++) {
            if (i) b_put(&sh, ", ");
            const char *pn = (fn->params[i]->kind == N_IDENT) ? fn->params[i]->name : "p";
            if (fn->params[i]->is_rest) {
                b_putf(&sh, "(mochi_array_from_argv(argv, %d, argc))", i);
            } else if (pt[i] == CT_STR) {
                b_putf(&sh, "(argc > %d ? mochi_str_or_empty(argv[%d]) : \"\")", i, i);
            } else if (pt[i] == CT_ARR) {
                b_putf(&sh, "(argc > %d ? mochi_arr_or_empty(argv[%d]) : NULL)", i, i);
            } else if (pt[i] == CT_OBJ) {
                const char *psh = (fn->params[i]->kind == N_IDENT)
                    ? cmc_param_shape(fn->fname, pn) : NULL;
                b_putf(&sh, "(argc > %d && argv[%d].p ? (struct %s *)argv[%d].p : NULL)",
                       i, i, psh ? psh : "CmcStruct_1", i);
            } else if (pt[i] == CT_MOCHI_VAL) {
                b_putf(&sh, "(argc > %d ? argv[%d] : mochi_undef())", i, i);
            } else if (pt[i] == CT_BOOL) {
                b_putf(&sh, "(argc > %d ? mochi_val_bool(argv[%d]) : 0)", i, i);
            } else {
                b_putf(&sh, "(argc > %d ? mochi_val_num(argv[%d]) : 0)", i, i);
            }
        }
        b_put(&sh, ");\n");
        b_put(&sh, "    ");
        if (ret == CT_STR) b_put(&sh, "return mochi_str(__r);\n");
        else if (ret == CT_BOOL) b_put(&sh, "return mochi_bool(__r);\n");
        else if (ret == CT_ARR) b_put(&sh, "return mochi_arr_box(__r);\n");
        else if (ret == CT_OBJ && rshape) b_put(&sh, "return mochi_obj_box(__r);\n");
        else if (ret == CT_MOCHI_VAL) b_put(&sh, "return __r;\n");
        else b_put(&sh, "return mochi_num((double)__r);\n");
        b_put(&sh, "}\n");
        b_put(&em->fn_extra, sh.s);
        free(sh.s);
    }
    cmc_env_pop(&em->env);
    free(pt);
}

/* ---------------------- entry ---------------------- */
