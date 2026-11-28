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
#include <setjmp.h>

#ifdef CMC_WITH_MOCHIBERRY
#include "mbjs_alloc.h"
#define CMC_ALLOC(sz) mbjs_gc_alloc((sz))
#define CMC_FREE(p)   ((void)(p))      /* owned by host GC / arena */
#else
#define CMC_ALLOC(sz) malloc((sz))
#define CMC_FREE(p)   free((p))
#endif

/* ---------- exceptions: setjmp/longjmp unwinding ---------- */
/* A throw longjmps to the innermost live try frame; the caught message is
 * copied into a static buffer so it survives the unwind (the throwing frame
 * is gone). Uncaught throws print and exit. */
typedef jmp_buf MochiJmpBuf;   /* jmp_buf is an array type; keep it as one */
#define MOCHI_TRY_MAX 32
static MochiJmpBuf *mochi_try_stack[MOCHI_TRY_MAX];
static int mochi_try_depth = 0;
static char mochi_thrown_buf[512];
static const char *mochi_last_thrown = 0;
/* Register/unregister the try frame. The setjmp itself MUST live in the same
 * function as the try body (a longjmp into a returned frame is UB), so the
 * generated code calls setjmp inline and only registers the buffer here. */
static void mochi_try_push(MochiJmpBuf *jb) {
    if (mochi_try_depth < MOCHI_TRY_MAX) mochi_try_stack[mochi_try_depth++] = jb;
}
static void mochi_try_pop(void) { if (mochi_try_depth > 0) mochi_try_depth--; }
static void mochi_throw(const char *msg) {
    if (!msg) msg = "";
    strncpy(mochi_thrown_buf, msg, sizeof(mochi_thrown_buf) - 1);
    mochi_thrown_buf[sizeof(mochi_thrown_buf) - 1] = 0;
    mochi_last_thrown = mochi_thrown_buf;
    if (mochi_try_depth > 0) {
        MochiJmpBuf *target = mochi_try_stack[mochi_try_depth - 1];
        longjmp(*target, 1);
    }
    fprintf(stderr, "Exception: %s\n", mochi_thrown_buf);
    exit(1);
}

/* ---------- tagged value + dynamic array ---------- */
typedef struct { unsigned char t; double n; const char *s; void *p; } mochi_val;
static mochi_val mochi_undef(void);
/* ---------- dynamic string-key map (overflow storage) ---------- */
/* A shape object may need to hold keys that were only ever written through a
 * runtime expression (obj[k] = v): the static struct fields are checked
 * first, then this lazily-allocated map keeps the rest. */
typedef struct MochiMapEntry { const char *key; mochi_val val; struct MochiMapEntry *next; } MochiMapEntry;
typedef struct { MochiMapEntry *head; MochiMapEntry *tail; size_t len; } mochi_map;
static const char *mochi_map_strdup(const char *s) {
    size_t n = strlen(s) + 1; char *d = (char *)CMC_ALLOC(n); memcpy(d, s, n); return d;
}
static mochi_map *mochi_map_new(void) {
    mochi_map *m = (mochi_map *)CMC_ALLOC(sizeof(mochi_map));
    m->head = NULL; m->tail = NULL; m->len = 0; return m;
}
static mochi_val mochi_map_get(mochi_map *m, const char *k) {
    for (MochiMapEntry *e = m->head; e; e = e->next)
        if (strcmp(e->key, k) == 0) return e->val;
    return mochi_undef();
}
static int mochi_map_has(mochi_map *m, const char *k) {
    for (MochiMapEntry *e = m->head; e; e = e->next)
        if (strcmp(e->key, k) == 0) return 1;
    return 0;
}
static void mochi_map_set(mochi_map *m, const char *k, mochi_val v) {
    if (!k) return;
    for (MochiMapEntry *e = m->head; e; e = e->next)
        if (strcmp(e->key, k) == 0) { e->val = v; return; }
    MochiMapEntry *e = (MochiMapEntry *)CMC_ALLOC(sizeof(MochiMapEntry));
    e->key = mochi_map_strdup(k); e->val = v; e->next = NULL;
    if (m->tail) m->tail->next = e; else m->head = e;
    m->tail = e; m->len++;
}
static void mochi_map_del(mochi_map *m, const char *k) {
    MochiMapEntry *prev = NULL, *e = m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) {
            if (prev) prev->next = e->next; else m->head = e->next;
            if (m->tail == e) m->tail = prev;
            m->len--; return;
        }
        prev = e; e = e->next;
    }
}
static size_t mochi_map_len(mochi_map *m) { return m ? m->len : 0; }


