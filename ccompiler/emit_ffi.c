#include "emit_internal.h"

static int ffi_type(const char *s, size_t n, CmcType *out) {
    if (n == 4 && memcmp(s, "void", 4) == 0) { *out = CT_VOID; return 1; }
    if (n == 2 && memcmp(s, "i8", 2) == 0) { *out = CT_INT; return 1; }
    if (n == 2 && memcmp(s, "u8", 2) == 0) { *out = CT_INT; return 1; }
    if (n == 3 && memcmp(s, "i16", 3) == 0) { *out = CT_INT; return 1; }
    if (n == 3 && memcmp(s, "u16", 3) == 0) { *out = CT_INT; return 1; }
    if (n == 3 && memcmp(s, "i32", 3) == 0) { *out = CT_INT; return 1; }
    if (n == 3 && memcmp(s, "u32", 3) == 0) { *out = CT_INT; return 1; }
    if (n == 4 && memcmp(s, "bool", 4) == 0) { *out = CT_INT; return 1; }
    if (n == 3 && memcmp(s, "int", 3) == 0) { *out = CT_INT; return 1; }
    if (n == 4 && memcmp(s, "uint", 4) == 0) { *out = CT_INT; return 1; }
    if (n == 3 && memcmp(s, "i64", 3) == 0) { *out = CT_INT64; return 1; }
    if (n == 3 && memcmp(s, "u64", 3) == 0) { *out = CT_INT64; return 1; }
    if (n == 4 && memcmp(s, "size", 4) == 0) { *out = CT_INT64; return 1; }
    if (n == 3 && memcmp(s, "f32", 3) == 0) { *out = CT_DOUBLE; return 1; }
    if (n == 3 && memcmp(s, "f64", 3) == 0) { *out = CT_DOUBLE; return 1; }
    if (n == 3 && memcmp(s, "ptr", 3) == 0) { *out = CT_STR; return 1; }
    if (n == 3 && memcmp(s, "str", 3) == 0) { *out = CT_STR; return 1; }
    if (n == 4 && memcmp(s, "char", 4) == 0) { *out = CT_STR; return 1; }
    return 0;
}

static void trim_type(const char **p, size_t *n) {
    while (*n && (**p == ' ' || **p == '\t')) { (*p)++; (*n)--; }
    while (*n && ((*p)[*n - 1] == ' ' || (*p)[*n - 1] == '\t')) (*n)--;
}

int cmc_ffi_parse_type(const char *type, CmcType *out) {
    if (!type || !out) return 0;
    const char *t = type;
    size_t n = strlen(t);
    trim_type(&t, &n);
    return ffi_type(t, n, out);
}

int cmc_ffi_parse_sig(const char *sig, CmcType *ret, CmcType *args,
                      int maxargs, int *nargs) {
    if (!sig || !ret || !args || !nargs) return 0;
    const char *lp = strchr(sig, '(');
    const char *rp = lp ? strrchr(lp, ')') : NULL;
    if (!lp || !rp || rp < lp) return 0;
    const char *rs = lp;
    while (rs > sig && rs[-1] != ' ' && rs[-1] != '\t' && rs[-1] != ':') rs--;
    if (!ffi_type(rs, (size_t)(lp - rs), ret)) return 0;
    int n = 0;
    const char *p = lp + 1;
    while (p < rp && n < maxargs) {
        const char *end = p;
        while (end < rp && *end != ',') end++;
        const char *t = p;
        size_t tn = (size_t)(end - t);
        trim_type(&t, &tn);
        if (!tn) { if (end == rp) break; p = end + 1; continue; }
        if (!ffi_type(t, tn, &args[n])) return 0;
        n++;
        if (end == rp) break;
        p = end + 1;
    }
    *nargs = n;
    return 1;
}

static const char *ffi_ret_type(CmcType t) {
    return t == CT_VOID ? "void" : cmc_type_name(t);
}

