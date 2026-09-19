# weftc — RFC-0017 universal zero-serialization schema compiler
#
# Targets mirror the repo's C tool naming (see tools/weft-fanout-rec):
#   weftc            O2 CLI binary (check / inspect / compile)
#   test             full gate: layout + hash + dump suites + goldens
#   test-asan        same gate under AddressSanitizer (Law 2 battery)
#   clean
#
# C11 + libc only (Law 4 — no external dependencies).

CC       ?= cc
CFLAGS   ?= -O2 -std=c11 -Wall -Wextra
CFLAGS_ASAN = -O1 -g -std=c11 -Wall -Wextra -fsanitize=address

SRC      = src/lex.c src/parse.c src/diag.c src/layout.c src/hash.c \
           src/dump.c src/main.c
LIBSRC   = src/lex.c src/parse.c src/diag.c src/layout.c src/hash.c \
           src/dump.c

.PHONY: all test test-asan clean

all: weftc

weftc: $(SRC) src/weftc.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: weftc tests/test_layout tests/test_hash tests/test_dump
	bash tests/run.sh

tests/test_layout: tests/test_layout.c $(LIBSRC) src/weftc.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_layout.c $(LIBSRC)

tests/test_hash: tests/test_hash.c $(LIBSRC) src/weftc.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_hash.c $(LIBSRC)

tests/test_dump: tests/test_dump.c $(LIBSRC) src/weftc.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_dump.c $(LIBSRC)

test-asan: clean-bin
	$(CC) $(CFLAGS_ASAN) -Isrc -o tests/test_layout tests/test_layout.c $(LIBSRC)
	$(CC) $(CFLAGS_ASAN) -Isrc -o tests/test_hash tests/test_hash.c $(LIBSRC)
	$(CC) $(CFLAGS_ASAN) -Isrc -o tests/test_dump tests/test_dump.c $(LIBSRC)
	$(CC) $(CFLAGS_ASAN) -o weftc $(SRC)
	bash tests/run.sh

clean: clean-bin
	rm -f tests/golden/*.actual

clean-bin:
	rm -f weftc tests/test_layout tests/test_hash tests/test_dump
