#!/usr/bin/env bash
# run_series10_shard.sh — Series 10 observability + reactive flow gate
# (RFC 0016 flight recorder + Perfetto bridge, RFC 0017 flow operators,
#  RFC 0018 sensor fusion, RFC 0019 time-travel replay, RFC 0020 trend).
#
# Everything this shard runs is deterministic (injected clocks everywhere —
# Law 4); the Rust/VM parity legs self-skip when their toolchains are
# absent (declared, per the per-port honesty pattern) — the workflows that
# own those toolchains run the same fixtures with them present.
#
# Outputs: ci/run-artifacts/shard-series10.log + the flight demo artifacts
# (.weftrec + .wsid + trace.json) for the Perfetto evidence bundle.
set -euo pipefail
mkdir -p ci/run-artifacts

{
  echo "== Series 10 shard: observability fabric + zero-alloc flow =="

  # 1. C conformance suites (T/O/Y/R/D series) — release + ASan
  echo "-- [1/5] C series suites (release + ASan)"
  make -C core/c trace-test trace-test-dbg trace-test-asan
  ./core/c/trace-test
  ./core/c/trace-test-dbg > /dev/null   # binding-discipline build (quiet)
  ./core/c/trace-test-asan > /dev/null  # ASan-clean assertion
  make -C core/c replay-test replay-test-asan flow-test flow-test-asan \
    sync-test sync-test-asan trend-test trend-test-asan replay-runner trend-runner
  ./core/c/replay-test
  ./core/c/replay-test-asan > /dev/null
  ./core/c/flow-test
  ./core/c/flow-test-asan > /dev/null
  ./core/c/sync-test
  ./core/c/sync-test-asan > /dev/null
  ./core/c/trend-test
  ./core/c/trend-test-asan > /dev/null

  # 2. TS ports (packages/core: replay + trend pinned parity, full suite)
  echo "-- [2/5] TS port suite (@weft/core)"
  if command -v pnpm > /dev/null 2>&1; then
    pnpm --filter @weft/core build > /dev/null
    pnpm --filter @weft/core test 2>&1 | tail -2
  elif [ -f packages/core/dist/index.js ]; then
    # no workspace manager in this environment: dist must already exist
    # (the build gate owns the fresh build; here we run the suite directly)
    (cd packages/core && npx vitest run 2>&1 | tail -2)
  else
    echo "packages/core/dist missing and pnpm absent" >&2
    exit 1
  fi

  # 3. Perfetto bridge golden gate
  echo "-- [3/5] Perfetto bridge (golden + cross-process determinism)"
  node tools/perfetto/test.mjs

  # 4. Cross-language parity fixtures (C + TS always; Rust/VM declared skips)
  echo "-- [4/5] xlang parity fixtures (replay + trend)"
  if command -v cargo > /dev/null 2>&1; then
    (cd core/rust && cargo build --release --bin replay_xlang --bin trend_xlang > /dev/null 2>&1 || true)
  fi
  fixtures/xlang-replay/run.sh
  fixtures/xlang-trend/run.sh

  # 5. Flight demo: full-fabric integration — kernel + recorder + governor
  #    + trend -> .weftrec + .wsid -> Perfetto JSON artifacts
  echo "-- [5/5] flight demo (full-fabric integration)"
  make -C core/c flight-runner
  ./core/c/flight-runner --out ci/run-artifacts/flight.weftrec \
    --sidecar ci/run-artifacts/flight.wsid
  node tools/perfetto/weftrec2perfetto.mjs ci/run-artifacts/flight.weftrec \
    --sidecar ci/run-artifacts/flight.wsid \
    --out ci/run-artifacts/flight-trace.json

  echo "== Series 10 shard: ALL GREEN =="
} 2>&1 | tee ci/run-artifacts/shard-series10.log

# No silent green: the pipeline above fails the shard on any nonzero exit.
exit "${PIPESTATUS[0]}"
