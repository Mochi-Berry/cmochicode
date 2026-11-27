# Makefile — build the cmochi native JS->C compiler (C99 pipeline).
#
#   make            -> cmochi.exe (the compiler)
#   make app SRC=examples/hello.js
#   make check      -> transpile + compile + run all examples
#
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -I ccompiler -I runtime
SRC     = ccompiler/main.c ccompiler/lex.c ccompiler/parse.c ccompiler/ir.c ccompiler/emit.c

cmochi: $(SRC) ccompiler/cmochi.h runtime/cmochicode_rt.h
	$(CC) $(CFLAGS) -o cmochi.exe $(SRC)

# Transpile one example and compile the result to a native binary.
app: cmochi
	@if [ -z "$(SRC)" ]; then echo "usage: make app SRC=examples/hello.js"; exit 1; fi
	./cmochi.exe $(SRC) -o build/$(basename $(notdir $(SRC))).c
	$(CC) -std=c99 -O2 -s -I runtime build/$(basename $(notdir $(SRC))).c -o build/$(basename $(notdir $(SRC))).exe -lm

check: cmochi
	@mkdir -p build
	@for f in examples/*.js; do \
	  name=$$(basename $$f .js); \
	  echo "== $$name =="; \
	  ./cmochi.exe $$f -o build/$$name.c || exit 1; \
	  $(CC) -std=c99 -O2 -s -I runtime build/$$name.c -o build/$$name.exe -lm || exit 1; \
	done
	@echo "OK: all examples transpiled + compiled."

clean:
	rm -f cmochi.exe build/*.c build/*.exe

.PHONY: app check clean
