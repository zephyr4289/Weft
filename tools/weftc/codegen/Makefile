# tools/weftc/codegen/Makefile — build + verify the weftc native codegen
# backends (Pillar 1: C11 / Rust / WGSL / GLSL).
#
#   make        — build the weftc-codegen driver (pure C11, -Werror -pedantic)
#   make test   — run tests/run_tests.sh (determinism, goldens, C roundtrip
#                 x4 build flavors, Rust cargo tests, GPU layout double-entry,
#                 optional glslang compile)
#   make clean  — remove build + test artifacts

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
SRC     := main.c weftc_codegen.c weft_ir.c \
           c/emit_c.c rust/emit_rust.c \
           gpu/gpu_layout.c gpu/emit_wgsl.c gpu/emit_glsl.c
HDR     := weftc_codegen.h gpu/gpu_layout.h
BIN     := weftc-codegen

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

test: all
	./tests/run_tests.sh

clean:
	rm -f $(BIN)
	rm -rf tests/out tests/rust/src/gen tests/rust/target

.PHONY: all test clean
