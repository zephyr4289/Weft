# core/c/synthetic/Makefile — Pillar 8 (Weft Synthetic Silicon Lab).
#
# Targets:
#   synth-battery{,-asan,-tsan}     T1-T3 / T4-T6 / N1-N7 / C1-C5 / M1
#                                   unit+integration battery
#   synth-torture{,-asan,-tsan}     2,000,000-cycle concurrent
#                                   thermal+bus+net chaos torture
#   synth-bench{,-asan,-tsan}       G1 (<5% disabled overhead vs the
#                                   interceptor-free recompile), G2
#                                   (determinism), G3 (per-op budgets),
#                                   G4 (seqlock retry p99 < 100 ns)
#   synth-stress                    standalone chaos-stress CLI
#   synth-thermal-curve             thermal response-curve CSV CLI
#   test / test-asan / test-tsan    the full battery legs
#
# Every compile is fail-closed: -std=c11 -Wall -Wextra -Werror -pedantic.
# The PLAIN battery leg links with allocator interposition
# (-Wl,--wrap=malloc,calloc,realloc) so the M1 zero-heap windows are
# witnessed; sanitizer legs skip the wrap (sanitizer runtimes own the
# allocator) — declared in the battery output and in D-82.
#
# G1 baseline discipline: bench links weft_synth_net.c TWICE — once as
# shipped, once compiled with -DWEFT_SYNTH_NET_NO_INTERCEPTOR and
# objcopy-renamed (twin_*) — so the disabled-interceptor overhead is
# priced against the SAME code with only the chaos layer removed.

CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -Werror -pedantic -pthread -D_GNU_SOURCE
CFLAGS_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
              -std=c11 -Wall -Wextra -Werror -pedantic -pthread -D_GNU_SOURCE
CFLAGS_TSAN = -O1 -g -fsanitize=thread -std=c11 -Wall -Wextra -Werror \
              -pedantic -pthread -D_GNU_SOURCE

BUILD    = ../../../tests/verify/build
NATIVE   = ../../../tests/verify/native
TOOLS    = ../../../tools/verify/synthetic

SYNTH_SRC = src/weft_synth_thermal.c src/weft_synth_bus.c \
            src/weft_synth_net.c
UTIL_SRC  = $(NATIVE)/test_util.c
INCLUDES  = -Iinclude -I$(NATIVE)
LDLIBS    = -pthread -lrt -lm

# plain-leg allocator interposition (the M1 zero-heap witnesses)
CFLAGS_WRAP = -DTU_ALLOC_GUARD
LD_WRAP     = -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc

all: synth-battery synth-torture synth-bench synth-stress \
     synth-thermal-curve

$(BUILD):
	mkdir -p $(BUILD)

# --- module objects (the runner's namespace gate consumes these) --------

$(BUILD)/weft_synth_thermal.o: src/weft_synth_thermal.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

$(BUILD)/weft_synth_bus.o: src/weft_synth_bus.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

$(BUILD)/weft_synth_net.o: src/weft_synth_net.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

MODULE_OBJS = $(BUILD)/weft_synth_thermal.o $(BUILD)/weft_synth_bus.o \
              $(BUILD)/weft_synth_net.o

# --- the interceptor-free recompile (G1 baseline, objcopy-renamed) ------

TWIN_RENAMES = $(BUILD)/twin_renames.txt

$(BUILD)/weft_synth_net_twin.o: src/weft_synth_net.c | $(BUILD)
	$(CC) $(CFLAGS) -DWEFT_SYNTH_NET_NO_INTERCEPTOR -Iinclude -c $< \
	    -o $(BUILD)/weft_synth_net_twin.raw.o
	nm --defined-only $(BUILD)/weft_synth_net_twin.raw.o | \
	    awk '$$2 ~ /^[TDBRC]$$/ {print $$3 " twin_" $$3}' > $(TWIN_RENAMES)
	objcopy --redefine-syms=$(TWIN_RENAMES) \
	    $(BUILD)/weft_synth_net_twin.raw.o
	mv $(BUILD)/weft_synth_net_twin.raw.o $@

$(BUILD)/weft_synth_net_twin-asan.o: src/weft_synth_net.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) -DWEFT_SYNTH_NET_NO_INTERCEPTOR -Iinclude -c $< \
	    -o $(BUILD)/weft_synth_net_twin-asan.raw.o
	nm --defined-only $(BUILD)/weft_synth_net_twin-asan.raw.o | \
	    awk '$$2 ~ /^[TDBRC]$$/ {print $$3 " twin_" $$3}' > $(TWIN_RENAMES)
	objcopy --redefine-syms=$(TWIN_RENAMES) \
	    $(BUILD)/weft_synth_net_twin-asan.raw.o
	mv $(BUILD)/weft_synth_net_twin-asan.raw.o $@