typedef struct { int len, cap; mochi_val *items; } mochi_array;
static inline mochi_val mochi_num(double v)      { mochi_val x; x.t=0; x.n=v; x.s=0; x.p=0; return x; }
static inline mochi_val mochi_str(const char *v) { mochi_val x; x.t=1; x.n=0; x.s=v; x.p=0; return x; }
static inline mochi_val mochi_bool(int b)        { mochi_val x; x.t=2; x.n=b; x.s=0; x.p=0; return x; }
static inline mochi_val mochi_null(void)         { mochi_val x; x.t=3; x.n=0; x.s=0; x.p=0; return x; }
static inline mochi_val mochi_undef(void)        { mochi_val x; x.t=6; x.n=0; x.s=0; x.p=0; return x; }
static inline mochi_val mochi_arr_box(mochi_array *a){ mochi_val x; x.t=4; x.n=0; x.s=0; x.p=a; return x; }
static inline mochi_val mochi_obj_box(void *o)   { mochi_val x; x.t=5; x.n=0; x.s=0; x.p=o; return x; }
static inline double mochi_val_num(mochi_val v)  { return v.t==1 ? atof(v.s) : v.n; }
/* JS-style value repr: strings get quotes, numbers/bools/null as-is —
 * matches how node prints an object value. */
/* JSON rendering of a dynamic value: strings are quoted and escaped,
 * numbers/bools/null bare — what JSON.stringify emits. */
static inline const char *mochi_val_str(mochi_val v);
static inline const char *mochi_val_json(mochi_val v){
    if (v.t==1) {
        static char sb[4][512]; static int si=0; si=(si+1)&3;
        int n = snprintf(sb[si], 512, "\"");
        for (const char *p = v.s; *p && n < 480; p++) {
            if (*p == '\"' || *p == '\\') n += snprintf(sb[si]+n, 512-n, "\\%c", *p);
            else if (*p == '\n') n += snprintf(sb[si]+n, 512-n, "\\n");
            else n += snprintf(sb[si]+n, 512-n, "%c", *p);
        }
        snprintf(sb[si]+n, 512-n, "\"");
        return sb[si];
    }
    return mochi_val_str(v);
}
static inline const char *mochi_val_str(mochi_val v);
static inline const char *mochi_val_str_js(mochi_val v){
    if (v.t==1) { static char sb[4][256]; static int si=0; si=(si+1)&3;
        snprintf(sb[si],256,"'%s'", v.s); return sb[si]; }
    return mochi_val_str(v);
}
static inline const char *mochi_val_str(mochi_val v){
    if (v.t==1) return v.s;
    if (v.t==0) { static char b[4][32]; static int bi=0; bi=(bi+1)&3; snprintf(b[bi],32,"%g",v.n); return b[bi]; }
    if (v.t==2) return v.n ? "true" : "false";
    if (v.t==3) return "null";
    if (v.t==6) return "undefined";
    return "";
}
static inline int mochi_val_bool(mochi_val v)    { return v.t==2 ? (int)v.n : (v.t==3 || v.t==6 ? 0 : (v.t==1 ? (v.s[0]!=0) : (v.n!=0))); }

