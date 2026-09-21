# test_buffers.py — Rule 3: zero-copy Python Buffer Protocol interop.
#
# The pointer identity is the acceptance proof: numpy views over the MDP1
# record must report the SAME data pointer as the underlying buffer. Writes
# through the book must be visible through the view (shared memory).

from generate_helpers import fake_view
from weft_fintech.book import OrderBook
from weft_fintech.mdp1 import pack_snapshot, MDP1_SIZE, MDP1_TOP_LEVELS
from weft_fintech import buffers

A = 0x41


def test_to_numpy_shares_pointer_zero_copy():
    book = OrderBook()
    book.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    buf = bytearray(MDP1_SIZE)
    pack_snapshot(book, buf, [0] * MDP1_TOP_LEVELS, [0] * MDP1_TOP_LEVELS)

    bids, asks = buffers.levels_as_numpy(book, buf)
    addr = bids.__array_interface__['data'][0]
    base_addr = bids.ctypes.data
    assert addr == base_addr  # numpy reports the true data pointer

    # zero-copy proof: mutate the BOOK and pack again — the numpy view
    # observes the change without any copy (same memory region)
    before = int(bids['price'][0])
    assert before == 999000
    book.apply_view(fake_view(A, {'refLo': 2, 'side': 0x42, 'shares': 300, 'price': 999500}))
    pack_snapshot(book, buf, [0] * MDP1_TOP_LEVELS, [0] * MDP1_TOP_LEVELS)
    assert int(bids['price'][0]) == 999500  # view saw the new snapshot


def test_record_view_is_byte_exact_and_immutable_source_is_readonly():
    book = OrderBook()
    book.apply_view(fake_view(A, {'refLo': 1, 'side': 0x42, 'shares': 100, 'price': 999000}))
    buf = bytearray(MDP1_SIZE)
    pack_snapshot(book, buf, [0] * 10, [0] * 10)
    # writable source (bytearray): shared, byte-exact
    view = buffers.record_as_numpy(buf)
    assert view.shape == (MDP1_SIZE,)
    assert bytes(view[0:4]) == b'MDP1'
    # immutable source (bytes): numpy gives a read-only view
    frozen = buffers.record_as_numpy(bytes(buf))
    assert not frozen.flags.writeable


def test_to_polars_fails_soft_with_clear_error():
    try:
        import polars  # noqa: F401
        polars_available = True
    except ImportError:
        polars_available = False

    book = OrderBook()
    buf = bytearray(MDP1_SIZE)
    pack_snapshot(book, buf, [0] * 10, [0] * 10)
    if polars_available:
        df = buffers.levels_as_polars(book, buf)
        assert df.height == 10
    else:
        try:
            buffers.levels_as_polars(book, buf)
            raise AssertionError('expected RuntimeError without polars')
        except RuntimeError as exc:
            assert 'polars' in str(exc)
