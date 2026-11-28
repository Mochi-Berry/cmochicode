/*
 * ir.c — stage 3 of the cmochicode pipeline: typed Micro-IR.
 *  3a: type inference over the AST (with a scoped symbol table)
 *  3b: escape analysis (which heap values leave their function)
 *
 * The IR is a lightweight annotation layer: cmc_type_of() and the escape
 * query feed the C99 emitter. Feature lowering (arrow functions, ?., ??,
 * destructuring, template folding) happens here and in the emitter so the
 * output is pure C99.
 */
#include "cmochi.h"

/* ------------------------- symbol table ------------------------- */
void cmc_env_init(CmcEnv *env, CmcArena *arena) {
    env->stack = NULL;
    cmc_env_push(env, arena);
}

void cmc_env_push(CmcEnv *env, CmcArena *arena) {
    CmcScopeNode *s = (CmcScopeNode *)cmc_arena_alloc(arena, sizeof(CmcScopeNode));
    s->scope.syms = NULL; s->scope.n = 0; s->scope.cap = 0;
    s->up = env->stack;
    env->stack = s;
}

void cmc_env_pop(CmcEnv *env) {
    if (env->stack && env->stack->up) env->stack = env->stack->up;
}

CmcSym *cmc_env_decl(CmcEnv *env, const char *name, CmcType type, CmcSymKind kind) {
    CmcScope *s = &env->stack->scope;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        CmcSym *ns = (CmcSym *)realloc(s->syms, sizeof(CmcSym) * s->cap);
        s->syms = ns;
    }
    CmcSym *sym = &s->syms[s->n++];
    memset(sym, 0, sizeof(*sym));
    sym->name = name;
    sym->cname = name;
    sym->type = type;
    sym->kind = kind;
    return sym;
}

CmcSym *cmc_env_lookup(CmcEnv *env, const char *name) {
    for (CmcScopeNode *s = env->stack; s; s = s->up) {
        for (int i = 0; i < s->scope.n; i++) {
            if (strcmp(s->scope.syms[i].name, name) == 0) return &s->scope.syms[i];
        }
    }
    return NULL;
}

const char *cmc_cname(CmcEnv *env, const char *name) {
    CmcSym *s = cmc_env_lookup(env, name);
    return s ? s->cname : name;
}

/* ------------------------- type helpers ------------------------- */
int cmc_is_numeric(CmcType t) {
    return t == CT_INT || t == CT_INT64 || t == CT_DOUBLE;
}

const char *cmc_type_name(CmcType t) {
    switch (t) {
        case CT_INT:    return "int";
        case CT_INT64:  return "int64_t";
        case CT_DOUBLE: return "double";
        case CT_STR:    return "const char *";
        case CT_BOOL:   return "int";
        case CT_OBJ:    return "void *";
        case CT_ARR:    return "mochi_array *";
        case CT_NULL:   return "void *";
        case CT_UNDEF:  return "void *";
        case CT_VOID:   return "void";
        case CT_UNKNOWN:return "double";
        case CT_MOCHI_VAL: return "mochi_val";
    }
    return "double";
}

static CmcType promote(CmcType a, CmcType b) {
    if (a == CT_STR || b == CT_STR) return CT_STR;
    if (a == CT_MOCHI_VAL || b == CT_MOCHI_VAL) return CT_MOCHI_VAL;
    if (a == CT_DOUBLE || b == CT_DOUBLE) return CT_DOUBLE;
    if (a == CT_INT64 || b == CT_INT64) return CT_INT64;
    if (cmc_is_numeric(a) && cmc_is_numeric(b)) return CT_INT;
    if (a == CT_NULL) return b;
    if (b == CT_NULL) return a;
    if (a == CT_OBJ || b == CT_OBJ) return CT_OBJ;
    if (a == CT_ARR || b == CT_ARR) return CT_ARR;
    return CT_UNKNOWN;
}

