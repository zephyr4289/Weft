# telemetry.py — zero-permanent-allocation market telemetry (Python side).
#
# Mirrors packages/fintech/src/telemetry.js: EWMA rate/latency + 32-bucket
# log2 histogram, all state in preallocated array('d')/array('I') slots.

from array import array
from math import frexp

HIST_BUCKETS = 32


def _log2_floor(ns):
    if ns <= 0:
        return 0
    _, e = frexp(ns)
    return e - 1


class MarketTelemetry:
    __slots__ = ('slots', 'hist', 'hist_total')

    def __init__(self):
        # [0] msgs_total [1] bytes_total [2] rate_ewma [3] lat_ewma_ns
        # [4] last_ts [5] reserved [6] window_start [7] reserved
        self.slots = array('d', bytes(8 * 8))
        self.hist = array('I', bytes(4 * HIST_BUCKETS))
        self.hist_total = 0

    @property
    def msgs_total(self):
        return self.slots[0]

    @property
    def rate_ewma(self):
        return self.slots[2]

    @property
    def lat_ewma_ns(self):
        return self.slots[3]

    def record(self, nbytes, now_ns):
        s = self.slots
        s[0] += 1
        s[1] += nbytes
        dt = now_ns - s[6]
        if dt > 0:
            s[2] += (1e9 / dt - s[2]) * 0.001
        s[6] = now_ns

    def record_latency(self, ns):
        b = _log2_floor(ns)
        if b < 0:
            b = 0
        elif b >= HIST_BUCKETS:
            b = HIST_BUCKETS - 1
        self.hist[b] += 1
        self.hist_total += 1
        self.slots[3] += (ns - self.slots[3]) * 0.001

    def percentiles(self):
        out = [0, 0, 0]
        if self.hist_total == 0:
            return out
        acc = 0
        p50 = -1
        p99 = -1
        mx = -1
        for b in range(HIST_BUCKETS):
            acc += self.hist[b]
            if acc >= self.hist_total * 0.5 and p50 < 0:
                p50 = b
            if acc >= self.hist_total * 0.99 and p99 < 0:
                p99 = b
            if self.hist[b] > 0:
                mx = b
        out[0] = 2 ** p50 if p50 >= 0 else 0
        out[1] = 2 ** p99 if p99 >= 0 else 0
        out[2] = 2 ** (mx + 1) - 1 if mx >= 0 else 0
        return out
