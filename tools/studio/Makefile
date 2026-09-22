# tools/studio/Makefile — Weft Studio core build (Law 3: strict C11)
#
# Targets:
#   all            weft-lsp-server + weftrec-inspect CLIs
#   studio-tests   the five core test binaries (plain, -O2)
#   asan-tests     the test binaries under ASan+UBSan
#   check          studio-tests + run each once
#   clean
#
# The canonical gate battery lives in tests/run_studio_core_suite.sh
# (dual-compiler strict, oracles, fuzz, alloc probe, sanitizers, ABI).

CC       ?= gcc
CFLAGS   ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
CFLAGS_ASAN = -O1 -g -std=c11 -Wall -Wextra -Werror -pedantic \
              -fsanitize=address,undefined

ROOT     = ../..
INC      = $(ROOT)/core/c/include
SRC      = $(ROOT)/core/c/studio/src
TESTS    = $(ROOT)/tests/studio/core
ENG_O    = $(SRC)/weftc_inmem.o $(SRC)/weft_lsp.o $(SRC)/weftrec_engine.o
LDLIBS   = -ldl

ENGINES  = $(SRC)/weftc_inmem.c $(SRC)/weft_lsp.c $(SRC)/weftrec_engine.c

.PHONY: all studio-tests asan-tests check clean

all: weft-lsp-server weftrec-inspect

weft-lsp-server: weft_lsp_server.c $(ENG_O)
	$(CC) $(CFLAGS) -I$(INC) -I$(SRC) -o $@ weft_lsp_server.c $(ENG_O) $(LDLIBS)

weftrec-inspect: weftrec_inspect.c $(ENG_O)
	$(CC) $(CFLAGS) -I$(INC) -I$(SRC) -o $@ weftrec_inspect.c $(ENG_O)

$(SRC)/weftc_inmem.o: $(SRC)/weftc_inmem.c $(SRC)/weft_studio_internal.h $(INC)/weft_studio.h
	$(CC) $(CFLAGS) -I$(INC) -c $(SRC)/weftc_inmem.c -o $@

$(SRC)/weft_lsp.o: $(SRC)/weft_lsp.c $(SRC)/weft_studio_internal.h $(INC)/weft_studio.h
	$(CC) $(CFLAGS) -I$(INC) -c $(SRC)/weft_lsp.c -o $@

$(SRC)/weftrec_engine.o: $(SRC)/weftrec_engine.c $(SRC)/weft_crc32c_tables.h $(SRC)/weft_studio_internal.h $(INC)/weft_studio.h
	$(CC) $(CFLAGS) -I$(INC) -c $(SRC)/weftrec_engine.c -o $@

# ---- test binaries ----------------------------------------------------

T_PLAIN = studio-test-inmem studio-test-lsp studio-test-weftrec \
          studio-alloc-probe studio-fuzz
T_ASAN  = $(addsuffix -asan,$(T_PLAIN))

studio-tests: $(T_PLAIN)

studio-test-inmem: $(TESTS)/test_weftc_inmem.c $(SRC)/weftc_inmem.o
	$(CC) $(CFLAGS) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_weftc_inmem.c $(SRC)/weftc_inmem.o

studio-test-lsp: $(TESTS)/test_oracle_lsp.c $(SRC)/weftc_inmem.o $(SRC)/weft_lsp.o
	$(CC) $(CFLAGS) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_oracle_lsp.c $(SRC)/weftc_inmem.o $(SRC)/weft_lsp.o

studio-test-weftrec: $(TESTS)/test_oracle_weftrec.c $(SRC)/weftc_inmem.o $(SRC)/weftrec_engine.o
	$(CC) $(CFLAGS) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_oracle_weftrec.c $(SRC)/weftc_inmem.o \
	    $(SRC)/weftrec_engine.o

studio-alloc-probe: $(TESTS)/test_alloc_probe.c $(ENG_O)
	$(CC) $(CFLAGS) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_alloc_probe.c $(ENG_O) $(LDLIBS)

studio-fuzz: $(TESTS)/test_fuzz_studio.c $(SRC)/weftc_inmem.o $(SRC)/weftrec_engine.o
	$(CC) $(CFLAGS) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_fuzz_studio.c $(SRC)/weftc_inmem.o \
	    $(SRC)/weftrec_engine.o

# ---- sanitizer legs (ASan + UBSan; reduced fuzz) ----------------------

asan-tests: $(T_ASAN)

studio-test-inmem-asan: $(TESTS)/test_weftc_inmem.c $(SRC)/weftc_inmem.c
	$(CC) $(CFLAGS_ASAN) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_weftc_inmem.c $(SRC)/weftc_inmem.c

studio-test-lsp-asan: $(TESTS)/test_oracle_lsp.c $(SRC)/weftc_inmem.c $(SRC)/weft_lsp.c
	$(CC) $(CFLAGS_ASAN) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_oracle_lsp.c $(SRC)/weftc_inmem.c $(SRC)/weft_lsp.c

studio-test-weftrec-asan: $(TESTS)/test_oracle_weftrec.c $(SRC)/weftc_inmem.c $(SRC)/weftrec_engine.c
	$(CC) $(CFLAGS_ASAN) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_oracle_weftrec.c $(SRC)/weftc_inmem.c \
	    $(SRC)/weftrec_engine.c

studio-alloc-probe-asan: $(TESTS)/test_alloc_probe.c $(ENGINES)
	$(CC) $(CFLAGS_ASAN) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_alloc_probe.c $(ENGINES) $(LDLIBS)

studio-fuzz-asan: $(TESTS)/test_fuzz_studio.c $(SRC)/weftc_inmem.c $(SRC)/weftrec_engine.c
	$(CC) $(CFLAGS_ASAN) -I$(INC) -I$(SRC) -I$(TESTS) -o $@ \
	    $(TESTS)/test_fuzz_studio.c $(SRC)/weftc_inmem.c \
	    $(SRC)/weftrec_engine.c

check: studio-tests
	./studio-test-inmem && ./studio-test-lsp && ./studio-test-weftrec && \
	./studio-alloc-probe && ./studio-fuzz

clean:
	rm -f weft-lsp-server weftrec-inspect $(T_PLAIN) $(T_ASAN) \
	      $(SRC)/weftc_inmem.o $(SRC)/weft_lsp.o $(SRC)/weftrec_engine.o