/* Struct shape registry: object literals -> struct name + field types. */
typedef struct {
    const char *sig;
    const char *name;
    CmcNode    *literal;
    const char *field_names[32];
    CmcType     field_types[32];
    const char *field_shapes[32];   /* inner shape of an object-typed field */
    int         nfields;
} CmcShape;

typedef struct {
    CmcShape *shapes;
    int       n, cap;
} CmcShapes;

static CmcShapes g_shapes;

static const char *shape_sig(CmcArena *arena, CmcNode *obj) {
    /* build a signature string like "x:int,y:str" */
    char buf[512]; size_t p = 0;
    for (int i = 0; i < obj->nprops; i++) {
        CmcProp *pr = &obj->props[i];
        if (pr->spread || pr->key == NULL) continue; /* spread handled at emit (limited) */
        CmcType t = CT_UNKNOWN;
        if (pr->value) {
            CmcNode *v = pr->value;
            if (v->kind == N_NUM) t = (v->num == (long long)v->num) ? CT_INT : CT_DOUBLE;
            else if (v->kind == N_BIGINT) t = CT_INT64;
            else if (v->kind == N_STR) t = CT_STR;
            else if (v->kind == N_BOOL) t = CT_BOOL;
            else if (v->kind == N_NULL) t = CT_NULL;
            else if (v->kind == N_ARR) t = CT_ARR;
            else if (v->kind == N_OBJ) t = CT_OBJ;
        }
        int n = snprintf(buf + p, sizeof(buf) - p, "%s:%d,", pr->key, (int)t);
        p += n;
    }
    return cmc_arena_strndup(arena, buf, p);
}

/* Register an object literal; return its struct name. */
const char *cmc_register_shape(CmcArena *arena, CmcNode *obj) {
    /* Key shapes per literal INSTANCE (by node identity), not per signature:
     * two `{a:1}` literals are independent objects, so a dynamic field added
     * to one (d.x = 5) must not leak into the other as an uninitialized field.
     * The same AST node is seen in every pass, so the name is stable. */
    const char *sig = shape_sig(arena, obj);
    for (int i = 0; i < g_shapes.n; i++) {
        if (g_shapes.shapes[i].literal == obj) return g_shapes.shapes[i].name;
    }
    if (g_shapes.n == g_shapes.cap) {
        g_shapes.cap = g_shapes.cap ? g_shapes.cap * 2 : 16;
        CmcShape *ns = (CmcShape *)realloc(g_shapes.shapes, sizeof(CmcShape) * g_shapes.cap);
        g_shapes.shapes = ns;
    }
    CmcShape *sh = &g_shapes.shapes[g_shapes.n++];
    memset(sh, 0, sizeof(*sh));
    sh->sig = sig;
    char nm[32]; snprintf(nm, sizeof nm, "CmcStruct_%d", g_shapes.n);
    sh->name = cmc_arena_strndup(arena, nm, strlen(nm));
    sh->literal = obj;
    int nf = 0;
    for (int i = 0; i < obj->nprops && i < 32; i++)
        if (!(obj->props[i].spread || obj->props[i].key == NULL)) nf++;
    sh->nfields = nf;
    int fi = 0;
    for (int i = 0; i < obj->nprops && i < 32; i++) {
        if (obj->props[i].spread || obj->props[i].key == NULL) continue; /* spread: skip */
        sh->field_names[fi] = obj->props[i].key;
        CmcNode *v = obj->props[i].value;
        CmcType t = CT_UNKNOWN;
        if (v) {
            if (v->kind == N_NUM) t = (v->num == (long long)v->num) ? CT_INT : CT_DOUBLE;
            else if (v->kind == N_BIGINT) t = CT_INT64;
            else if (v->kind == N_STR) t = CT_STR;
            else if (v->kind == N_BOOL) t = CT_BOOL;
            else if (v->kind == N_NULL) t = CT_NULL;
            else if (v->kind == N_ARR) t = CT_ARR;
            else if (v->kind == N_OBJ) {
                            t = CT_OBJ;
                            /* a nested object literal gets its own shape, so the field is
                               typed `struct X *` and can be printed recursively */
                            sh->field_shapes[fi] = cmc_register_shape(arena, v);
                        }
                        else if (v->kind == N_BIN) {
                /* string concatenation / template-ish expression */
                int ls = v->lhs && (v->lhs->kind == N_STR || v->lhs->kind == N_TEMPLATE);
                int rs = v->rhs && (v->rhs->kind == N_STR || v->rhs->kind == N_TEMPLATE);
                t = (ls || rs) ? CT_STR : CT_UNKNOWN;
            }
            else t = CT_UNKNOWN;
        }
        sh->field_types[fi] = t;
        fi++;
    }
    return sh->name;
}

