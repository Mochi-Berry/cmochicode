/*
 * cmochicode — MochiBerry native-first module (JS -> C99 transpiler).
 *
 * ABI contract (see mochiberry/module-loader-contract.md):
 *   - exports the single entry point mbjs_init_module(MBJSContext*, const char*)
 *   - registers C functions via NewCModule / AddModuleExportList / SetModuleExportList
 *     (AddModuleExportList names only; VALUES are bound in mod_init)
 *   - C functions that throw MUST return MBJS_EXCEPTION (never MBJS_UNDEFINED)
 *
 * Runtime symbols (MBJS_*) are resolved through mbjs_runtime_resolve() from
 * <root>/native/mbjs_runtime.h at load time; do NOT link against mbjs.exe.
 *
 * Exported runtime symbols (measured): compile, compileFile, version
 */
#ifndef MB_CMOCHICODE_H
#define MB_CMOCHICODE_H

#include <stdint.h>

typedef struct MBJSContext MBJSContext;
typedef struct MBJSModuleDef MBJSModuleDef;

/* the only export the engine loader looks for */
MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name);

#endif /* MB_CMOCHICODE_H */