void cmc_ffi_emit_decl(Emit *em, const char *cname, const char *sig) {
    CmcType ret, args[16]; int na = 0;
    if (!cmc_ffi_parse_sig(sig, &ret, args, 16, &na)) return;
    em->need_ffi = 1;
    Buf b; b_init(&b);
    b_putf(&b, "static %s (*%s)(", ffi_ret_type(ret), cname);
    if (!na) b_put(&b, "void");
    for (int i = 0; i < na; i++) {
        if (i) b_put(&b, ", ");
        b_put(&b, cmc_type_name(args[i]));
        if (args[i] == CT_INT64) em->need_stdint = 1;
    }
    b_put(&b, ") = 0;\n");
    b_put(&em->fn_extra, b.s);
    free(b.s);
}

void cmc_ffi_emit_bind_stmts(Emit *em, Buf *out,
                             const char *cname, const char *file,
                             const char *sym) {
    em->need_ffi = 1;
    const char *h = tmp_name(em, "cmcdll");
    Buf fl, sl; b_init(&fl); b_init(&sl);
    c_lit(&fl, file ? file : "");
    c_lit(&sl, sym ? sym : "");
    b_putf(out, "void *%s = CMC_DL_OPEN(%s);\n", h, fl.s);
    b_putf(out, "if (!%s) mochi_throw(\"cmochicode: cannot open native library %s\");\n",
           h, file ? file : "");
    b_putf(out, "CMC_BIND_FN(%s, %s, %s);\n", cname, h, sl.s);
    b_putf(out, "if (!%s) mochi_throw(\"cmochicode: native symbol %s not found\");\n",
           cname, sym ? sym : "");
    free(fl.s); free(sl.s);
}

void cmc_ffi_emit_value(Emit *em, Buf *b, CmcType t, EStr e) {
    if (t == CT_STR) co_str(em, b, e);
    else if (t == CT_DOUBLE) co_num(em, b, e);
    else {
        Buf n; b_init(&n);
        co_num(em, &n, e);
        b_putf(b, "(%s)(%s)", cmc_type_name(t), n.s);
        free(n.s);
        if (t == CT_INT64) em->need_stdint = 1;
    }
}

void cmc_ffi_emit_args(Emit *em, Buf *b, const char *sig,
                       EStr *eargs, int nargs) {
    CmcType ret, at[16]; int na = 0;
    if (!cmc_ffi_parse_sig(sig, &ret, at, 16, &na)) na = 0;
    for (int i = 0; i < na; i++) {
        if (i) b_put(b, ", ");
        if (i >= nargs) { b_put(b, "0"); continue; }
        cmc_ffi_emit_value(em, b, at[i], eargs[i]);
    }
}

void cmc_ffi_emit_var_decl(Emit *em, const char *cname, CmcType t) {
    em->need_ffi = 1;
    Buf b; b_init(&b);
    b_putf(&b, "static %s *%s = 0;\n", cmc_type_name(t), cname);
    b_put(&em->fn_extra, b.s);
    free(b.s);
}

void cmc_ffi_emit_var_bind_stmts(Emit *em, Buf *out, const char *cname,
                                 const char *file, const char *sym,
                                 CmcType t) {
    em->need_ffi = 1;
    const char *h = tmp_name(em, "cmcdll");
    Buf fl, sl; b_init(&fl); b_init(&sl);
    c_lit(&fl, file ? file : "");
    c_lit(&sl, sym ? sym : "");
    b_putf(out, "void *%s = CMC_DL_OPEN(%s);\n", h, fl.s);
    b_putf(out, "if (!%s) mochi_throw(\"cmochicode: cannot open native library %s\");\n",
           h, file ? file : "");
    b_putf(out, "CMC_BIND_VAR(%s, %s, %s, %s);\n",
           cname, cmc_type_name(t), h, sl.s);
    b_putf(out, "if (!%s) mochi_throw(\"cmochicode: native variable %s not found\");\n",
           cname, sym ? sym : "");
    free(fl.s); free(sl.s);
}