const char *cmc_shape_name_for(const char *sig_or_name);
/* The C type of a field. An object-typed field with a registered inner shape
 * is typed `struct X *` (not void *), so nested objects keep their shape:
 * property access on them and recursive printing both work. */
static const char *field_type_str(const CmcShape *sh, int j) {
    if (sh->field_shapes[j]) {
        static char bufs[4][96]; static int bi = 0;
        bi = (bi + 1) & 3;
        snprintf(bufs[bi], sizeof bufs[bi], "struct %s *", sh->field_shapes[j]);
        return bufs[bi];
    }
    return cmc_type_name(sh->field_types[j]);
}
const char *cmc_struct_field_type(const char *struct_name, const char *field) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) == 0) {
            for (int j = 0; j < g_shapes.shapes[i].nfields; j++) {
                if (strcmp(g_shapes.shapes[i].field_names[j], field) == 0)
                    return field_type_str(&g_shapes.shapes[i], j);
            }
        }
    }
    return NULL; /* field not in shape: caller decides (dynamic add / default) */
}

/* Semantic (CmcType) of a named field — lets the emitter tell `bool` (CT_BOOL,
   C type "int") from a real integer, since both map to "int" in C. */
CmcType cmc_struct_field_kind(const char *struct_name, const char *field) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) == 0) {
            for (int j = 0; j < g_shapes.shapes[i].nfields; j++) {
                if (strcmp(g_shapes.shapes[i].field_names[j], field) == 0)
                    return g_shapes.shapes[i].field_types[j];
            }
        }
    }
    return CT_UNKNOWN;
}

/* Field access by index — used by the emitter to print object literals. */
const char *cmc_struct_field_name_n(const char *struct_name, int idx) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) == 0) {
            if (idx < 0 || idx >= g_shapes.shapes[i].nfields) return NULL;
            return g_shapes.shapes[i].field_names[idx];
        }
    }
    return NULL;
}
const char *cmc_struct_field_type_n(const char *struct_name, int idx) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) == 0) {
            if (idx < 0 || idx >= g_shapes.shapes[i].nfields) return NULL;
            return field_type_str(&g_shapes.shapes[i], idx);
        }
    }
    return NULL;
}

/* Registered-shape iteration (used by the emitter to emit struct defs). */
int cmc_shape_count(void) { return g_shapes.n; }
const char *cmc_shape_name(int i) { return (i >= 0 && i < g_shapes.n) ? g_shapes.shapes[i].name : NULL; }
int cmc_shape_field_count(int i) { return (i >= 0 && i < g_shapes.n) ? g_shapes.shapes[i].nfields : 0; }
const char *cmc_shape_field_name(int i, int j) {
    if (i < 0 || i >= g_shapes.n || j < 0 || j >= g_shapes.shapes[i].nfields) return NULL;
    return g_shapes.shapes[i].field_names[j];
}
const char *cmc_shape_field_type(int i, int j) {
    if (i < 0 || i >= g_shapes.n || j < 0 || j >= g_shapes.shapes[i].nfields) return NULL;
    return field_type_str(&g_shapes.shapes[i], j);
}
/* The inner shape of an object-typed field (NULL for other fields). */
const char *cmc_struct_field_shape(const char *struct_name, const char *field) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) != 0) continue;
        for (int j = 0; j < g_shapes.shapes[i].nfields; j++)
            if (strcmp(g_shapes.shapes[i].field_names[j], field) == 0)
                return g_shapes.shapes[i].field_shapes[j];
    }
    return NULL;
}

