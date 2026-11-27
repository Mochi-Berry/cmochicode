/*
 * cmochicode_module.c — the cmochicode compiler as a MochiBerry native module.
 *
 *   const cm = await import('cmochicode.dll');
 *   const c  = cm.compile('var x = 1; console.log(x);');   // -> C source
 *   const ok = cm.compileFile('examples/hello.js', 'out.c'); // -> true
 *
 * Lets MochiBerry transpile JS -> C at runtime, and lets the compiler be
 * driven from JS instead of only the standalone cmochi.exe CLI. The
 * compiler TUs (lex/parse/ir/emit) are linked in; only the *generated* C
 * borrows the MochiBerry GC/arena (via -DCMC_WITH_MOCHIBERRY), the
 * compiler itself is standalone.
 *
 * ABI contract (see mochiberry-native-modules skill):
 *   - export name is exactly `mbjs_init_module`
 *   - import specifier must end in `.dll`
 *   - never import mbjs.exe — resolve engine symbols from the host image
 *   - a C function that throws MUST return MBJS_EXCEPTION
 */
#include "mbjs_runtime.h"
#include "cmochi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MBJSRuntime rt;

static char *js_cstr_len(MBJSContext *ctx, MBJSValueConst v, size_t *out_len)
{
    size_t len = 0;
    const char *p = rt.ToCStringLen(ctx, &len, v);
    char *out;
    if (!p)
        return NULL;
    out = (char *)malloc(len + 1);
    if (!out) {
        rt.FreeCString(ctx, p);
        return NULL;
    }
    memcpy(out, p, len);
    out[len] = 0;
    rt.FreeCString(ctx, p);
    if (out_len)
        *out_len = len;
    return out;
}

static MBJSValue js_str(MBJSContext *ctx, const char *s, size_t len)
{
    if (!s)
        return rt.NewStringLen(ctx, "", 0);
    return rt.NewStringLen(ctx, s, len);
}

static MBJSValue js_error(MBJSContext *ctx, const char *what)
{
    rt.ThrowTypeError(ctx, "cmochicode: %s", what);
    return MBJS_EXCEPTION; /* contract: a throwing C func returns MBJS_EXCEPTION */
}

/* compile(source) -> C source string */
static MBJSValue cmc_compile(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    size_t srclen = 0;
    char *src;
    CmcArena arena;
    CmcCtx cctx;
    CmcNode *prog;
    char *code;
    MBJSValue out;
    (void)this_val;
    if (argc < 1)
        return js_error(ctx, "compile needs a source string");
    src = js_cstr_len(ctx, argv[0], &srclen);
    if (!src)
        return js_error(ctx, "source conversion failed");

    cmc_arena_init(&arena);
    memset(&cctx, 0, sizeof cctx);
    cctx.arena = &arena;

    prog = cmc_parse(&cctx, src, srclen);
    if (cctx.err_count) {
        free(src);
        cmc_arena_free(&arena);
        return js_error(ctx, cctx.errbuf);
    }
    code = cmc_emit(&cctx, prog);
    out = js_str(ctx, code, strlen(code));
    free(code);
    free(src);
    cmc_arena_free(&arena);
    return out;
}

/* compileFile(inPath, outPath?) -> C source string, or true if outPath given */
static MBJSValue cmc_compile_file(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    char *path;
    FILE *f;
    long size;
    char *buf;
    size_t got;
    MBJSValue out;
    (void)this_val;
    if (argc < 1)
        return js_error(ctx, "compileFile needs a path");
    path = js_cstr_len(ctx, argv[0], NULL);
    if (!path)
        return js_error(ctx, "compileFile path conversion failed");
    f = fopen(path, "rb");
    if (!f) {
        free(path);
        return js_error(ctx, "compileFile could not open file");
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        free(path);
        return js_error(ctx, "compileFile could not stat file");
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        free(path);
        return js_error(ctx, "compileFile out of memory");
    }
    got = fread(buf, 1, (size_t)size, f);
    buf[got] = 0;
    fclose(f);

    {
        CmcArena arena;
        CmcCtx cctx;
        CmcNode *prog;
        char *code;
        cmc_arena_init(&arena);
        memset(&cctx, 0, sizeof cctx);
        cctx.arena = &arena;
        prog = cmc_parse(&cctx, buf, got);
        if (cctx.err_count) {
            free(buf);
            free(path);
            cmc_arena_free(&arena);
            return js_error(ctx, cctx.errbuf);
        }
        code = cmc_emit(&cctx, prog);
        out = js_str(ctx, code, strlen(code));
        free(code);
        cmc_arena_free(&arena);
    }
    free(buf);
    free(path);
    return out;
}

static MBJSValue cmc_version(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return js_str(ctx, "0.1.0", 5);
}

/* Export table — file scope so cmc_mod_init can bind the VALUES with
 * SetModuleExportList (AddModuleExportList only registers the NAMES). */
static const MBJSCFunctionListEntry cmc_tab[] = {
    MBJS_CFUNC_DEF("compile", 1, cmc_compile),
    MBJS_CFUNC_DEF("compileFile", 1, cmc_compile_file),
    MBJS_CFUNC_DEF("version", 0, cmc_version),
};
#define CMC_TAB_N 3

/* mod_init callback — binds the function values into the module namespace.
 * MUST be non-NULL and return 0 (loader contract). */
static int cmc_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    return rt.SetModuleExportList(ctx, m, cmc_tab, CMC_TAB_N);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                   const char *name)
{
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;
    if (missing)
        return NULL;
    m = rt.NewCModule(ctx, name ? name : "cmochicode", cmc_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, cmc_tab, CMC_TAB_N) != 0)
        return NULL;
    return m;
}
