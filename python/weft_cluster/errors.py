# errors.py — WC error taxonomy, byte-frozen mirror of
# packages/weft-cluster/src/errors.js (docs/weft-cluster/WIRE-V1.md §5).
# Appending new codes is allowed; renumbering is NOT.

WC_OK = 0
WC_E_BAD_MAGIC = 1
WC_E_BAD_VERSION = 2
WC_E_TRUNCATED = 3
WC_E_BAD_HEADER = 4
WC_E_BAD_CRC = 5
WC_E_UNKNOWN_NODE = 6
WC_E_STALE_SEQ = 7
WC_E_TOPIC_UNROUTED = 8
WC_E_PARTITION = 9
WC_E_NODE_LOST = 10
WC_E_SHUTDOWN = 11
WC_E_SCHEMA_MISMATCH = 12

_NAMES = {
    WC_OK: "WC_OK",
    WC_E_BAD_MAGIC: "WC_E_BAD_MAGIC",
    WC_E_BAD_VERSION: "WC_E_BAD_VERSION",
    WC_E_TRUNCATED: "WC_E_TRUNCATED",
    WC_E_BAD_HEADER: "WC_E_BAD_HEADER",
    WC_E_BAD_CRC: "WC_E_BAD_CRC",
    WC_E_UNKNOWN_NODE: "WC_E_UNKNOWN_NODE",
    WC_E_STALE_SEQ: "WC_E_STALE_SEQ",
    WC_E_TOPIC_UNROUTED: "WC_E_TOPIC_UNROUTED",
    WC_E_PARTITION: "WC_E_PARTITION",
    WC_E_NODE_LOST: "WC_E_NODE_LOST",
    WC_E_SHUTDOWN: "WC_E_SHUTDOWN",
    WC_E_SCHEMA_MISMATCH: "WC_E_SCHEMA_MISMATCH",
}


def wc_name(code: int) -> str:
    """Stable name for a numeric code (e.g. 5 -> 'WC_E_BAD_CRC')."""
    return _NAMES.get(code, f"WC_E_UNKNOWN_{code}")


class WeftClusterError(Exception):
    """Error carrying a stable WC code; `code` is numeric and frozen."""

    def __init__(self, code: int, detail: str | None = None):
        self.code = code
        text = wc_name(code) if detail is None else f"{wc_name(code)}: {detail}"
        super().__init__(text)