/* Merge a dynamically-assigned field into a registered shape (JS allows
 * point.label = "P" on an object literal that only had {x, y}). */
/* Shapes that use dynamic-key dispatch (and thus need the `extra` overflow
 * map field in their struct). Global across passes: the dry pass discovers the
 * need, the real pass's literal allocation (which runs before the dynamic-key
 * use) must still know to init `extra` to NULL. */
static const char *g_dk_shapes[64];
static int g_dk_shapes_n = 0;
void cmc_dk_register(const char *sn) {
    if (!sn) return;
    for (int i = 0; i < g_dk_shapes_n; i++) if (strcmp(g_dk_shapes[i], sn) == 0) return;
    if (g_dk_shapes_n < 64) g_dk_shapes[g_dk_shapes_n++] = sn;
}
int cmc_dk_is(const char *sn) {
    if (!sn) return 0;
    for (int i = 0; i < g_dk_shapes_n; i++) if (strcmp(g_dk_shapes[i], sn) == 0) return 1;
    return 0;
}

/* Fields added by member-assign promotion. A shape that ends up with a
 * dynamic overflow map (dk) must NOT keep these as static fields, or the
 * console.log order would diverge from JS insertion order and the map would
 * be bypassed. We record the promotion, then at the end of the dry passes
 * (cmc_shape_flush_promotions) drop the field from any shape that became dk. */
typedef struct { const char *shape; const char *field; } CmcPromoted;
static CmcPromoted g_promoted[128]; static int g_promoted_n = 0;
void cmc_shape_mark_promoted(const char *sn, const char *field) {
    if (!sn || !field) return;
    for (int i = 0; i < g_promoted_n; i++)
        if (g_promoted[i].shape == sn && g_promoted[i].field == field) return;
    if (g_promoted_n < 128) { g_promoted[g_promoted_n].shape = sn; g_promoted[g_promoted_n].field = field; g_promoted_n++; }
}
void cmc_shape_remove_field(const char *sn, const char *field) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, sn) != 0) continue;
        for (int j = 0; j < g_shapes.shapes[i].nfields; j++)
            if (strcmp(g_shapes.shapes[i].field_names[j], field) == 0) {
                for (int k = j; k < g_shapes.shapes[i].nfields - 1; k++) {
                    g_shapes.shapes[i].field_names[k] = g_shapes.shapes[i].field_names[k+1];
                    g_shapes.shapes[i].field_types[k] = g_shapes.shapes[i].field_types[k+1];
                }
                g_shapes.shapes[i].nfields--;
                return;
            }
        return;
    }
}
void cmc_shape_flush_promotions(void) {
    for (int i = 0; i < g_promoted_n; i++)
        if (cmc_dk_is(g_promoted[i].shape))
            cmc_shape_remove_field(g_promoted[i].shape, g_promoted[i].field);
}
/* Call-site shape inference for polymorphic object params: a function
 * parameter that receives an object at a call site gets the object's shape,
 * so `fill(q)` with q = {v:1} types fill's `q` as `struct <shape> *`. */