/* ---------- dynamic array ops ---------- */
static inline mochi_array *mochi_array_new(void) {
    mochi_array *a = (mochi_array *)CMC_ALLOC(sizeof(mochi_array));
    a->len = 0; a->cap = 4;
    a->items = (mochi_val *)CMC_ALLOC(sizeof(mochi_val) * 4);
    return a;
}
/* unbox a possibly-array mochi_val: NULL-safe empty array when not an array */
static mochi_array *mochi_arr_or_empty(mochi_val v) {
    static mochi_array *empty = NULL;
    if (v.t == 4 && v.p) return (mochi_array *)v.p;
    if (!empty) empty = mochi_array_new();
    return empty;
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
    return (i >= 0 && i < a->len) ? a->items[i] : mochi_undef();
}
static inline mochi_val mochi_array_pop(mochi_array *a) {
    return a->len ? a->items[--a->len] : mochi_undef();
}
static inline void mochi_array_set(mochi_array *a, int i, mochi_val v) {
    if (i >= 0 && i < a->len) a->items[i] = v;
}
static inline mochi_array *mochi_array_slice(mochi_array *a, int s, int e) {
    mochi_array *r = mochi_array_new();
    int n = a->len;
    if (s < 0) s = n + s;
    if (s < 0) s = 0;
    if (e < 0) e = n + e;
    if (e < 0) e = 0;
    if (e > n) e = n;
    for (int i = s; i < e; i++) mochi_array_push(r, a->items[i]);
    return r;
}
static inline mochi_array *mochi_array_reverse(mochi_array *a) {
    /* in place, JS-mutating semantics; returns the same array */
    for (int i = 0, j = a->len - 1; i < j; i++, j--) {
        mochi_val t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;
    }
    return a;
}
static inline mochi_val mochi_array_shift(mochi_array *a) {
    if (!a->len) return mochi_undef();
    mochi_val v = a->items[0];
    for (int i = 1; i < a->len; i++) a->items[i-1] = a->items[i];
    a->len--; return v;
}
static inline void mochi_array_unshift(mochi_array *a, mochi_val v) {
    mochi_array_push(a, mochi_null());
    for (int i = a->len - 1; i > 0; i--) a->items[i] = a->items[i-1];
    a->items[0] = v;
}
static inline int mochi_val_cmp(mochi_val a, mochi_val b) {
    int na = (a.t == 0 || a.t == 2), nb = (b.t == 0 || b.t == 2);
    if (na && nb) return mochi_val_num(a) < mochi_val_num(b) ? -1 : (mochi_val_num(a) > mochi_val_num(b) ? 1 : 0);
    if (na) return -1;                 /* numbers sort before strings (JS) */
    if (nb) return 1;
    return strcmp(mochi_val_str(a), mochi_val_str(b));
}
/* JS splice: remove `del` items at `start`, insert `ins` (may be NULL),
 * return the removed items. */
