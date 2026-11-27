/*
 * cmochi.h — shared declarations for the cmochicode C99 native compiler.
 *
 * cmochicode is a JS -> C99 source compiler for MochiBerry. It runs as a
 * four-stage pipeline, all in C99, no tsc, no VM:
 *
 *   1. Lexer       — zero-copy tokenizer (slices into the source buffer)
 *   2. Parser      — Pratt/recursive-descent -> AST nodes in a bump arena
 *   3. Typed IR    — type inference, feature lowering, escape analysis
 *   4. C99 Emitter — pure C output, 3-tier memory (stack / CMC_ALLOC / free)
 *
 * The generated C is standalone or MochiBerry-native depending on
 * -DCMC_WITH_MOCHIBERRY (see runtime/cmochicode_rt.h).
 *
 * LICENSE: MIT
 */
#ifndef CMOCHI_H
#define CMOCHI_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Arena: bump allocator for AST/IR nodes — no malloc/free per node.   */
/* ------------------------------------------------------------------ */
typedef struct CmcChunk {
    struct CmcChunk *next;
    size_t used;
    size_t cap;
    unsigned char data[];
} CmcChunk;

typedef struct {
    CmcChunk *head;
} CmcArena;

void cmc_arena_init(CmcArena *a);
void *cmc_arena_alloc(CmcArena *a, size_t n);      /* zeroed, aligned */
char *cmc_arena_strndup(CmcArena *a, const char *s, size_t n);
void cmc_arena_free(CmcArena *a);

/* ------------------------------------------------------------------ */
/* Zero-copy slice into the source buffer.                             */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *ptr;
    size_t      len;
} CmcSlice;

static inline int cmc_slice_eq(CmcSlice a, const char *s) {
    size_t n = strlen(s);
    return a.len == n && memcmp(a.ptr, s, n) == 0;
}

/* ------------------------------------------------------------------ */
/* Tokens.                                                             */
/* ------------------------------------------------------------------ */
typedef enum {
    TK_EOF = 0,
    TK_IDENT, TK_NUM, TK_STR, TK_BIGINT,
    TK_VAR, TK_LET, TK_CONST, TK_FUNCTION, TK_IF, TK_ELSE, TK_WHILE,
    TK_DO, TK_FOR, TK_RETURN, TK_BREAK, TK_CONTINUE, TK_SWITCH, TK_CASE,
    TK_DEFAULT, TK_NEW, TK_TYPEOF, TK_NULL, TK_TRUE, TK_FALSE,
    TK_UNDEFINED, TK_THIS, TK_TRY, TK_CATCH, TK_FINALLY, TK_THROW,
    TK_IN, TK_OF, TK_CLASS, TK_EXTENDS, TK_DELETE, TK_VOID, TK_INSTANCEOF,
    /* punctuation / operators */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_SEMI, TK_COMMA, TK_DOT, TK_DOTDOTDOT,
    TK_QMARK, TK_COLON, TK_ARROW,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_STARSTAR,
    TK_EQ, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
    TK_STARSTAREQ, TK_EQEQ, TK_EQEQEQ, TK_NE, TK_NEEQ, TK_LT, TK_LE,
    TK_GT, TK_GE,    TK_ANDAND, TK_OROR, TK_NULLISH, TK_QMARKDOT,
    TK_ANDANDEQ, TK_OROREQ, TK_NULLISHEQ,
    TK_AND, TK_OR, TK_XOR, TK_NOT, TK_TILDE, TK_SHL, TK_SHR, TK_SHRU,
    TK_ANDEQ, TK_OREQ, TK_XOREQ, TK_SHLEQ, TK_SHREQ, TK_SHRUEQ,
    TK_PLUSPLUS, TK_MINUSMINUS,
    TK_TEMPLATE,
    TK_COUNT
} CmcTokKind;

typedef struct {
    CmcTokKind kind;
    CmcSlice   text;       /* zero-copy slice into source               */
    double     num;        /* numeric value (TK_NUM)                    */
    int64_t    bigint;     /* bigint value (TK_BIGINT)                  */
    int        line, col;
} CmcToken;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    int         line, col;
    CmcArena   *arena;
} CmcLexer;

void cmc_lex_init(CmcLexer *lx, const char *src, size_t len, CmcArena *arena);
CmcToken cmc_lex_next(CmcLexer *lx);

/* ------------------------------------------------------------------ */
/* AST nodes.                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    N_EOF,
    N_PROGRAM,
    N_NUM, N_BIGINT, N_STR, N_BOOL, N_NULL, N_UNDEF, N_IDENT,
    N_MEMBER, N_CALL, N_BIN, N_UNARY, N_ASSIGN, N_UPDATE, N_COND,
    N_ARR, N_OBJ, N_TEMPLATE, N_ARROW, N_SEQ, N_NULLISH, N_QMARKDOT,
    N_VAR, N_FUNC, N_IF, N_WHILE, N_DOWHILE, N_FOR, N_FOROF, N_RETURN,
    N_BREAK, N_CONTINUE, N_BLOCK, N_SWITCH, N_EXPRSTMT, N_THROW, N_TRY,
    N_FORIN,
    N_DESTRUCT_OBJ, N_DESTRUCT_ARR, N_THIS,
} CmcNodeKind;

typedef struct CmcNode CmcNode;
typedef struct CmcProp CmcProp;

struct CmcNode {
    CmcNodeKind kind;
    int line, col;
    /* literal */
    double      num;
    int64_t     bigint;
    const char *str;         /* NUL-terminated arena copy (strings/idents) */
    int         boolval;
    /* identifiers / binding */
    const char *name;
    /* member / call */
    CmcNode    *obj, *prop;  /* obj.prop  |  obj[prop]  |  call(obj, args) */
    CmcNode   **args;
    int         nargs;
    int         computed;    /* a[expr] */
    int         optional;    /* a?.b / a?.()  */
    /* binary / unary / assign / update */
    CmcNode    *lhs, *rhs, *third;
    int         op;          /* operator enum (reuse token kind ranges) */
    /* object literal */
    CmcProp    *props;
    int         nprops;
    /* array literal */
    CmcNode   **elems;
    int         nelems;
    /* template literal */
    CmcNode   **quasis;      /* string constants for the raw segments */
    CmcNode   **exprs;
    int         nquasis, nexprs;
    /* arrow function */
    CmcNode   **params;
    int         nparams;
    CmcNode    *body;        /* expression or block */
    int         is_async, is_expr_body;
    /* function declaration */
    const char *fname;
    /* statements */
    CmcNode    *init, *test, *update, *stmt;       /* for loops */
    CmcNode    *left;                              /* for-of binding */
    CmcNode   **stmts;
    int         nstmts;
    CmcNode    *cons, *alt;                        /* if / ternary */
    CmcNode   **cases;
    CmcNode   **case_tests;
    int         ncases;
    /* destructuring */
    CmcNode   **dpat_keys;   /* property keys (ident) */
    CmcNode   **dpat_vals;   /* binding identifiers */
    int         ndpat;
};