typedef struct { const char *fn; const char *param; const char *sname; CmcType type; } CmcParamShape;
static CmcParamShape g_param_shapes[64];
static int g_param_shapes_n = 0;
void cmc_param_shapes_reset(void) { g_param_shapes_n = 0; }
void cmc_record_param_shape(const char *fn, const char *param, const char *sname) {
    if (!fn || !param || !sname) return;
    for (int i = 0; i < g_param_shapes_n; i++)
        if (strcmp(g_param_shapes[i].fn, fn) == 0 && strcmp(g_param_shapes[i].param, param) == 0) {
            g_param_shapes[i].sname = sname;
            g_param_shapes[i].type = CT_OBJ;
            return;
        }
    if (g_param_shapes_n < 64) {
        g_param_shapes[g_param_shapes_n].fn = fn;
        g_param_shapes[g_param_shapes_n].param = param;
        g_param_shapes[g_param_shapes_n].sname = sname;
        g_param_shapes[g_param_shapes_n].type = CT_OBJ;
        g_param_shapes_n++;
    }
}
const char *cmc_param_shape(const char *fn, const char *param) {
    if (!fn || !param) return NULL;
    for (int i = 0; i < g_param_shapes_n; i++)
        if (strcmp(g_param_shapes[i].fn, fn) == 0 && strcmp(g_param_shapes[i].param, param) == 0)
            return g_param_shapes[i].sname;
    return NULL;
}
void cmc_record_param_type(const char *fn, const char *param, CmcType t) {
    if (!fn || !param || t == CT_UNKNOWN || t == CT_VOID) return;
    if (t == CT_NULL || t == CT_UNDEF) t = CT_MOCHI_VAL;
    for (int i = 0; i < g_param_shapes_n; i++)
        if (strcmp(g_param_shapes[i].fn, fn) == 0 && strcmp(g_param_shapes[i].param, param) == 0) {
            CmcType old = g_param_shapes[i].type;
            g_param_shapes[i].type = (old == CT_UNKNOWN || old == t) ? t : CT_MOCHI_VAL;
            return;
        }
    if (g_param_shapes_n < 64) {
        g_param_shapes[g_param_shapes_n].fn = fn;
        g_param_shapes[g_param_shapes_n].param = param;
        g_param_shapes[g_param_shapes_n].sname = NULL;
        g_param_shapes[g_param_shapes_n].type = t;
        g_param_shapes_n++;
    }
}
CmcType cmc_param_type(const char *fn, const char *param) {
    if (!fn || !param) return CT_UNKNOWN;
    for (int i = 0; i < g_param_shapes_n; i++)
        if (strcmp(g_param_shapes[i].fn, fn) == 0 && strcmp(g_param_shapes[i].param, param) == 0)
            return g_param_shapes[i].type;
    return CT_UNKNOWN;
}

/* Change an existing field's type (e.g. widen int -> double, or any static
 * type -> CT_MOCHI_VAL when a value of an incompatible type is assigned).
 * The struct definition is emitted AFTER the whole body, so this is visible
 * to every read in the program once a pass has run. */
void cmc_shape_widen_field(const char *struct_name, const char *field, CmcType t) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) != 0) continue;
        for (int j = 0; j < g_shapes.shapes[i].nfields; j++)
            if (strcmp(g_shapes.shapes[i].field_names[j], field) == 0) {
                g_shapes.shapes[i].field_types[j] = t;
                return;
            }
        return;
    }
}

void cmc_shape_add_field(const char *struct_name, const char *field, CmcType t) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) != 0) continue;
        for (int j = 0; j < g_shapes.shapes[i].nfields; j++)
            if (strcmp(g_shapes.shapes[i].field_names[j], field) == 0) return; /* already there */
        if (g_shapes.shapes[i].nfields < 32) {
            int n = g_shapes.shapes[i].nfields++;
            g_shapes.shapes[i].field_names[n] = field;
            g_shapes.shapes[i].field_types[n] = t;
        }
        return;
    }
}

/* Reorder a shape's fields to match JS property insertion order (spread
   sources first, then literal props). Kinds/names travel with the field. */
