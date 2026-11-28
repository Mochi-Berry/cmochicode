/*
 * main.c — cmochi CLI driver for the C99 native pipeline.
 *
 *   cmochi in.js [-o out.c]
 *
 * Lex + parse + IR + emit in one process, all C99. The output is a
 * standalone C file (compiles with `gcc -O2 -s out.c -o app`), or a
 * MochiBerry-native module when built with -DCMC_WITH_MOCHIBERRY.
 */
#include "cmochi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

/* Create the parent directory of `path` (single level, best effort). */
static void ensure_dir(const char *path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof buf) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i]; buf[i] = 0;
            if (buf[0]) MKDIR(buf);
            buf[i] = c;
        }
    }
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cmochi: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1);
    if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "cmochi: short read on %s\n", path); exit(1);
    }
    b[sz] = 0;
    fclose(f);
    *len = (size_t)sz;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cmochi in.js [-o out.c]\n");
        return 1;
    }
    const char *in = argv[1];
    const char *out = "out.c";
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
    }
    ensure_dir(out);

    size_t len;
    char *src = read_file(in, &len);

    CmcArena arena;
    cmc_arena_init(&arena);
    CmcCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.arena = &arena;

    CmcNode *prog = cmc_parse(&ctx, src, len);
    if (getenv("CMC_DEBUG")) {
        fprintf(stderr, "nstmts=%d\n", prog->nstmts);
        for (int i = 0; i < prog->nstmts; i++)
            fprintf(stderr, "  [%d] kind=%d%s\n", i, prog->stmts[i]->kind,
                    prog->stmts[i]->kind == N_FUNC ? " (func)" : "");
    }
    if (ctx.err_count) {
        fprintf(stderr, "%s\n", ctx.errbuf);
        return 1;
    }
    char *code = cmc_emit(&ctx, prog);

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cmochi: cannot write %s\n", out); return 1; }
    fputs(code, f);
    fclose(f);
    fprintf(stderr, "cmochi: %s -> %s\n", in, out);
    return 0;
}
