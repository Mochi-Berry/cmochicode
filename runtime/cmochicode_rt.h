/*
 * cmochicode_rt.h — micro runtime for cmochicode-generated C (<150 lines).
 *
 * The ONLY things a generated program borrows from a runtime are:
 *   - a memory allocator (CMC_ALLOC / CMC_FREE): host GC/arena when
 *     -DCMC_WITH_MOCHIBERRY, otherwise plain malloc/free.
 *   - small tagged-value + dynamic-array helpers for heterogeneous data.
 * Everything else is lowered to plain C99 (puts/printf, structs, math).
 *
 * LICENSE: MIT
 */
#ifndef CMOCHICODE_RT_H
#define CMOCHICODE_RT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifdef CMC_WITH_MOCHIBERRY
#include "mbjs_alloc.h"
#define CMC_ALLOC(sz) mbjs_gc_alloc((sz))
#define CMC_FREE(p)   ((void)(p))      /* owned by host GC / arena */
#else
#define CMC_ALLOC(sz) malloc((sz))
#define CMC_FREE(p)   free((p))
#endif

/* ---------- tagged value + dynamic array ---------- */
typedef struct { unsigned char t; double n; const char *s; void *p; } mochi_val;
typedef struct { int len, cap; mochi_val *items; } mochi_array;
static inline mochi_val mochi_num(double v)      { mochi_val x; x.t=0; x.n=v; x.s=0; x.p=0; return x; }
static inline mochi_val mochi_str(const char *v) { mochi_val x; x.t=1; x.n=0; x.s=v; x.p=0; return x; }
static inline mochi_val mochi_bool(int b)        { mochi_val x; x.t=2; x.n=b; x.s=0; x.p=0; return x; }
static inline mochi_val mochi_null(void)         { mochi_val x; x.t=3; x.n=0; x.s=0; x.p=0; return x; }
static inline mochi_val mochi_arr_box(mochi_array *a){ mochi_val x; x.t=4; x.n=0; x.s=0; x.p=a; return x; }
static inline mochi_val mochi_obj_box(void *o)   { mochi_val x; x.t=5; x.n=0; x.s=0; x.p=o; return x; }
static inline double mochi_val_num(mochi_val v)  { return v.t==1 ? atof(v.s) : v.n; }
static inline const char *mochi_val_str(mochi_val v){
    if (v.t==1) return v.s;
    if (v.t==0) { static char b[4][32]; static int bi=0; bi=(bi+1)&3; snprintf(b[bi],32,"%g",v.n); return b[bi]; }
    if (v.t==2) return v.n ? "true" : "false";
    if (v.t==3) return "null";
    return "";
}
static inline int mochi_val_bool(mochi_val v)    { return v.t==2 ? (int)v.n : (v.t==3 ? 0 : (v.t==1 ? (v.s[0]!=0) : (v.n!=0))); }

/* ---------- dynamic array ops ---------- */
static inline mochi_array *mochi_array_new(void) {
    mochi_array *a = (mochi_array *)CMC_ALLOC(sizeof(mochi_array));
    a->len = 0; a->cap = 4;
    a->items = (mochi_val *)CMC_ALLOC(sizeof(mochi_val) * 4);
    return a;
}
static inline void mochi_array_push(mochi_array *a, mochi_val v) {
    if (a->len == a->cap) {
        int nc = a->cap * 2;
        mochi_val *ni = (mochi_val *)CMC_ALLOC(sizeof(mochi_val) * nc);
        if (a->len) memcpy(ni, a->items, sizeof(mochi_val) * a->len);
        a->items = ni; a->cap = nc;
    }
    a->items[a->len++] = v;
}
static inline mochi_val mochi_array_get(mochi_array *a, int i) {
    return (i >= 0 && i < a->len) ? a->items[i] : mochi_null();
}
static inline mochi_val mochi_array_pop(mochi_array *a) {
    return a->len ? a->items[--a->len] : mochi_null();
}
static inline void mochi_array_set(mochi_array *a, int i, mochi_val v) {
    if (i >= 0 && i < a->len) a->items[i] = v;
}
static inline const char *mochi_array_join(mochi_array *a, const char *sep) {
    size_t need = 1, sl = strlen(sep);
    for (int i = 0; i < a->len; i++) need += (a->items[i].t == 1 ? strlen(a->items[i].s) : 32) + sl;
    char *out = (char *)CMC_ALLOC(need); out[0] = 0; size_t o = 0;
    for (int i = 0; i < a->len; i++) {
        if (i) { memcpy(out + o, sep, sl); o += sl; }
        if (a->items[i].t == 1) { size_t l = strlen(a->items[i].s); memcpy(out + o, a->items[i].s, l); o += l; }
        else o += (size_t)snprintf(out + o, need - o, "%g", a->items[i].n);
    }
    out[o] = 0; return out;
}
/* Node-style inspect: `[ 10, 20, 30 ]` with 'single' quotes for strings. */
static inline const char *mochi_array_inspect(mochi_array *a) {
    static char b[4][4096]; static int bi = 0; bi = (bi + 1) & 3;
    char *p = b[bi]; int off = 0;
    off += snprintf(p + off, 4096 - off, "[ ");
    for (int i = 0; i < a->len; i++) {
        mochi_val v = a->items[i];
        if (i) off += snprintf(p + off, 4096 - off, ", ");
        if (v.t == 1) off += snprintf(p + off, 4096 - off, "'%s'", v.s);
        else if (v.t == 0) off += snprintf(p + off, 4096 - off, "%g", v.n);
        else if (v.t == 2) off += snprintf(p + off, 4096 - off, "%s", v.n ? "true" : "false");
        else if (v.t == 3) off += snprintf(p + off, 4096 - off, "null");
        else off += snprintf(p + off, 4096 - off, "...");
    }
    off += snprintf(p + off, 4096 - off, " ]");
    return p;
}

