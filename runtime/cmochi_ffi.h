#ifndef CMOCHI_FFI_H
#define CMOCHI_FFI_H

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define CMC_DL_OPEN(p)  ((void *)LoadLibraryA((const char *)(p)))
#define CMC_DL_SYM(h,n) ((void *)GetProcAddress((HMODULE)(h), (const char *)(n)))
#define CMC_DL_CLOSE(h) ((void)FreeLibrary((HMODULE)(h)))
#else
#include <dlfcn.h>
#define CMC_DL_OPEN(p)  dlopen((const char *)(p), RTLD_NOW | RTLD_LOCAL)
#define CMC_DL_SYM(h,n) dlsym((void *)(h), (const char *)(n))
#define CMC_DL_CLOSE(h) ((void)dlclose((void *)(h)))
#endif

#define CMC_BIND_FN(slot, h, name) do { \
    void *__cmc_fp_ = CMC_DL_SYM((h), (name)); \
    memcpy(&(slot), &__cmc_fp_, sizeof __cmc_fp_); \
} while (0)

#define CMC_BIND_VAR(slot, T, h, name) \
    ((slot) = (T *)CMC_DL_SYM((h), (name)))

#endif
