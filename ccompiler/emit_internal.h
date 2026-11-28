/* emit_internal.h — shared types + prototypes for the C99 emitter.
 * Split from emit.c into per-concern TUs: helpers / expr / stmt / class.
 */
#ifndef CMC_EMIT_INTERNAL_H
#define CMC_EMIT_INTERNAL_H

#include "cmochi.h"
#include <stdarg.h>

/* ---------------------- growable string buffer ---------------------- */
typedef struct { char *s; size_t len, cap; } Buf;

/* ---------------------- emitter state ---------------------- */
typedef struct { char *code; CmcType type; int escapes; int ptr; const char *struct_name; } EStr;

typedef struct {
    CmcCtx   *ctx;
    CmcEnv    env;
    Buf       out;
    Buf       structs;
    Buf       fn_extra;
    Buf       pre;
    int       indent;
    int       need_math, need_string, need_stdint;
    int       tmp, fnid;
    CmcNode  *cur_fn;
    const char *u_names[128];
    CmcType     u_types[128];
    int         u_n;
    int       try_depth;
    const char *last_thrown;
    /* locals assigned inside a try/catch must be volatile: a longjmp makes
       any auto variable modified between setjmp and longjmp indeterminate */
    const char *vol_names[64]; int vol_n;
    const char *m_shape[64]; const char *m_field[64]; const char *m_fn[64];
    CmcNode   *m_node[64];
    int        m_guarded[64];
    int        m_nparams[64];
    CmcType    m_ret[64];
    int         m_n;
    const char *this_name;
    const char *cl_name[32]; const char *cl_struct[32]; const char *cl_ctor[32];
    int         cl_n;
    const char *cstr_name[64]; const char *cstr_val[64]; int cstr_count[64]; int cstr_n;
    const char *dk_shapes[64]; int dk_n;   /* shapes with runtime-key get/set helpers */
    const char *keys_shapes[64]; int keys_n;  /* shapes with a _keys helper */
    const char *asg_pairs[64]; int asg_n;      /* 'src|tgt' Object.assign helpers */
} Emit;

/* prototypes (all functions share the Emit/Buf/EStr types) */
void b_init(Buf *b);
void b_ensure(Buf *b, size_t n);
void b_put(Buf *b, const char *s);
void b_putn(Buf *b, const char *s, size_t n);
void b_putf(Buf *b, const char *fmt, ...);
EStr estr(const char *code, CmcType t, int esc, int ptr, const char *sn);
void e_line(Emit *em, const char *s);
void        ensure_keys_helpers(Emit *em, const char *sn);
void        cmc_emit_str_helper(Emit *em, const char *sn);
void        cmc_emit_json_helper(Emit *em, const char *sn);
void e_linef(Emit *em, const char *fmt, ...);
void e_preline(Emit *em, const char *s);
void flush_pre(Emit *em);
const char *tmp_name(Emit *em, const char *base);
void c_lit(Buf *b, const char *s);
void co_num(Emit *em, Buf *b, EStr e);
void co_int(Buf *b, EStr e);
void co_str(Emit *em, Buf *b, EStr e);
void co_bool(Emit *em, Buf *b, EStr e);
void co_box(Buf *b, EStr e);
void member_base(Emit *em, Buf *b, EStr o);
void num_lit(Buf *b, double v);
void farg_push(Buf *fargs, const char *code);
void append_print_arg(Emit *em, CmcNode *node, EStr e, Buf *fmt, Buf *fargs);
int expr_has_side_effect(CmcNode *n);
const char *kfold(Emit *em, CmcNode *n);      /* fold expr -> const string, or NULL */
void collect_cstr(Emit *em, CmcNode *n);      /* pre-pass: collect const string bindings */
void cstr_count_pass(Emit *em, CmcNode *n);   /* pre-pass: count assignments per name */
void emit_console(Emit *em, CmcNode **args, int nargs, int to_stderr);
EStr emit_literal(Emit *em, CmcNode *n);
EStr emit_ident(Emit *em, CmcNode *n);
EStr emit_member(Emit *em, CmcNode *n);
EStr emit_member_field(Emit *em, EStr o, CmcNode *obj_node, const char *prop, int optional);
EStr emit_bin(Emit *em, CmcNode *n);
EStr emit_unary(Emit *em, CmcNode *n);
EStr emit_assign(Emit *em, CmcNode *n);
EStr emit_update(Emit *em, CmcNode *n);
EStr emit_cond(Emit *em, CmcNode *n);
EStr emit_array(Emit *em, CmcNode *n);
EStr emit_object(Emit *em, CmcNode *n);
EStr emit_template(Emit *em, CmcNode *n);
EStr emit_arrow(Emit *em, CmcNode *n);
void arg_has(Emit *em, CmcNode *arg, Buf *out);
EStr emit_arr_higher(Emit *em, CmcNode *n, const char *prop, EStr o);
const char *build_rest_array(Emit *em, EStr *args, int ri, int nargs);
void class_strctx(CmcNode *n, const char ***ctx, int *nn, int *cap);
CmcType class_lit_type(CmcNode *n, const char *sn);
void class_scan(CmcNode *n, const char ***names, CmcNode ***vals, int *nn, int *cap);
CmcType class_param_type(CmcNode *n, const char *sn, const char *pname);
CmcType class_ret_type(CmcNode *n, const char *sn);
const char *emit_class_method(Emit *em, CmcNode *cls, CmcProp *pr,                                     const char *sn, int has_ctor);
void emit_class(Emit *em, CmcNode *n);
EStr emit_new(Emit *em, CmcNode *n);
EStr emit_call(Emit *em, CmcNode *n);
EStr emit_expr(Emit *em, CmcNode *n);
void uninit_record(Emit *em, const char *name);
CmcType uninit_type(Emit *em, const char *name);
void uninit_set(Emit *em, const char *name, CmcType t);
void uninit_collect(Emit *em, CmcNode *s);
void uninit_resolve(Emit *em, CmcNode *s);
void uninit_prepass(Emit *em, CmcNode *block);
void emit_var_decl(Emit *em, CmcNode *n);
void vol_pass(Emit *em, CmcNode *block);
void emit_if(Emit *em, CmcNode *n);
void emit_block(Emit *em, CmcNode *n);
void emit_while(Emit *em, CmcNode *n);
void emit_for(Emit *em, CmcNode *n);
void emit_forof(Emit *em, CmcNode *n);
void emit_forin(Emit *em, CmcNode *n);
void emit_switch(Emit *em, CmcNode *n);
void emit_stmt(Emit *em, CmcNode *n);
CmcType fn_return_type(CmcNode *fn);
CmcType infer_param_type_lite(Emit *em, CmcNode *fn, const char *pname);
void emit_function(Emit *em, CmcNode *fn);
char *cmc_emit(CmcCtx *ctx, CmcNode *program);

#endif /* CMC_EMIT_INTERNAL_H */
