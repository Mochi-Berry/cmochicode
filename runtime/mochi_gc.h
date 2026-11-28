/*
 * mochi_gc.h — wiring stub to borrow MochiBerry's GC allocator.
 *
 * This is NOT part of the default runtime. It shows the contract the
 * generated program expects when built with -DMOCHI_USE_GC:
 *
 *     void *mochi_gc_alloc(size_t size);          // must be provided
 *     void  mochi_gc_root(void **slot);           // optional
 *     void  mochi_gc_unroot(void **slot);         // optional
 *
 * To link against a real GC (e.g. the MochiBerry engine's), provide an
 * implementation of mochi_gc_alloc that wraps the engine's allocation
 * function, then compile the generated .c with -DMOCHI_USE_GC.
 *
 * The example below is a tracing-collector sketch with a tiny bump
 * allocator and no collection — replace mochi_gc_collect() with the host
 * collector when integrating. It exists so the contract compiles and the
 * generated program runs with GC semantics (never frees manually).
 */
#ifndef MOCHI_GC_H
#define MOCHI_GC_H

#include <stddef.h>
#include <stdlib.h>

static void *mochi_gc_pool = NULL;
static size_t mochi_gc_used = 0;
static size_t mochi_gc_cap  = 0;

void *mochi_gc_alloc(size_t size) {
    if (size == 0) size = 1;
    if (mochi_gc_used + size > mochi_gc_cap) {
        size_t ncap = mochi_gc_cap ? mochi_gc_cap * 2 : 65536;
        while (ncap < mochi_gc_used + size) ncap *= 2;
        void *np = realloc(mochi_gc_pool, ncap);
        if (!np) return NULL;
        mochi_gc_pool = np;
        mochi_gc_cap  = ncap;
    }
    void *p = (char *)mochi_gc_pool + mochi_gc_used;
    mochi_gc_used += size;
    return p;
}

/* Rooting hooks are no-ops in this bump-pool sketch; a real tracing GC
 * would register/unregister the pointer here. */
void mochi_gc_root(void **slot)   { (void)slot; }
void mochi_gc_unroot(void **slot) { (void)slot; }

#endif /* MOCHI_GC_H */