void cmc_shape_reorder(const char *struct_name, const char **order, int n) {
    for (int i = 0; i < g_shapes.n; i++) {
        if (strcmp(g_shapes.shapes[i].name, struct_name) != 0) continue;
        const char *oldn[32]; CmcType oldt[32];
        int on = g_shapes.shapes[i].nfields;
        for (int j = 0; j < on && j < 32; j++) { oldn[j] = g_shapes.shapes[i].field_names[j]; oldt[j] = g_shapes.shapes[i].field_types[j]; }
        int w = 0;
        for (int j = 0; j < n && w < 32; j++) {
            for (int k = 0; k < on && k < 32; k++) {
                if (strcmp(oldn[k], order[j]) == 0) {
                    g_shapes.shapes[i].field_names[w] = oldn[k];
                    g_shapes.shapes[i].field_types[w] = oldt[k];
                    w++; break;
                }
            }
        }
        /* Fields that were added dynamically (o.x = 1) are not part of the
         * literal's insertion order: keep them (and their current type)
         * instead of dropping them from the shape. */
        for (int k = 0; k < on && w < 32; k++) {
            int listed = 0;
            for (int j = 0; j < n; j++) if (strcmp(oldn[k], order[j]) == 0) { listed = 1; break; }
            if (listed) continue;
            g_shapes.shapes[i].field_names[w] = oldn[k];
            g_shapes.shapes[i].field_types[w] = oldt[k];
            w++;
        }
        g_shapes.shapes[i].nfields = w;
        return;
    }
}