$(BUILD)/weft_synth_net_twin-tsan.o: src/weft_synth_net.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) -DWEFT_SYNTH_NET_NO_INTERCEPTOR -Iinclude -c $< \
	    -o $(BUILD)/weft_synth_net_twin-tsan.raw.o
	nm --defined-only $(BUILD)/weft_synth_net_twin-tsan.raw.o | \
	    awk '$$2 ~ /^[TDBRC]$$/ {print $$3 " twin_" $$3}' > $(TWIN_RENAMES)
	objcopy --redefine-syms=$(TWIN_RENAMES) \
	    $(BUILD)/weft_synth_net_twin-tsan.raw.o
	mv $(BUILD)/weft_synth_net_twin-tsan.raw.o $@

# --- battery --------------------------------------------------------------

synth-battery: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_battery.c | $(BUILD)
	$(CC) $(CFLAGS) $(CFLAGS_WRAP) $(INCLUDES) -o $(BUILD)/synth-battery \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_battery.c \
	    $(LDLIBS) $(LD_WRAP)

synth-battery-asan: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_battery.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/synth-battery-asan \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_battery.c $(LDLIBS)

synth-battery-tsan: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_battery.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -o $(BUILD)/synth-battery-tsan \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_battery.c $(LDLIBS)

# --- torture ---------------------------------------------------------------

synth-torture: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_torture.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/synth-torture \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_torture.c $(LDLIBS)

synth-torture-asan: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_torture.c | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/synth-torture-asan \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_torture.c $(LDLIBS)

synth-torture-tsan: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_torture.c | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -o $(BUILD)/synth-torture-tsan \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/test_synth_torture.c $(LDLIBS)

# --- gated benchmark ---------------------------------------------------------

synth-bench: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/bench_synth_lab.c \
             $(BUILD)/weft_synth_net_twin.o | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BUILD)/synth-bench \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/bench_synth_lab.c \
	    $(BUILD)/weft_synth_net_twin.o $(LDLIBS)

synth-bench-asan: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/bench_synth_lab.c \
                  $(BUILD)/weft_synth_net_twin-asan.o | $(BUILD)
	$(CC) $(CFLAGS_ASAN) $(INCLUDES) -o $(BUILD)/synth-bench-asan \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/bench_synth_lab.c \
	    $(BUILD)/weft_synth_net_twin-asan.o $(LDLIBS)

synth-bench-tsan: $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/bench_synth_lab.c \
                  $(BUILD)/weft_synth_net_twin-tsan.o | $(BUILD)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -o $(BUILD)/synth-bench-tsan \
	    $(SYNTH_SRC) $(UTIL_SRC) $(NATIVE)/bench_synth_lab.c \
	    $(BUILD)/weft_synth_net_twin-tsan.o $(LDLIBS)

# --- standalone CLIs ----------------------------------------------------------

synth-stress: $(SYNTH_SRC) $(TOOLS)/weft_synth_stress.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -o $(BUILD)/synth-stress \
	    $(SYNTH_SRC) $(TOOLS)/weft_synth_stress.c $(LDLIBS)

synth-thermal-curve: $(SYNTH_SRC) $(TOOLS)/weft_synth_thermal_curve.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -o $(BUILD)/synth-thermal-curve \
	    $(SYNTH_SRC) $(TOOLS)/weft_synth_thermal_curve.c $(LDLIBS)

# --- legs ----------------------------------------------------------------------

test: synth-battery synth-torture synth-bench
	$(BUILD)/synth-battery
	$(BUILD)/synth-torture
	$(BUILD)/synth-bench

test-asan: synth-battery-asan synth-torture-asan synth-bench-asan
	$(BUILD)/synth-battery-asan
	WEFT_QUICK=1 $(BUILD)/synth-torture-asan
	BENCH_NO_GATES=1 $(BUILD)/synth-bench-asan

test-tsan: synth-battery-tsan synth-torture-tsan
	WEFT_QUICK=1 $(BUILD)/synth-battery-tsan
	WEFT_QUICK=1 $(BUILD)/synth-torture-tsan

clean:
	rm -f $(BUILD)/synth-* $(BUILD)/*.o $(BUILD)/twin_renames.txt

.PHONY: all test test-asan test-tsan clean
