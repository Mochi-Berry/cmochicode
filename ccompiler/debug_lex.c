/* debug_lex.c — dump tokens from a file (dev only). */
#include "cmochi.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc(sz + 1);
    fread(b, 1, sz, f); b[sz] = 0; fclose(f);
    CmcArena a; cmc_arena_init(&a);
    CmcLexer lx; cmc_lex_init(&lx, b, sz, &a);
    for (int i = 0; i < 12; i++) {
        CmcToken t = cmc_lex_next(&lx);
        fprintf(stderr, "tok kind=%d text='%.*s' line=%d col=%d\n",
                t.kind, (int)t.text.len, t.text.ptr, t.line, t.col);
        if (t.kind == TK_EOF) break;
    }
    return 0;
}