/* ------------------------- type inference ------------------------- */
CmcType cmc_type_of(CmcCtx *ctx, CmcEnv *env, CmcNode *n) {
    if (!n) return CT_UNKNOWN;
    switch (n->kind) {
        case N_NUM:    return (n->num == (long long)n->num) ? CT_INT : CT_DOUBLE;
        case N_BIGINT: return CT_INT64;
        case N_STR:    return CT_STR;
        case N_BOOL:   return CT_BOOL;
        case N_NULL:   return CT_NULL;
        case N_UNDEF:  return CT_UNDEF;
        case N_THIS:   return CT_UNKNOWN;
        case N_IDENT: {
            CmcSym *s = cmc_env_lookup(env, n->name);
            return s ? s->type : CT_UNKNOWN;
        }
        case N_MEMBER: {
            CmcType ot = cmc_type_of(ctx, env, n->obj);
            if (n->computed) return ot == CT_ARR ? CT_MOCHI_VAL : (ot == CT_STR ? CT_STR : CT_UNKNOWN);
            const char *prop = n->prop ? n->prop->name : "";
            if (ot == CT_ARR && strcmp(prop, "length") == 0) return CT_INT;
            if (ot == CT_STR && strcmp(prop, "length") == 0) return CT_INT;
            if (ot == CT_STR) return CT_STR;
            if (ot == CT_OBJ) {
                /* look up symbol for struct */
                if (n->obj->kind == N_IDENT) {
                    CmcSym *s = cmc_env_lookup(env, n->obj->name);
                    if (s && s->struct_name) {
                        for (int i = 0; i < g_shapes.n; i++) {
                            if (strcmp(g_shapes.shapes[i].name, s->struct_name) == 0) {
                                for (int j = 0; j < g_shapes.shapes[i].nfields; j++) {
                                    if (strcmp(g_shapes.shapes[i].field_names[j], prop) == 0)
                                        return g_shapes.shapes[i].field_types[j];
                                }
                            }
                        }
                    }
                }
                return CT_UNKNOWN;
            }
            return CT_UNKNOWN;
        }
        case N_CALL: {
            CmcNode *callee = n->obj;
            if (callee->kind == N_IDENT) {
                const char *nm = callee->name;
                if (strcmp(nm, "parseInt") == 0 || strcmp(nm, "parseFloat") == 0 ||
                    strcmp(nm, "Number") == 0 || strcmp(nm, "isNaN") == 0 ||
                    strcmp(nm, "isFinite") == 0) return CT_DOUBLE;
                if (strcmp(nm, "String") == 0) return CT_STR;
                if (strcmp(nm, "Boolean") == 0) return CT_BOOL;
                CmcSym *s = cmc_env_lookup(env, nm);
                if (s && s->kind == SYM_FUNC) return s->type;
                /* a variable/param used as a callee is a boxed callable */
                if (s && (s->kind == SYM_LOCAL || s->kind == SYM_PARAM)) return CT_MOCHI_VAL;
                return CT_UNKNOWN;
            }
            if (callee->kind == N_MEMBER) {
                CmcType ot = cmc_type_of(ctx, env, callee->obj);
                const char *prop = callee->prop ? callee->prop->name : "";
                if (ot == CT_ARR) {
                    if (strcmp(prop, "push") == 0 || strcmp(prop, "pop") == 0 ||
                        strcmp(prop, "length") == 0) return CT_INT;
                    if (strcmp(prop, "join") == 0) return CT_STR;
                }
                if (ot == CT_STR) {
                    if (strcmp(prop, "indexOf") == 0 || strcmp(prop, "includes") == 0 ||
                        strcmp(prop, "startsWith") == 0) return CT_INT;
                    return CT_STR;
                }
            }
            return CT_UNKNOWN;
        }
        case N_BIN: {
            if (n->op == TK_PLUS) {
                CmcType l = cmc_type_of(ctx, env, n->lhs);
                CmcType r = cmc_type_of(ctx, env, n->rhs);
                if (l == CT_STR || r == CT_STR) return CT_STR;
                return promote(l, r);
            }
            if (n->op == TK_SLASH) return CT_DOUBLE;
            if (n->op == TK_PERCENT) return CT_INT;
            if (n->op == TK_EQEQ || n->op == TK_EQEQEQ || n->op == TK_NE || n->op == TK_NEEQ ||
                n->op == TK_LT || n->op == TK_LE || n->op == TK_GT || n->op == TK_GE) return CT_BOOL;
            if (n->op == TK_ANDAND || n->op == TK_OROR || n->op == TK_NULLISH)
                return promote(cmc_type_of(ctx, env, n->lhs), cmc_type_of(ctx, env, n->rhs));
            return CT_INT;
        }
        case N_UNARY:
            if (n->op == TK_TYPEOF) return CT_STR;
            if (n->op == TK_NOT || n->op == TK_TILDE) return CT_INT;
            return cmc_type_of(ctx, env, n->lhs);
        case N_ASSIGN:
            return n->rhs ? cmc_type_of(ctx, env, n->rhs) : cmc_type_of(ctx, env, n->lhs);
        case N_UPDATE:
            return cmc_type_of(ctx, env, n->lhs);
        case N_COND:
            return promote(cmc_type_of(ctx, env, n->rhs), cmc_type_of(ctx, env, n->third));
        case N_ARR:  return CT_ARR;
        case N_OBJ:  return CT_OBJ;
        case N_TEMPLATE: return CT_STR;
        case N_SEQ:  return n->nstmts ? cmc_type_of(ctx, env, n->stmts[n->nstmts - 1]) : CT_UNDEF;
        case N_NULLISH:
            return promote(cmc_type_of(ctx, env, n->lhs), cmc_type_of(ctx, env, n->rhs));
        default: return CT_UNKNOWN;
    }
}

/* ------------------------- escape analysis ------------------------- */
static int uses_as_arg(CmcNode *node, const char *name) {
    if (!node) return 0;
    if (node->kind == N_IDENT && strcmp(node->name, name) == 0) return 1;
    if (node->kind == N_CALL) {
        for (int i = 0; i < node->nargs; i++) {
            if (uses_as_arg(node->args[i], name)) return 1;
        }
        return uses_as_arg(node->obj, name);
    }
    if (node->kind == N_MEMBER) {
        return uses_as_arg(node->obj, name) || uses_as_arg(node->prop, name);
    }
    if (node->kind == N_BIN) return uses_as_arg(node->lhs, name) || uses_as_arg(node->rhs, name);
    if (node->kind == N_ASSIGN) return uses_as_arg(node->lhs, name) || uses_as_arg(node->rhs, name);
    if (node->kind == N_RETURN) return uses_as_arg(node->lhs, name);
    return 0;
}

int cmc_var_escapes(CmcNode *fn, const char *name) {
    /* conservative: escapes if the value is returned or passed as an arg */
    return uses_as_arg(fn, name);
}