struct CmcProp {
    const char *key;
    CmcNode    *value;
    int         computed;
    int         spread;   /* 1 = spread `...src` — value is the spread source */
};

/* ------------------------------------------------------------------ */
/* IR: typed, lowered, escaped.                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    CT_INT, CT_INT64, CT_DOUBLE, CT_STR, CT_BOOL, CT_OBJ, CT_ARR,
    CT_NULL, CT_UNDEF, CT_VOID, CT_UNKNOWN, CT_MOCHI_VAL
} CmcType;

const char *cmc_type_name(CmcType t);          /* C type spelling        */
int         cmc_is_numeric(CmcType t);

/* Shape registry: object literals -> concrete C structs. */
const char *cmc_register_shape(CmcArena *arena, CmcNode *obj);
const char *cmc_struct_field_type(const char *struct_name, const char *field);
const char *cmc_struct_field_type_n(const char *struct_name, int idx);
CmcType   cmc_struct_field_kind(const char *struct_name, const char *field);
void      cmc_shape_reorder(const char *struct_name, const char **order, int n);
const char *cmc_struct_field_name_n(const char *struct_name, int idx);
int         cmc_shape_count(void);
const char *cmc_shape_name(int i);
int         cmc_shape_field_count(int i);
const char *cmc_shape_field_name(int i, int j);
const char *cmc_shape_field_type(int i, int j);
void        cmc_shape_add_field(const char *struct_name, const char *field, CmcType t);

/* A lowered statement/expression carrying a C type + escape info. */
typedef enum {
    IR_EXPR, IR_STMT
} CmcIRKind;

/* Emitted C text chunk. */
typedef struct {
    char *text;          /* arena-owned C code string */
    CmcType type;
    int    escapes;      /* heap/GC allocated, must be freed/GC'd */
    int    is_lvalue;
} CmcIR;

/* Compilation context. */
typedef struct {
    CmcArena *arena;
    int       with_mochiberry;   /* -DCMC_WITH_MOCHIBERRY */
    int       opt_o2;
    int       err_count;
    char      errbuf[512];
} CmcCtx;

/* ------------------- IR: symbol table + analysis ------------------- */
typedef enum {
    SYM_LOCAL, SYM_PARAM, SYM_GLOBAL, SYM_FUNC
} CmcSymKind;

typedef struct CmcSym {
    const char *name;      /* JS identifier            */
    const char *cname;     /* C identifier (sanitized) */
    CmcType     type;
    CmcSymKind  kind;
    int         escapes;   /* heap-allocated, may outlive scope */
    int         heap;      /* allocated via CMC_ALLOC      */
    const char *struct_name; /* concrete struct for CT_OBJ   */
    const char *has_name;    /* bool guard `has_X` for nullish/default params */
    CmcNode    *func_node;   /* the N_ARROW/N_FUNC this sym aliases (for calls) */
} CmcSym;

typedef struct {
    CmcSym   *syms;
    int       n;
    int       cap;
} CmcScope;

/* One frame of the scope stack (chunk-linked, lives in the bump arena). */
typedef struct CmcScopeNode {
    CmcScope scope;
    struct CmcScopeNode *up;
} CmcScopeNode;

typedef struct {
    CmcScopeNode *stack;   /* chunk-linked scope stack */
} CmcEnv;

void    cmc_env_init(CmcEnv *env, CmcArena *arena);
void    cmc_env_push(CmcEnv *env, CmcArena *arena);
void    cmc_env_pop(CmcEnv *env);
CmcSym *cmc_env_decl(CmcEnv *env, const char *name, CmcType type, CmcSymKind kind);
CmcSym *cmc_env_lookup(CmcEnv *env, const char *name);
const char *cmc_cname(CmcEnv *env, const char *name);

/* Stage 3a: type inference for an expression. */
CmcType   cmc_type_of(CmcCtx *ctx, CmcEnv *env, CmcNode *node);
int       cmc_is_numeric(CmcType t);
const char *cmc_type_name(CmcType t);

/* Stage 3b: escape analysis. Does a local variable's value leave its
 * function (returned, passed to a call, stored into a heap container)? */
int       cmc_var_escapes(CmcNode *fn, const char *name);

/* Frontend: lex + parse -> program node. */
CmcNode *cmc_parse(CmcCtx *ctx, const char *src, size_t len);

/* Stage 4: lower + emit -> C source string (arena-owned). */
char *cmc_emit(CmcCtx *ctx, CmcNode *program);

#endif /* CMOCHI_H */
