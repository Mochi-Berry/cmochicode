# cmochicode

**JavaScript → C99 source codegen for MochiBerry.** Transpile JS to readable,
self-contained C99 that compiles with `gcc`/`clang` into a tiny native binary —
no bytecode, no VM dispatch loop, no heavyweight JSValue objects (unlike
QuickJS `qjsc`). Optionally, the generated C can borrow the MochiBerry
GC/arena allocator when built with `-DCMC_WITH_MOCHIBERRY`.

Built for **Mochi-Berry** (MochiBerry). MIT license. Node >= 18.

## Architecture: 4-stage pipeline

```
ECMA Lexer/Pratt Parser  ->  Typed Micro-IR  ->  Escape & Scope Analyzer  ->  Pure C99 Emitter
(ccompiler/lex.c parse.c)   (ccompiler/ir.c)      (ir.c / emit.c)             (ccompiler/emit.c)
```

- **Frontend is C99, not TypeScript.** No `tsc`/Compiler API (tens of MB, slow
  startup, stale ES5/TS semantics). Nodes are allocated in a bump arena with
  zero-copy slices into the source buffer — no malloc/free per node.
- **Typed Micro-IR:** integral numbers stay `int64_t`/`int32_t` (not `double`),
  `100n` → `int64_t`, `bool` → `<stdbool.h>`, string literals → `const char *`.
  Arrow functions are lambda-lifted to `static` functions (with an env struct if
  they capture outer variables). `?.` / `??` lower to short-circuit guards with
  **no wrapper objects** (e.g. `int32_t port = (cfg && cfg->has_port) ? cfg->port : 8080;`).
  Destructuring lowers via SROA (`const {x, y} = point` → read fields straight into registers).
- **Compile-time string fusion** for `console.log` & template literals: a
  concatenation/template is split at the AST level into a single `printf` call —
  no heap string allocation. One constant → `puts("...")`; string+var →
  `printf("... %lld ...\n", val)` with the format specifier picked from type inference.
- **Escape analysis:** locals that do not escape (not returned, not stored to a
  global, not captured by a closure) are lowered to stack structs/arrays — zero
  allocation cycles, automatically freed.
- **Micro-runtime `runtime/cmochicode_rt.h`** (125 lines, macro-based): the
  allocator is pluggable — `CMC_ALLOC`/`CMC_FREE` map to `malloc`/`free` in
  standalone mode, or to the MochiBerry GC/arena (`mbjs_gc_alloc`) when
  `-DCMC_WITH_MOCHIBERRY` is defined. The runtime is only touched when the
  program actually needs the allocator/GC or heap/string ops; everything else
  is `puts`/`printf`/arithmetic/structs.

## Build & use

### Compiler (`cmochi.exe`)

```sh
make                 # -> cmochi.exe (<200 KB)
./cmochi.exe in.js -o out.c    # transpile
gcc -std=c99 -O2 -s -I runtime out.c -o out -lm   # native binary
./out
```

`make check` transpiles + compiles all `examples/*.js`.

### MochiBerry native module (`mochi_modules`)

```js
const cm = await import('path/to/cmochicode.dll');
cm.version();                       // '0.1.0'
const c = cm.compile(src);          // JS string -> C99 string
const c2 = cm.compileFile('a.js');  // JS file -> C99 string
```

The DLL exports `mbjs_init_module` (the MochiBerry ABI) and binds
`compile`/`compileFile`/`version` via `SetModuleExportList` inside its
`mod_init` callback — same pattern as `mochifs.c`.

## Examples

`examples/` contains 10 programs that are verified to produce **byte-for-byte
identical output to node** (`make check` + a diff against `node file.js`):
`hello`, `math`, `strings`, `objects`, `arrays`, `control`, `fib` (the core 7)
plus `ecma2020`, `ecma_arrays`, `forin` (ECMA-2020+ extension).

### ECMA-2020+ extension coverage

Beyond ES2015/ES6, the C99 emitter also lowers:

- **Logical assignment** `||=` `&&=` `??=` (ternary/null-guard lowering).
- **Optional chaining** `?.` and **nullish coalescing** `??` (short-circuit, no
  wrapper objects).
- **Numeric separators** `1_000_000` / `0xFF_FF`.
- **Optional catch binding** `catch {}` (no binding) and `catch (e)`.
- **Default params** `(a, b = 10) => ...` (nullish-guarded `has_b`).
- **String methods** `includes`/`startsWith`/`endsWith`/`slice`/`replace`/
  `split`/`indexOf` (lowered to `mochi_*` runtime helpers).
- **Array methods** `map`/`filter`/`reduce`/`forEach` with an **inline arrow**:
  the callback body is inlined into a C loop — **no closure, no indirect call,
  no function pointer**.
- **`for...in`** over an object: keys materialized into a compile-time
  `const char *keys[]` field list, looped in C.
- **`true`/`false`** printed as words (not `1`/`0`).

Known limits (documented, not silently wrong): object spread `{...src}` and
call spread `f(...arr)` are accepted by the parser but not yet merged/lowered;
`obj[key]` with a *runtime* string key is not supported; the exception model is
best-effort (no unwinding).

## Layout

```
ccompiler/   C99 pipeline: cmochi.h, lex.c, parse.c, ir.c, emit*.c (5 TU), main.c
runtime/     cmochicode_rt.h (micro-runtime) + mochi.h/mochi_gc.h (GC wiring)
node_modules/ TGPM JS face layer: manifest.json, src/index.js, lib/, native/  (tracked, not npm output)
mochi_modules/ TGPM native layer: lib/ src/ include/ manifest.json script/ CMakeLists.txt
test/           full-loop module test (test_native.mjs)
examples/    15 verified JS->C examples
src/         acorn JS->C oracle, split into src/codegen/ (core/expr/stmt/helpers/index); not the C99 path
```

## Status

- C99 native pipeline is **green**: all 10 examples transpile, compile with
  `gcc -O2 -s -lm`, and match node output exactly.
- The MochiBerry native module (`mochi_modules/cmochicode.dll`) works: it
  compiles JS → C inside MochiBerry, and the emitted C compiles + runs and
  matches node.
- Compiler binary `cmochi.exe` ≈ **132 KB** (target < 200 KB).
- The generated C is C99-safe against **trigraphs** (`??=` etc. in literals
  are emitted as `\x3f`, so `-std=c99` never mangles them).
