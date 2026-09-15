# Weft Phase 5 — Root Makefile
# Per WO-P5-RELEASE §1.T5 + §3 success criteria.
#
# Five contracted targets:
#   make litmus    — 24 cells (8 tests × 3 languages), exit 0
#   make bench     — B-cells + W-cells; canonical bundle isolation (decision 1)
#   make site      — regenerate static HTML from bundles
#   make validate  — port_validator over all four source-only targets
#   make build-*   — language-specific builds
#
# `make validate` is wired to fail the release on any non-zero validator exit.

ROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
export PATH := $(HOME)/.cargo/bin:$(PATH)

.PHONY: all build build-c build-rust build-ts litmus bench site validate clean

all: litmus bench site validate

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------

build: build-c build-rust build-ts

build-c:
	cd core/c && $(MAKE)

build-rust:
	cd core/rust && cargo build --release

build-ts:
	@node -e "import('./core/ts/weft.ts').then(() => console.log('TS kernel loads OK'))"

# ---------------------------------------------------------------------------
# make litmus — Phase 0 gate (24 cells: 8 tests × 3 languages × 2 builds)
# ---------------------------------------------------------------------------

litmus: build-c build-rust build-ts
	python3 tools/litmus_driver.py --langs c,rust,ts

# ---------------------------------------------------------------------------
# make bench — Phase 1 (B-cells) + Phase 5 (W-cells)
#
# Canonical bundle isolation per WO-P5-RELEASE decision 1:
# - bench/results.json (sha256 16b5c663) ships AS-IS — the whitepaper's pinned source
# - re-runs write to scratch path; verify harness runs green; discard scratch
# - W-suite results write to bench/results/wsuite-*.json (separate file)
# ---------------------------------------------------------------------------

BENCH_LANGS ?= c,rust,ts

build-c-bench:
	cd core/c && gcc -O2 -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
	        -o bench bench_runner.c weft.c

bench: build-c-bench
	@echo "=== B-suite (canonical: bench/results.json, sha256 16b5c663) ==="
	@# Per WO-P5-RELEASE decision 1: canonical bundle ships untouched.
	@# Re-run writes to a scratch path; verify harness runs green; discard scratch.
	@# The bench_driver doesn't natively support --out, so we capture via temp copy
	@# of the canonical bundle, run the driver (which writes to bench/results.json),
	@# check green, then restore the canonical bundle.
	@canon_sha=$$(sha256sum bench/results.json | cut -d' ' -f1); \
	        cp bench/results.json /tmp/weft-canonical-results.json.bak; \
	        echo "Canonical bundle pre-run sha256: $$canon_sha"; \
	        if python3 tools/bench_driver.py --langs $(BENCH_LANGS); then \
	                echo "B-suite harness: green"; \
	                new_sha=$$(sha256sum bench/results.json | cut -d' ' -f1); \
	                echo "Re-run produced sha256: $$new_sha (different from canonical — expected per decision 1)"; \
	                cp /tmp/weft-canonical-results.json.bak bench/results.json; \
	                restored_sha=$$(sha256sum bench/results.json | cut -d' ' -f1); \
	                case "$$restored_sha" in \
	                        16b5c663*) echo "Canonical bundle restored: $$restored_sha matches 16b5c663... ✓"; \
	                                rm /tmp/weft-canonical-results.json.bak; \
	                                echo ""; \
	                                echo "=== W-suite (separate bundle: bench/results/wsuite-x86_64-sandbox.json) ==="; \
	                                PYTHONPATH=bench/workloads python3 bench/workloads/wsuite_runner.py \
	                                        --workloads W1,W2,W3,W4,W5 --backends A,B,C,D ;; \
	                        *) echo "!! Canonical bundle restore failed — release gate FAILS"; \
	                                rm /tmp/weft-canonical-results.json.bak; \
	                                exit 1 ;; \
	                esac; \
	        else \
	                echo "!! B-suite harness failed — release gate FAILS"; \
	                cp /tmp/weft-canonical-results.json.bak bench/results.json 2>/dev/null; \
	                rm /tmp/weft-canonical-results.json.bak; \
	                exit 1; \
	        fi

# ---------------------------------------------------------------------------
# make site — Phase 5 / T4: regenerate static HTML from bundles
# ---------------------------------------------------------------------------

site:
	python3 tools/make_site.py
	@# Determinism check: regenerating must produce byte-identical output
	@first=$$(sha256sum bench/site/index.html | cut -d' ' -f1); \
	        python3 tools/make_site.py > /dev/null 2>&1; \
	        second=$$(sha256sum bench/site/index.html | cut -d' ' -f1); \
	        if [ "$$first" = "$$second" ]; then \
	                echo "Site regeneration: deterministic ✓ (sha256 $$first)"; \
	        else \
	                echo "!! Site regeneration: NON-deterministic — release gate FAILS"; \
	                exit 1; \
	        fi

# ---------------------------------------------------------------------------
# make validate — Phase 4 port structural validation
# ---------------------------------------------------------------------------

validate:
	python3 tools/port_validator.py
	@exit $$?

# ---------------------------------------------------------------------------
# make dist — build release tarball reproducibly
# ---------------------------------------------------------------------------

dist:
	@git archive --format=tar --prefix=weft-sandbox-v0.1/ HEAD | gzip -n > weft-sandbox-v0.1.tar.gz
	@sha=$$(sha256sum weft-sandbox-v0.1.tar.gz | cut -d' ' -f1); \
	echo "Built weft-sandbox-v0.1.tar.gz (sha256: $$sha)"

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

clean:
	rm -f core/c/spike core/c/spike-tsan core/c/bench core/c/libweft.so
	rm -rf core/rust/target
	rm -f litmus/REPORT.md litmus/results.json
	rm -rf bench/site
	rm -f weft-sandbox-v0.1.tar.gz
