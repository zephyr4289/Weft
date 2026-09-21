# test_parity.py — TS == Python bit-for-bit parity (Stage 5 core evidence).
#
# The frozen manifest was produced by the TS engine over the golden stream.
# Python must independently reproduce the same MDP1 checkpoint bytes.

from array import array
from hashlib import sha256

from weft_fintech.book import OrderBook
from weft_fintech.itch import ItchEngine, frame_offsets
from weft_fintech.mdp1 import pack_snapshot, parity_of, MDP1_SIZE, MDP1_TOP_LEVELS

CHECKPOINTS = [512, 1024, 1536, 2048]


def run_checkpoints(stream):
    book = OrderBook()
    engine = ItchEngine(book)
    offs = frame_offsets(stream)
    snaps = []
    sb = array('i', bytes(4 * MDP1_TOP_LEVELS))
    sa = array('i', bytes(4 * MDP1_TOP_LEVELS))
    for cp in CHECKPOINTS:
        end = offs[cp] if cp < len(offs) else len(stream)
        engine.process(stream, end)
        assert book.msgs_applied == cp
        snap = bytearray(MDP1_SIZE)
        pack_snapshot(book, snap, sb, sa)
        snaps.append(bytes(snap))
    return book, snaps


def test_python_reproduces_frozen_ts_checkpoint_hashes(golden_stream, manifest):
    _, snaps = run_checkpoints(golden_stream)
    for cp, snap in zip(CHECKPOINTS, snaps):
        h = sha256(snap).hexdigest()
        assert h == manifest['hashes'][f'mdp1-ckpt-{cp}.bin'], f'checkpoint {cp}'


def test_python_parity_hash_matches_manifest(golden_stream, manifest):
    _, snaps = run_checkpoints(golden_stream)
    assert parity_of(snaps) == manifest['parity_hash']


def test_python_final_state_matches_manifest(golden_stream, manifest):
    book, _ = run_checkpoints(golden_stream)
    f = manifest['final']
    assert book.msgs_applied == f['msgsApplied']
    assert book.skipped == f['skipped']
    assert book.live_orders == f['liveOrders']
    assert book.trade_count == f['tradeCount']
    assert book.last_match == f['lastMatch']
    assert book.last_ts == f['lastTs']
    assert book.best_bid() == f['bestBid']
    assert book.best_ask() == f['bestAsk']
    assert book.total_rejects() == f['rejectsTotal']


def test_python_engine_deterministic_double_run(golden_stream):
    _, a = run_checkpoints(golden_stream)
    _, b = run_checkpoints(golden_stream)
    assert a == b


def test_sbe_golden_stream_decodes(schema=None):
    import json
    from weft_fintech.sbe import SbeDecoder
    schema = json.loads((FIXTURES_DIR / 'sbe-schema.json').read_text())
    stream = (FIXTURES_DIR / 'sbe-stream.bin').read_bytes()
    dec = SbeDecoder(schema)
    seen = []
    n = dec.process(stream, len(stream), lambda t, vec, cnt: seen.append(t))
    assert n == 4096
    assert dec.truncated == 0 and dec.skipped == 0
    assert sum(1 for t in seen if t == 1002) == 512


FIXTURES_DIR = None  # set by conftest import side effect below


def _init_fixtures():
    global FIXTURES_DIR
    import conftest
    FIXTURES_DIR = conftest.FIXTURES


_init_fixtures()
