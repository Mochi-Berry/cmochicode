# cmochicode — ABI reference

Entry point: `mbjs_init_module(MBJSContext*, const char*)`.
All runtime `MBJS_*` symbols are resolved from the host engine at load time.

| export | behaviour |
|--------|-----------|
| `compile()` | runtime export (see the module README) |
| `compileFile()` | runtime export (see the module README) |
| `version()` | runtime export (see the module README) |

## Return convention

Functions that report failures return `MBJS_EXCEPTION` (never
`MBJS_UNDEFINED`) and set the last-error string, which `tlsLastError()`
returns in full — error code, facility and message. Success is `MBJS_UNDEFINED`
or the plain value.

Certificate chain validation is the OS default (revocation checking enabled
per Schannel policy); `tlsVersion()` reports the negotiated protocol so a
caller can refuse TLS < 1.2 explicitly.