static inline mochi_array *mochi_array_splice(mochi_array *a, int start, int del, mochi_array *ins) {
    int n = a->len;
    if (start < 0) start = n + start;
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (del < 0) del = 0;
    if (del > n - start) del = n - start;
    mochi_array *rem = mochi_array_new(), *r = mochi_array_new();
    for (int i = 0; i < del; i++) mochi_array_push(rem, a->items[start + i]);
    for (int i = 0; i < start; i++) mochi_array_push(r, a->items[i]);
    if (ins) for (int i = 0; i < ins->len; i++) mochi_array_push(r, ins->items[i]);
    for (int i = start + del; i < n; i++) mochi_array_push(r, a->items[i]);
    a->items = r->items; a->len = r->len; a->cap = r->cap;
    return rem;
}
static inline mochi_array *mochi_array_concat(mochi_array *a, mochi_array *b) {
    mochi_array *r = mochi_array_new();
    for (int i = 0; i < a->len; i++) mochi_array_push(r, a->items[i]);
    if (b) for (int i = 0; i < b->len; i++) mochi_array_push(r, b->items[i]);
    return r;
}
static inline mochi_array *mochi_array_sort(mochi_array *a) {
    /* insertion sort: small arrays, stable, no qsort comparator context */
    for (int i = 1; i < a->len; i++) {
        mochi_val v = a->items[i]; int j = i - 1;
        while (j >= 0 && mochi_val_cmp(a->items[j], v) > 0) { a->items[j+1] = a->items[j]; j--; }
        a->items[j+1] = v;
    }
    return a;
}
static inline mochi_array *mochi_array_fill(mochi_array *a, mochi_val v, int s, int e) {
    int n = a->len;
    if (s < 0) s = n + s; if (s < 0) s = 0;
    if (e < 0) e = n + e; if (e > n) e = n;
    for (int i = s; i < e; i++) a->items[i] = v;
    return a;
}
static inline mochi_val mochi_array_at(mochi_array *a, int i) {
    if (i < 0) i += a->len;
    return (i >= 0 && i < a->len) ? a->items[i] : mochi_undef();
}
static inline int mochi_array_last_index_of(mochi_array *a, double v) {
    for (int i = a->len - 1; i >= 0; i--)
        if (a->items[i].t == 0 && mochi_val_num(a->items[i]) == v) return i;
    return -1;
}
static inline mochi_array *mochi_array_entries(mochi_array *a) {
    mochi_array *r = mochi_array_new();
    for (int i = 0; i < a->len; i++) {
        mochi_array *pair = mochi_array_new();
        mochi_array_push(pair, mochi_num((double)i));
        mochi_array_push(pair, a->items[i]);
        mochi_array_push(r, mochi_arr_box(pair));
    }
    return r;
}
static inline mochi_array *mochi_array_flat(mochi_array *a) {
    mochi_array *r = mochi_array_new();
    for (int i = 0; i < a->len; i++) {
        if (a->items[i].t == 4 && a->items[i].p) {
            mochi_array *in = (mochi_array *)a->items[i].p;
            for (int j = 0; j < in->len; j++) mochi_array_push(r, in->items[j]);
        } else mochi_array_push(r, a->items[i]);
    }
    return r;
}
static inline int mochi_array_index_of(mochi_array *a, double v) {
    for (int i = 0; i < a->len; i++)
        if (a->items[i].t == 0 && mochi_val_num(a->items[i]) == v) return i;
    return -1;
}
static inline int mochi_array_includes(mochi_array *a, double v) {
    return mochi_array_index_of(a, v) >= 0;
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
static inline const char *mochi_array_json(mochi_array *a) {
    static char buf[1024]; int off = 0;
    off += snprintf(buf+off, sizeof buf - off, "[");
    if (a) for (int i = 0; i < a->len && off < 900; i++) {
        if (i) off += snprintf(buf+off, sizeof buf - off, ",");
        off += snprintf(buf+off, sizeof buf - off, "%s", mochi_val_json(a->items[i]));
    }
    snprintf(buf+off, sizeof buf - off, "]");
    return buf;
}
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
/* --- dynamic receiver dispatch: a mochi_val may hold a string or an array,
 * so the method has to be chosen at runtime (JS semantics) --- */
static inline const char *mochi_slice(const char *s, int a, int b);
static inline mochi_array *mochi_array_slice(mochi_array *a, int s, int e);
static inline mochi_val mochi_array_at(mochi_array *a, int i);
static inline const char *mochi_str_at(const char *s, int i);
static inline int mochi_str_last_index_of(const char *s, const char *needle);
static inline const char *mochi_str_concat2(const char *a, const char *b);
static inline int mochi_index_of(const char *s, const char *needle, int from);
static inline int mochi_includes(const char *s, const char *sub);
static inline mochi_array *mochi_array_concat(mochi_array *a, mochi_array *b);
static inline int mochi_val_length(mochi_val v) {
    if (v.t == 4 && v.p) return ((mochi_array *)v.p)->len;
    if (v.t == 1 && v.s) return (int)strlen(v.s);
    return 0;
}
static inline mochi_val mochi_dyn_slice(mochi_val v, int a, int b) {
    if (v.t == 4 && v.p) return mochi_arr_box(mochi_array_slice((mochi_array *)v.p, a, b));
    if (v.t == 1 && v.s) return mochi_str(mochi_slice(v.s, a, b));
    return mochi_undef();
}
static inline int mochi_dyn_index_of(mochi_val v, const char *needle) {
    if (v.t == 4 && v.p) {
        mochi_array *a = (mochi_array *)v.p;
        for (int i = 0; i < a->len; i++)
            if (a->items[i].t == 1 && a->items[i].s && !strcmp(a->items[i].s, needle)) return i;
        return -1;
    }
    if (v.t == 1 && v.s) return mochi_index_of(v.s, needle, 0);
    return -1;
}
static inline int mochi_dyn_last_index_of(mochi_val v, const char *needle) {
    if (v.t == 4 && v.p) {
        mochi_array *a = (mochi_array *)v.p;
        for (int i = a->len - 1; i >= 0; i--)
            if (a->items[i].t == 1 && a->items[i].s && !strcmp(a->items[i].s, needle)) return i;
        return -1;
    }
    if (v.t == 1 && v.s) return mochi_str_last_index_of(v.s, needle);
    return -1;
}
static inline int mochi_dyn_includes(mochi_val v, const char *needle) {
    if (v.t == 4 && v.p) {
        mochi_array *a = (mochi_array *)v.p;
        for (int i = 0; i < a->len; i++)
            if (a->items[i].t == 1 && a->items[i].s && !strcmp(a->items[i].s, needle)) return 1;
        return 0;
    }
    if (v.t == 1 && v.s) return mochi_includes(v.s, needle);
    return 0;
}
static inline mochi_val mochi_dyn_at(mochi_val v, int i) {
    if (v.t == 4 && v.p) return mochi_array_at((mochi_array *)v.p, i);
    if (v.t == 1 && v.s) return mochi_str(mochi_str_at(v.s, i));
    return mochi_undef();
}
static inline mochi_val mochi_dyn_concat(mochi_val v, mochi_val other) {
    if (v.t == 4 && v.p) {
        mochi_array *r = mochi_array_concat((mochi_array *)v.p, NULL);
        mochi_array_push(r, other);
        return mochi_arr_box(r);
    }
    if (v.t == 1 && v.s) {
        const char *o = other.t == 1 && other.s ? other.s : mochi_val_str(other);
        return mochi_str(mochi_str_concat2(v.s, o));
    }
    return mochi_undef();
}
/* unbox a possibly-string mochi_val: "" when not a string */
static inline const char *mochi_str_or_empty(mochi_val v) {
    return v.t == 1 && v.s ? v.s : "";
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
static inline const char *mochi_replace_all(const char *s, const char *from, const char *to) {
    if (!*from) return s;
    size_t lf = strlen(from), lt = strlen(to), l = strlen(s);
    size_t count = 0;
    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;
    if (!count) return s;
    char *o = (char *)CMC_ALLOC(l + count * (lt > lf ? lt - lf : 0) + 1);
    char *w = o;
    const char *p = s;
    for (const char *h = strstr(p, from); h; h = strstr(p, from)) {
        memcpy(w, p, (size_t)(h - p)); w += h - p;
        memcpy(w, to, lt); w += lt;
        p = h + lf;
    }
    size_t rest = strlen(p); memcpy(w, p, rest); w[rest] = 0;
    return o;
}
static inline int mochi_str_last_index_of(const char *s, const char *needle) {
    int ls = (int)strlen(s), ln = (int)strlen(needle);
    for (int i = ls - ln; i >= 0; i--)
        if (!strncmp(s + i, needle, (size_t)ln)) return i;
    return -1;
}
static inline int mochi_char_code_at(const char *s, int i) {
    if (i < 0 || (size_t)i >= strlen(s)) return -1;
    return (int)(unsigned char)s[i];
}
static inline const char *mochi_str_concat2(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *o = (char *)CMC_ALLOC(la + lb + 1);
    memcpy(o, a, la); memcpy(o + la, b, lb); o[la + lb] = 0;
    return o;
}
static inline const char *mochi_str_at(const char *s, int i) {
    if (i < 0) i += (int)strlen(s);
    if (i < 0 || (size_t)i >= strlen(s)) return "undefined";
    static char b[2]; b[0] = s[i]; b[1] = 0; return b;
}
/* JS substring(start, end): end exclusive, negatives wrap, args swap */
static inline const char *mochi_substring(const char *s, int st, int en) {
    int l = (int)strlen(s);
    if (st < 0) st = 0; if (en < 0) en = 0;
    if (st > l) st = l; if (en > l) en = l;
    if (st > en) { int t = st; st = en; en = t; }
    return mochi_substr(s, st, en - st);
}
static inline mochi_array *mochi_split_limit(const char *s, const char *sep, int limit) {
    mochi_array *a = mochi_array_new();
    if (limit <= 0) return a;
    size_t sl = strlen(sep);
    const char *start = s;
    while (a->len < limit) {
        const char *p = sl ? strstr(start, sep) : NULL;
        if (!p) { mochi_array_push(a, mochi_str(start)); break; }
        size_t n = (size_t)(p - start);
        char *t = (char *)CMC_ALLOC(n + 1); memcpy(t, start, n); t[n] = 0;
        mochi_array_push(a, mochi_str(t));
        start = p + sl;
    }
    return a;
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
static inline const char *mochi_pad_start(const char *s, int len, const char *fill) {
    const char *f = (fill && fill[0]) ? fill : " ";
    size_t sl = strlen(s), fl = strlen(f), need = (size_t)len + 1;
    char *out = (char *)CMC_ALLOC(need);
    if (sl >= (size_t)len) { memcpy(out, s, sl + 1); return out; }
    size_t k = 0;
    while (k < (size_t)len - sl) out[k++] = f[k % fl];
    memcpy(out + k, s, sl + 1);
    return out;
}
static inline const char *mochi_pad_end(const char *s, int len, const char *fill) {
    const char *f = (fill && fill[0]) ? fill : " ";
    size_t sl = strlen(s), fl = strlen(f), need = (size_t)len + 1;
    char *out = (char *)CMC_ALLOC(need);
    memcpy(out, s, sl + 1);
    size_t k = sl;
    if ((size_t)len > sl) while (k < (size_t)len) out[k++] = f[(k - sl) % fl];
    return out;
}
static inline double mochi_rand01(void) { return (double)rand() / (double)RAND_MAX; }

#endif /* CMOCHICODE_RT_H */
