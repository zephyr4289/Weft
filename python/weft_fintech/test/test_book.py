# test_book.py — O(1) book semantics mirrored from packages/fintech book tests.

from generate_helpers import fake_view
from weft_fintech.book import (
    OrderBook, E_DUP_REF, E_UNKNOWN_REF, E_POOL_FULL, E_PRICE_RANGE,
    E_BAD_SIZE,
)

A, E, X, D, U, P = 0x41, 0x45, 0x58, 0x44, 0x55, 0x50


def test_aggregation_levels_and_best_tracking():
    b = OrderBook()
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    b.apply_view(fake_view(A, {'refLo': 2, 'side': 0x42, 'shares': 200, 'price': 999000}))
    b.apply_view(fake_view(A, {'refLo': 3, 'side': 0x42, 'shares': 300, 'price': 998000}))
    b.apply_view(fake_view(A, {'refLo': 4, 'side': 0x53, 'shares': 400, 'price': 1001000}))
    assert b.live_orders == 4
    assert b.best_bid() == 999000
    assert b.best_ask() == 1001000
    bids = list(range(10))
    n = b.top_levels_of(0, bids)
    assert n == 2 and bids[0] == 999000 and bids[1] == 998000
    assert b.buy.agg_size[999000 - b.base_tick] == 300
    assert b.buy.order_count[999000 - b.base_tick] == 2


def test_partial_then_full_execute_frees_slot():
    b = OrderBook(pool_capacity=4)
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    b.apply_view(fake_view(E, {'refLo': 1, 'shares': 40, 'match': 7}))
    assert b.live_orders == 1
    assert b.trade_count == 1 and b.last_match == 7
    assert b.buy.agg_size[999000 - b.base_tick] == 60
    assert b.size[0] == 60
    b.apply_view(fake_view(E, {'refLo': 1, 'shares': 60, 'match': 8}))
    assert b.live_orders == 0
    assert b.best_bid() == 0  # side empty -> best resets
    assert b.trade_count == 2


def test_cancel_delete_and_free_ring_reuse():
    b = OrderBook(pool_capacity=2)
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    b.apply_view(fake_view(A, {'refLo': 2, 'side': 0x53, 'shares': 100, 'price': 1000000}))
    b.apply_view(fake_view(X, {'refLo': 1, 'shares': 30}))
    assert b.buy.agg_size[999000 - b.base_tick] == 70
    b.apply_view(fake_view(D, {'refLo': 1}))
    assert b.live_orders == 1 and b.deletes == 1
    b.apply_view(fake_view(A, {'refLo': 3, 'side': 0x42, 'shares': 50, 'price': 997000}))
    assert b.live_orders == 2
    assert b.pool_capacity == 2  # never grew


def test_replace_resets_time_priority():
    b = OrderBook()
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    b.apply_view(fake_view(U, {'refLo': 1, 'ref2Lo': 2, 'shares': 150, 'price': 999500}))
    assert b.live_orders == 1 and b.replaces == 1
    assert b.best_bid() == 999500
    assert b.buy.agg_size[999500 - b.base_tick] == 150
    assert b.buy.order_count[999500 - b.base_tick] == 1
    assert b.buy.order_count[999000 - b.base_tick] == 0


def test_reject_paths_counted_never_thrown():
    b = OrderBook(pool_capacity=2)
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    b.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    assert b.rejects[E_DUP_REF] == 1
    b.apply_view(fake_view(E, {'refLo': 99, 'shares': 10, 'match': 1}))
    b.apply_view(fake_view(X, {'refLo': 99, 'shares': 10}))
    b.apply_view(fake_view(D, {'refLo': 99}))
    b.apply_view(fake_view(U, {'refLo': 99, 'ref2Lo': 100, 'shares': 10, 'price': 999000}))
    assert b.rejects[E_UNKNOWN_REF] == 4
    b.apply_view(fake_view(A, {'refLo': 5, 'side': 0x42, 'shares': 10, 'price': 100}))
    assert b.rejects[E_PRICE_RANGE] == 1
    b.apply_view(fake_view(E, {'refLo': 1, 'shares': 1000, 'match': 2}))
    assert b.rejects[E_BAD_SIZE] == 1
    assert b.live_orders == 0


def test_pool_exhaustion_exact():
    c = OrderBook(pool_capacity=1)
    c.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 10, 'price': 999000}))
    c.apply_view(fake_view(A, {'refLo': 2, 'side': 0x42, 'shares': 10, 'price': 999000}))
    assert c.rejects[E_POOL_FULL] == 1
    assert c.total_rejects() == 1


def test_u64_refs_exact_through_hash():
    b = OrderBook()
    hi, lo = 0x00C0FFEE, 0xDEADBEEF
    ref = hi * 4294967296 + lo
    b.apply_view(fake_view(A, {'refHi': hi, 'refLo': lo, 'side': 0x42,
                               'shares': 100, 'price': 999000}))
    assert b.live_orders == 1
    b.apply_view(fake_view(E, {'refHi': hi, 'refLo': lo, 'shares': 100, 'match': 1}))
    assert b.live_orders == 0
    assert b.rejects[E_UNKNOWN_REF] == 0


def test_trades_never_rest_on_book():
    b = OrderBook()
    b.apply_view(fake_view(P, {'refLo': 1, 'side': 0x53, 'shares': 500,
                               'price': 1000000, 'match': 55}))
    assert b.live_orders == 0
    assert b.trade_count == 1
    assert b.last_exec_price == 1000000
    assert b.last_match == 55


def test_many_round_trips_never_degenerate(hash_stress=None):
    """Insert/remove 50k orders cyclically — hash must never degenerate
    (backward-shift deletion keeps occupancy == live count)."""
    import time
    b = OrderBook(pool_capacity=1024)
    t0 = time.perf_counter()
    ref = 1
    for cycle in range(50):
        for _ in range(512):
            b.apply_view(fake_view(A, {'refLo': ref & 0xFFFFFFFF,
                                       'refHi': (ref >> 32) & 0xFFFFFFFF,
                                       'side': 0x42, 'shares': 10,
                                       'price': 999000 + (ref % 100)}))
            ref += 1
        for _ in range(512):
            ref -= 1
            b.apply_view(fake_view(D, {'refLo': ref & 0xFFFFFFFF,
                                       'refHi': (ref >> 32) & 0xFFFFFFFF}))
    elapsed = time.perf_counter() - t0
    assert b.live_orders == 0
    assert elapsed < 2.0, f'50k cyclic ops took {elapsed:.2f}s — degenerate?'
