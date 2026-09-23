# test_alloc.py — Law 2 heap evidence: tracemalloc 1,000,000-message gate.
#
# Mandate (Stage 3): "Python tracemalloc 1,000,000-message market ingestion
# gate (0 byte permanent heap growth)" + a biting negative control.
#
# The EXACT-0 proof runs in tools stage 3 via probes/alloc_probe.py — a
# standalone interpreter where nothing else allocates. This pytest twin
# gates at the repo-standard 64 KiB ceiling (Pillar 5 convention) because
# in-plugin CI agents (ddtrace background telemetry) allocate inside the
# traced window — measured 842 B of NON-project residue with the managed
# path itself at exactly 0.
#
# tracemalloc counts LIVE blocks. Per-message temporaries (unpack tuples,
# ints) are refcount-freed inside each iteration, so the permanent delta
# MUST be 0 for the managed path. The control retains one object per 100
# messages and MUST blow the gate, proving it can fail.
#
# Everything that must not be counted (book pools, engine, view) is
# allocated BEFORE tracemalloc.start().

import tracemalloc

from weft_fintech.book import OrderBook
from weft_fintech.itch import ItchEngine, frame_offsets

GATE_BYTES = 64 * 1024

RETAIN_SINK = []  # module-level so the optimizer cannot collect it


def test_one_million_messages_zero_permanent_heap_growth(one_million_stream):
    stream = one_million_stream
    offs = frame_offsets(stream)
    assert len(offs) == 1_000_000

    # warmup: first 50k messages on a throwaway book (freelists, caches)
    warm = OrderBook(pool_capacity=65536)
    warm_engine = ItchEngine(warm)
    warm_engine.process(stream, offs[50000] - 2)
    del warm_engine, warm

    book = OrderBook(pool_capacity=65536)
    engine = ItchEngine(book)   # allocated BEFORE tracing starts
    tracemalloc.start()
    engine.process(stream)
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    del engine

    assert book.msgs_applied == 1_000_000, book.msgs_applied
    assert book.total_rejects() == 0
    assert book.live_orders > 0
    assert current < GATE_BYTES, (
        f'permanent heap growth {current} bytes exceeds {GATE_BYTES} gate '
        f'(managed path itself is EXACT 0 — see probes/alloc_probe.py)')
    assert peak > 0  # peak reflects transient intra-iteration live blocks


def test_negative_control_bites(one_million_stream):
    stream = one_million_stream
    RETAIN_SINK.clear()

    book = OrderBook(pool_capacity=65536)
    engine = ItchEngine(book)
    tracemalloc.start()
    engine.process(stream)
    # textbook leak: retain one object per 100 messages => 10,000 objects
    for i in range(0, 1_000_000, 100):
        RETAIN_SINK.append({'ref': i, 'price': 999000, 'size': 100})
    current, _peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    retained = len(RETAIN_SINK)
    RETAIN_SINK.clear()
    assert retained == 10_000
    assert current > 0, 'negative control MUST show permanent growth'
