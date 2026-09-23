# Makefile — the native Vulkan probe (Tier 1 native leg).
#
#   make            build vk-heddle-probe (+ the .spv when glslangValidator
#                   is present; the committed .spv is canonical otherwise —
#                   the house pattern from core/c/Makefile)
#   make run        build + run under the current VK_ICD_FILENAMES
#   make clean
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
GLSLANG ?= glslangValidator

SPV = ../shaders/vk/osc_decimate.spv
COMP = ../shaders/vk/osc_decimate.comp

.PHONY: all run clean

all: vk-heddle-probe

$(SPV): $(COMP)
	@if command -v $(GLSLANG) >/dev/null 2>&1; then \
		$(GLSLANG) -V $< -o $@; \
	else echo "$(GLSLANG) not found — using committed $@"; touch $@; fi

vk-heddle-probe: vk_heddle_probe.c whp1_layout.h $(SPV)
	$(CC) $(CFLAGS) -I. -I../../../core/c -o $@ vk_heddle_probe.c -ldl -lm

run: vk-heddle-probe
	./vk-heddle-probe

clean:
	rm -f vk-heddle-probe
