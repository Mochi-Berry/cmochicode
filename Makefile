# Makefile — build the cmochi native JS->C compiler (C99 pipeline).
#
#   make            -> cmochi.exe (the compiler)
#   make app APPSRC=examples/hello.js
#   make check      -> transpile + compile + run all examples
#
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -I ccompiler -I runtime
UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Linux)
DL_LIBS = -ldl
else
DL_LIBS =
endif
CSRC    = ccompiler/main.c ccompiler/lex.c ccompiler/parse.c ccompiler/ir.c \
          ccompiler/emit.c ccompiler/emit_helpers.c ccompiler/emit_ffi.c \
          ccompiler/emit_expr.c ccompiler/emit_stmt.c ccompiler/emit_class.c

cmochi: $(CSRC) ccompiler/cmochi.h ccompiler/emit_internal.h \
        runtime/cmochicode_rt.h runtime/cmochi_ffi.h
	$(CC) $(CFLAGS) -o cmochi.exe $(CSRC)

# MochiBerry native module (TGPM layout):
#   mochi_modules/src/cmochicode.c -> mochi_modules/lib/cmochicode_x86_64.dll
MODULE = mochi_modules/lib/cmochicode_x86_64.dll
ENGINE_INC ?= E:/bcf/mochiberry

module: $(MODULE)

$(MODULE): mochi_modules/src/cmochicode.c $(CSRC) mochi_modules/include/cmochicode.h
	@mkdir -p mochi_modules/lib
	$(CC) -shared -O2 -o $@ mochi_modules/src/cmochicode.c \
	    ccompiler/lex.c ccompiler/parse.c ccompiler/ir.c \
	    ccompiler/emit.c ccompiler/emit_helpers.c ccompiler/emit_ffi.c \
	    ccompiler/emit_expr.c ccompiler/emit_stmt.c ccompiler/emit_class.c \
	    -Iccompiler -Iruntime -Imochi_modules/include -I$(ENGINE_INC) -IE:/sevm/native

# Transpile one example and compile the result to a native binary.
app: cmochi
	@if [ -z "$(APPSRC)" ]; then echo "usage: make app APPSRC=examples/hello.js"; exit 1; fi
	./cmochi.exe $(APPSRC) -o build/$(basename $(notdir $(APPSRC))).c
	$(CC) -std=c99 -O2 -s -I runtime build/$(basename $(notdir $(APPSRC))).c -o build/$(basename $(notdir $(APPSRC))).exe -lm $(DL_LIBS)

check: cmochi
	@mkdir -p build
	@for f in examples/*.js; do \
	  name=$$(basename $$f .js); \
	  echo "== $$name =="; \
	  ./cmochi.exe $$f -o build/$$name.c || exit 1; \
	  $(CC) -std=c99 -O2 -s -I runtime build/$$name.c -o build/$$name.exe -lm $(DL_LIBS) || exit 1; \
	done
	@echo "OK: all examples transpiled + compiled."

ffi-demo:
	@mkdir -p build
	$(CC) -shared -O2 -o build/demoffi.dll examples/native/demoffi.c

clean:
	rm -f cmochi.exe build/*.c build/*.exe build/demoffi.dll

.PHONY: module app check ffi-demo clean
