# tools/verify/Makefile — Weft Pillar 8 convenience wrapper.
# The NORMATIVE entry point is tests/run_verify_core_suite.sh (fail-closed
# 6-gate G1-G6). This Makefile just exposes quick developer targets.

ROOT := ../..
BUILD := $(ROOT)/build/verify
EV := evidence

CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS := -std=c++17 -Wall -Wextra -Werror -pedantic -O2
INC := -I$(ROOT)/core/c/verify/include -I$(ROOT)/tools/weftc/lint

.PHONY: all suite tests clean evidence

all: tests

tests: $(BUILD)/weftc-lint $(BUILD)/test_verify_bounds \
       $(BUILD)/test_weftc_lint_alloc $(BUILD)/test_oracle_tla

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/weftc-lint: tools_check
	$(CC) $(CFLAGS) $(INC) ../../tools/weftc/lint/weftc_lint_main.c \
	    ../../tools/weftc/lint/weftc_lint_alloc.c -o $@

$(BUILD)/test_verify_bounds: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) ../../tests/verify/core/test_verify_bounds.c \
	    ../../core/c/verify/src/weft_verify_bounds.c -o $@

$(BUILD)/test_weftc_lint_alloc: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) ../../tests/verify/core/test_weftc_lint_alloc.c \
	    ../../tools/weftc/lint/weftc_lint_alloc.c -o $@

$(BUILD)/test_oracle_tla: | $(BUILD)
	$(CC) $(CFLAGS) ../../tests/verify/core/test_oracle_tla.c -o $@

tools_check:
	@test -d ../../tools/weftc/lint || { mkdir -p $(BUILD); }

# full fail-closed gate suite (G1-G6; ~15 min: TLC dominates)
suite:
	bash tests/run_verify_core_suite.sh

clean:
	rm -rf $(BUILD)

evidence:
	ls -la $(EV)
