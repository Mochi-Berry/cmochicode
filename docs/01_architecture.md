# cmochicode — architecture

JS -> C99 transpiler for MochiBerry: lex/parse/IR/emit pipeline, readable native C, borrows the host GC allocator, no bytecode.

```
JS face                    native core
node_modules/src/index.js  mochi_modules/src/cmochicode.c
  | loadNative()             |  mbjs_init_module()
  v                         v
mochinode loader       lib/cmochicode_x86_64.dll  (Schannel / Winsock2)
```

The JS face is a thin re-export of the C ABI — no wrapping layer, no state
duplicated between layers. All socket, TLS and certificate work happens in C;
JavaScript only marshals strings, integers and byte arrays.

## Call flow

1. `compile()` — runtime export (see the module README)
1. `compileFile()` — runtime export (see the module README)
1. `version()` — runtime export (see the module README)

## Why native-first

Schannel is a Win32 API surface (SSPI + Cert + Crypt). Calling it from C
keeps the credential- and certificate-handling code in one place, where the
OS contract is explicit; a JS shim would only add a translation layer.
