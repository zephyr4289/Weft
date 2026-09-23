#!/usr/bin/env python3
# probes/alloc_probe.py — Stage 3 EXACT-0 permanent-heap proof (standalone).
#
# The Stage 3 mandate: "Python tracemalloc 1,000,000-message market
# ingestion gate — 0 byte permanent heap growth". Under pytest, in-plugin
# CI agents (ddtrace background telemetry) allocate inside the traced
# window (~800 B of non-project residue), so the EXACT-0 proof runs HERE,
# in a clean interpreter with no plugins, while the pytest twin
# (test/test_alloc.py) gates at the repo-standard 64 KiB ceiling.
#
# Exit codes: 0 = proof holds; 2 = gate violated (silent-green forbidden);
# 3 = negative control did NOT bite (probe insensitive).

import os
import subprocess
import sys
import tracemalloc
from pathlib import Path

PKG_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = PKG_ROOT.parent.parent           # python/weft_fintech -> Weft/
FIXTURES = REPO_ROOT / 'tests' / 'adapters' / 'managed' / 'fixtures'

sys.path.insert(0, str(PKG_ROOT / 'src'))

from weft_fintech.book import OrderBook       # noqa: E402
from weft_fintech.itch import ItchEngine, frame_offsets  # noqa: E402

CYCLES = 1_000_000
WARMUP = 50_000
RETAIN_SINK = []


def get_stream():
    cache = Path(os.environ.get('TMPDIR', '/tmp')) / 'weft-adapters-1m.bin'
    if not cache.exists() or cache.stat().st_size < 1_000_000:
        subprocess.run(
            ['node', str(FIXTURES / 'dump_stream.mjs'),
             '--count', '1000000', '--out', str(cache)],
            check=True, capture_output=True, text=True)
    return cache.read_bytes()


def main():
    stream = get_stream()
    offs = frame_offsets(stream)
    assert len(offs) == CYCLES, f'fixture has {len(offs)} frames'

    # warmup: freelists / int caches / code objects on a throwaway engine
    warm = OrderBook(pool_capacity=65536)
    warm_engine = ItchEngine(warm)
    warm_engine.process(stream, offs[WARMUP] - 2)
    del warm_engine, warm

    # --- EXACT-0 gate -----------------------------------------------------
    book = OrderBook(pool_capacity=65536)
    engine = ItchEngine(book)          # allocated BEFORE tracing starts
    tracemalloc.start()
    engine.process(stream)
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    ok = True
    print(f'managed ingestion of {CYCLES} messages:')
    print(f'  permanent heap growth : {current} byte(s)  (mandate: 0)')
    print(f'  transient peak        : {peak} B')
    print(f'  msgs_applied          : {book.msgs_applied}')
    print(f'  rejects               : {book.total_rejects()}')
    print(f'  live_orders           : {book.live_orders}')

    if book.msgs_applied != CYCLES:
        print('FAIL: msgs_applied != 1,000,000', file=sys.stderr)
        ok = False
    if book.total_rejects() != 0:
        print('FAIL: unexpected rejects', file=sys.stderr)
        ok = False
    if current != 0:
        print(f'FAIL: permanent heap growth {current} != 0 '
              '(Law 1 / Stage 3 mandate)', file=sys.stderr)
        ok = False
    if peak <= 0:
        print('FAIL: peak must reflect transient intra-iteration blocks',
              file=sys.stderr)
        ok = False

    # --- negative control: the gate MUST be able to fail -------------------
    book2 = OrderBook(pool_capacity=65536)
    engine2 = ItchEngine(book2)
    RETAIN_SINK.clear()
    tracemalloc.start()
    engine2.process(stream)
    for i in range(0, CYCLES, 100):
        RETAIN_SINK.append({'ref': i, 'price': 999000, 'size': 100})
    leaked, _ = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    retained = len(RETAIN_SINK)
    RETAIN_SINK.clear()

    print(f'negative control: retained {retained} objects -> +{leaked} B')
    if retained != 10_000 or leaked <= 0:
        print('FAIL: negative control did NOT bite — probe insensitive '
              '(silent-green violation)', file=sys.stderr)
        ok = False

    if not ok:
        sys.exit(2)
    print('PROOF: managed market ingestion retains EXACTLY 0 bytes over '
          '1,000,000 messages; control bites. Stage 3 PASS.')
    sys.exit(0)


if __name__ == '__main__':
    main()
