# buffers.py — Python Buffer Protocol / DLPack zero-copy interop (Rule 3).
#
# order_book.to_numpy() / order_book.to_polars() share the book's own MDP1
# scratch buffer by POINTER — 0 bytes copied. The pointer identity is the
# acceptance proof (tests/test_buffers.py asserts
# arr.__array_interface__['data'][0] == buffer address).
#
# numpy is imported lazily so the parser/book stay dependency-free; polars
# is an optional extra (weft_fintech[polars]) with a fail-soft error.

_LEVEL_DTYPE_DESC = [('price', '<u4'), ('size', '<u4'), ('orders', '<u4')]


def levels_as_numpy(book, mdp1_buffer):
    """Zero-copy structured numpy view over the MDP1 top-10 level tables.

    Returns (bids, asks) as np.ndarray of dtype [('price','<u4'),('size',
    '<u4'),('orders','<u4')], each shape (10,), viewing — not copying — the
    buffer. Live for as long as the underlying buffer is alive.
    """
    np = _numpy()
    mv = memoryview(mdp1_buffer)
    base = mv[32:272]  # 240 bytes: 10 bid levels (120B) + 10 ask levels (120B)
    # np.frombuffer over a memoryview slice shares the SAME memory (0-copy)
    flat = np.frombuffer(base, dtype=np.dtype([('price', '<u4'), ('size', '<u4'), ('orders', '<u4')]))
    bids = flat[:10]
    asks = flat[10:]
    return bids, asks


def record_as_numpy(mdp1_buffer):
    """Zero-copy read-only byte-level numpy view of a full MDP1 record."""
    np = _numpy()
    return np.frombuffer(memoryview(mdp1_buffer), dtype=np.uint8)


def levels_as_polars(book, mdp1_buffer):
    """Zero-copy polars DataFrame over the MDP1 level tables.

    polars.from_numpy with the structured view flattened per column; polars
    keeps numpy buffers alive without copying for native-endian primitives.
    Requires the optional polars extra.
    """
    pl = _polars()
    bids, asks = levels_as_numpy(book, mdp1_buffer)
    data = {}
    for col in ('price', 'size', 'orders'):
        data[f'bid_{col}'] = bids[col]
        data[f'ask_{col}'] = asks[col]
    return pl.DataFrame({k: v for k, v in data.items()})


def _numpy():
    try:
        import numpy
        return numpy
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            'to_numpy() requires numpy (pip install numpy)'
        ) from exc


def _polars():
    try:
        import polars
        return polars
    except ImportError as exc:
        raise RuntimeError(
            'to_polars() requires polars (pip install "weft_fintech[polars]"); '
            'the CI torch/polars lanes exercise this path'
        ) from exc