/* ---------- string helpers (only when the program uses them) ---------- */
static inline const char *mochi_concat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *o = (char *)CMC_ALLOC(la + lb + 1);
    memcpy(o, a, la); memcpy(o + la, b, lb + 1); return o;
}
static inline const char *mochi_num_to_str(double v) {
    char buf[64]; int l = snprintf(buf, 64, "%g", v);
    char *o = (char *)CMC_ALLOC(l + 1); memcpy(o, buf, l + 1); return o;
}
static inline const char *mochi_toupper(const char *s) {
    size_t l = strlen(s); char *o = (char *)CMC_ALLOC(l + 1);
    for (size_t i = 0; i < l; i++) { char c = s[i]; o[i] = (c >= 'a' && c <= 'z') ? c - 32 : c; }
    o[l] = 0; return o;
}
static inline const char *mochi_tolower(const char *s) {
    size_t l = strlen(s); char *o = (char *)CMC_ALLOC(l + 1);
    for (size_t i = 0; i < l; i++) { char c = s[i]; o[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c; }
    o[l] = 0; return o;
}
static inline const char *mochi_char_at(const char *s, int i) {
    static char b[2];
    if (i < 0 || (size_t)i >= strlen(s)) return "";
    b[0] = s[i]; b[1] = 0; return b;
}
static inline const char *mochi_substr(const char *s, int st, int len) {
    size_t l = strlen(s);
    if (st < 0 || st >= (int)l) return "";
    if (len < 0 || len > (int)l - st) len = (int)l - st;
    char *o = (char *)CMC_ALLOC(len + 1); memcpy(o, s + st, len); o[len] = 0; return o;
}
static inline int mochi_index_of(const char *s, const char *needle, int from) {
    if (from < 0) from = 0; if ((size_t)from > strlen(s)) return -1;
    const char *p = strstr(s + from, needle);
    return p ? (int)(p - s) : -1;
}
static inline const char *mochi_trim(const char *s) {
    size_t l = strlen(s), a = 0, b = l;
    while (a < l && (s[a] == ' ' || s[a] == '\n' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\n' || s[b-1] == '\t' || s[b-1] == '\r')) b--;
    char *o = (char *)CMC_ALLOC(b - a + 1); memcpy(o, s + a, b - a); o[b - a] = 0; return o;
}
static inline int mochi_includes(const char *s, const char *sub) { return strstr(s, sub) != NULL; }
static inline int mochi_starts_with(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }
static inline int mochi_ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}
static inline const char *mochi_slice(const char *s, int st, int en) {
    size_t l = strlen(s);
    if (st < 0) st = (int)l + st; if (en < 0) en = (int)l + en;
    if (st < 0) st = 0; if (en > (int)l) en = (int)l; if (en < st) en = st;
    return mochi_substr(s, st, en - st);
}
static inline const char *mochi_repeat(const char *s, int n) {
    if (n <= 0) return "";
    size_t l = strlen(s);
    char *o = (char *)CMC_ALLOC(l * n + 1);
    for (int i = 0; i < n; i++) memcpy(o + i * l, s, l);
    o[l * n] = 0; return o;
}
static inline const char *mochi_replace(const char *s, const char *from, const char *to) {
    const char *p = strstr(s, from);
    if (!p) return s;
    size_t lf = strlen(from), lt = strlen(to);
    size_t before = (size_t)(p - s), l = strlen(s);
    char *o = (char *)CMC_ALLOC(l - lf + lt + 1);
    memcpy(o, s, before); memcpy(o + before, to, lt);
    memcpy(o + before + lt, p + lf, l - before - lf + 1);
    return o;
}
static inline mochi_array *mochi_split(const char *s, const char *sep) {
    mochi_array *a = mochi_array_new();
    size_t l = strlen(s), sl = strlen(sep);
    if (sl == 0) { mochi_array_push(a, mochi_str(s)); return a; }
    const char *start = s;
    while (1) {
        const char *p = strstr(start, sep);
        if (!p) { mochi_array_push(a, mochi_str(start)); break; }
        size_t n = (size_t)(p - start);
        char *t = (char *)CMC_ALLOC(n + 1); memcpy(t, start, n); t[n] = 0;
        mochi_array_push(a, mochi_str(t));
        start = p + sl;
    }
    return a;
}
static inline double mochi_rand01(void) { return (double)rand() / (double)RAND_MAX; }

#endif /* CMOCHICODE_RT_H */
