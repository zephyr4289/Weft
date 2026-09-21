# __init__.py — weft_fintech: zero-copy ITCH 5.0/SBE market-data engine,
# O(1) ring-pool L2/L3 book, MDP1 snapshot wire, zero-copy numpy/polars
# interop (Pillar 6 deliverable A, Python side).

from .itch import (ItchEngine, ItchView, frame_offsets, E_TRUNC, T_SYSTEM,
                   T_ADD, T_ADD_MPID, T_EXEC, T_EXEC_PRICE, T_CANCEL,
                   T_DELETE, T_REPLACE, T_TRADE)
from .book import (OrderBook, OK, E_DUP_REF, E_UNKNOWN_REF, E_POOL_FULL,
                   E_PRICE_RANGE, E_BAD_SIZE)
from .mdp1 import (pack_snapshot, Mdp1View, mdp1_crc32, parity_of,
                   MDP1_SIZE, MDP1_TOP_LEVELS, F_BOOK_VALID, F_CROSSED,
                   F_LOCKED)
from .sbe import SbeDecoder
from .telemetry import MarketTelemetry
from . import buffers

__version__ = '0.1.0'

__all__ = [
    'ItchEngine', 'ItchView', 'frame_offsets', 'E_TRUNC',
    'T_SYSTEM', 'T_ADD', 'T_ADD_MPID', 'T_EXEC', 'T_EXEC_PRICE',
    'T_CANCEL', 'T_DELETE', 'T_REPLACE', 'T_TRADE',
    'OrderBook', 'OK', 'E_DUP_REF', 'E_UNKNOWN_REF', 'E_POOL_FULL',
    'E_PRICE_RANGE', 'E_BAD_SIZE',
    'pack_snapshot', 'Mdp1View', 'mdp1_crc32', 'parity_of',
    'MDP1_SIZE', 'MDP1_TOP_LEVELS', 'F_BOOK_VALID', 'F_CROSSED', 'F_LOCKED',
    'SbeDecoder', 'MarketTelemetry', 'buffers',
]
