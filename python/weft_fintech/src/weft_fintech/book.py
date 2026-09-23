# book.py — O(1) L2/L3 order book, ring-allocated pools (managed Python side).
#
# Mirrors packages/fintech/src/book.js exactly:
#   - SoA slot pool over array('I')/array('i')/array('B')/array('H') with a
#     LIFO free ring (O(1) alloc/free, deterministic order)
#   - open-addressed ref -> slot hash over u64 keys in array('Q'), SAME
#     Fibonacci mix as TS: (lo * 0x9E3779B1 ^ hi * 0x85EBCA77) & mask
#   - BACKWARD-SHIFT deletion (no tombstones — the TS livelock bug is fixed
#     identically here), occupancy always == live count
#   - direct-mapped tick ladders, O(1) best tracking
#   - counted rejects, never exceptions (Law 4 / W4-04)
#   - ITCH 'U' replace removes the original and appends at level BACK
#
# Steady state performs no heap-object allocation: all per-message state
# mutations are typed-array slot writes; refcount recycles the view's
# temporary tuples within each iteration.
#
# Zero-residue contract (Stage 3 gate: EXACT 0 bytes of permanent heap
# growth over 1,000,000 messages): every mutating counter lives in ONE
# preallocated array('q') block (self.stats) instead of Python int
# attributes — a boxed int attribute would leave the FINAL value as a live
# heap block at measurement time. Public counters are read-only properties
# over the same storage; API surface unchanged.

from array import array

OK = 0
E_DUP_REF = 1
E_UNKNOWN_REF = 2
E_POOL_FULL = 3
E_PRICE_RANGE = 4
E_BAD_SIZE = 5
E_REJECT_CODES = 6

SIDE_BUY = 0
SIDE_SELL = 1

MASK32 = 0xFFFFFFFF

GOLDEN = 0x9E3779B1
GOLDEN2 = 0x85EBCA77

# stats array('q') slots — all book counters, unboxed (Law 1: no permanent
# heap growth; signed 64-bit covers u48 timestamps and -1 sentinels).
S_MSGS = 0
S_SKIPPED = 1
S_LIVE = 2
S_LAST_TS = 3
S_LAST_MATCH = 4
S_LAST_EXEC = 5
S_TRADES = 6
S_ADDS = 7
S_EXECUTES = 8
S_CANCELS = 9
S_DELETES = 10
S_REPLACES = 11
S_FREE_TOP = 12
S_BEST_BID = 13
S_BEST_ASK = 14
S_COUNT = 15


def _pow2ceil(n):
    c = 1
    while c < n:
        c <<= 1
    return c


class _RefHash:
    __slots__ = ('mask', 'key', 'val', 'count')

    def __init__(self, capacity):
        self.mask = capacity - 1
        self.key = array('Q', bytes(8 * capacity))
        self.val = array('i', bytes(4 * capacity))
        for i in range(capacity):
            self.val[i] = -1
        self.count = array('q', bytes(8))  # unboxed: no boxed-int residue

    def hash(self, lo, hi):
        return ((lo * GOLDEN & MASK32) ^ (hi * GOLDEN2 & MASK32)) & self.mask

    def find(self, ref, lo, hi):
        i = self.hash(lo, hi)
        key = self.key
        val = self.val
        mask = self.mask
        while True:
            v = val[i]
            if v == -1:
                return -1
            if key[i] == ref:
                return v
            i = (i + 1) & mask

    def insert(self, ref, lo, hi, slot):
        i = self.hash(lo, hi)
        key = self.key
        val = self.val
        mask = self.mask
        while True:
            v = val[i]
            if v == -1:
                key[i] = ref
                val[i] = slot
                self.count[0] += 1
                return
            if key[i] == ref:
                val[i] = slot  # refresh (defensive; callers pre-check dups)
                return
            i = (i + 1) & mask

    def remove(self, ref, lo, hi):
        i = self.hash(lo, hi)
        key = self.key
        val = self.val
        mask = self.mask
        while True:
            v = val[i]
            if v == -1:
                return False
            if key[i] == ref:
                break
            i = (i + 1) & mask
        # backward-shift deletion: no tombstones, ever
        val[i] = -1
        self.count[0] -= 1
        j = i
        while True:
            j = (j + 1) & mask
            v = val[j]
            if v == -1:
                return True
            k = self.hash(key[j] & MASK32, key[j] >> 32)
            d = (j - i) & mask
            dk = (k - i) & mask
            if dk != 0 and dk <= d:
                continue
            val[i] = v
            key[i] = key[j]
            val[j] = -1
            i = j


class _Ladder:
    __slots__ = ('base_tick', 'tick_count', 'agg_size', 'order_count',
                 'head', 'tail')

    def __init__(self, base_tick, tick_count):
        self.base_tick = base_tick
        self.tick_count = tick_count
        self.agg_size = array('I', bytes(4 * tick_count))
        self.order_count = array('I', bytes(4 * tick_count))
        self.head = array('i', bytes(4 * tick_count))
        self.tail = array('i', bytes(4 * tick_count))
        for i in range(tick_count):
            self.head[i] = -1
            self.tail[i] = -1


class OrderBook:
    def __init__(self, pool_capacity=8192, base_tick=900_000,
                 tick_count=262_144, top_levels=10):
        self.pool_capacity = pool_capacity
        self.base_tick = base_tick
        self.tick_count = tick_count
        self.top_levels = top_levels

        n = pool_capacity
        self.ref = array('Q', bytes(8 * n))
        self.price = array('I', bytes(4 * n))
        self.size = array('I', bytes(4 * n))
        self.side = bytearray(n)
        self.locate = array('H', bytes(2 * n))
        self.prev = array('i', bytes(4 * n))
        self.next = array('i', bytes(4 * n))
        self.lvl_idx = array('i', bytes(4 * n))
        self.free = array('i', bytes(4 * n))
        for i in range(n):
            self.free[i] = n - 1 - i
        self.stats = array('q', bytes(8 * S_COUNT))
        self.stats[S_FREE_TOP] = n

        self.hash = _RefHash(_pow2ceil(pool_capacity * 2))

        self.buy = _Ladder(base_tick, tick_count)
        self.sell = _Ladder(base_tick, tick_count)
        self.stats[S_BEST_BID] = -1
        self.stats[S_BEST_ASK] = -1

        self.rejects = array('I', bytes(4 * E_REJECT_CODES))

    # -- level bookkeeping --------------------------------------------------
    def _level_append(self, ladder, idx, slot):
        old_tail = ladder.tail[idx]
        if old_tail >= 0:
            self.next[old_tail] = slot
            self.prev[slot] = old_tail
        else:
            ladder.head[idx] = slot
            self.prev[slot] = -1
        self.next[slot] = -1
        ladder.tail[idx] = slot
        ladder.agg_size[idx] += self.size[slot]
        ladder.order_count[idx] += 1
        # (stats untouched here)

    def _level_remove(self, ladder, idx, slot):
        ladder.agg_size[idx] -= self.size[slot]
        ladder.order_count[idx] -= 1
        p = self.prev[slot]
        n = self.next[slot]
        if p >= 0:
            self.next[p] = n
        else:
            ladder.head[idx] = n
        if n >= 0:
            self.prev[n] = p
        else:
            ladder.tail[idx] = p

    def _best_after_removal(self, side, tick):
        if side == SIDE_BUY:
            if tick != self.stats[S_BEST_BID]:
                return
            base = self.base_tick
            t = tick
            oc = self.buy.order_count
            while t >= base and oc[t - base] == 0:
                t -= 1
            self.stats[S_BEST_BID] = t if t >= base else -1
        else:
            if tick != self.stats[S_BEST_ASK]:
                return
            base = self.base_tick
            top = base + self.tick_count
            t = tick
            oc = self.sell.order_count
            while t < top and oc[t - base] == 0:
                t += 1
            self.stats[S_BEST_ASK] = t if t < top else -1

    def _slot_idx_for_tick(self, tick):
        idx = tick - self.base_tick
        if idx < 0 or idx >= self.tick_count:
            return -1
        return idx

    # -- operations (all O(1)) ------------------------------------------------
    def _apply_add(self, ref, side_byte, shares, price, locate, ts):
        side = SIDE_BUY if side_byte == 0x42 else SIDE_SELL
        if self.hash.find(ref, ref & MASK32, ref >> 32) >= 0:
            self.rejects[E_DUP_REF] += 1
            return
        idx = self._slot_idx_for_tick(price)
        if idx < 0:
            self.rejects[E_PRICE_RANGE] += 1
            return
        ft = self.stats[S_FREE_TOP]
        if ft == 0:
            self.rejects[E_POOL_FULL] += 1
            return
        ft -= 1
        self.stats[S_FREE_TOP] = ft
        slot = self.free[ft]
        self.ref[slot] = ref
        self.price[slot] = price
        self.size[slot] = shares
        self.side[slot] = side
        self.locate[slot] = locate
        ladder = self.buy if side == SIDE_BUY else self.sell
        self.lvl_idx[slot] = idx
        self._level_append(ladder, idx, slot)
        if side == SIDE_BUY:
            if price > self.stats[S_BEST_BID]:
                self.stats[S_BEST_BID] = price
        elif self.stats[S_BEST_ASK] < 0 or price < self.stats[S_BEST_ASK]:
            self.stats[S_BEST_ASK] = price
        self.hash.insert(ref, ref & MASK32, ref >> 32, slot)
        self.stats[S_LIVE] += 1
        self.stats[S_ADDS] += 1
        if ts > 0:
            self.stats[S_LAST_TS] = ts

    def _reduce(self, slot, shares, is_cancel, match, price):
        if shares > self.size[slot]:
            self.rejects[E_BAD_SIZE] += 1
            shares = self.size[slot]
        side = self.side[slot]
        ladder = self.buy if side == SIDE_BUY else self.sell
        idx = self.lvl_idx[slot]
        self.size[slot] -= shares
        ladder.agg_size[idx] -= shares
        if self.size[slot] == 0:
            tick = self.price[slot]
            self._level_remove(ladder, idx, slot)
            self.hash.remove(self.ref[slot], self.ref[slot] & MASK32, self.ref[slot] >> 32)
            self.free[self.stats[S_FREE_TOP]] = slot
            self.stats[S_FREE_TOP] += 1
            self.stats[S_LIVE] -= 1
            self._best_after_removal(side, tick)
        if not is_cancel:
            self.stats[S_TRADES] += 1
            if match > 0:
                self.stats[S_LAST_MATCH] = match
            if price > 0:
                self.stats[S_LAST_EXEC] = price

    def _apply_execute(self, ref, shares, match, price):
        slot = self.hash.find(ref, ref & MASK32, ref >> 32)
        if slot < 0:
            self.rejects[E_UNKNOWN_REF] += 1
            return
        self._reduce(slot, shares, False, match, price)
        self.stats[S_EXECUTES] += 1

    def _apply_cancel(self, ref, shares):
        slot = self.hash.find(ref, ref & MASK32, ref >> 32)
        if slot < 0:
            self.rejects[E_UNKNOWN_REF] += 1
            return
        self._reduce(slot, shares, True, 0, 0)
        self.stats[S_CANCELS] += 1

    def _apply_delete(self, ref):
        slot = self.hash.find(ref, ref & MASK32, ref >> 32)
        if slot < 0:
            self.rejects[E_UNKNOWN_REF] += 1
            return
        side = self.side[slot]
        ladder = self.buy if side == SIDE_BUY else self.sell
        idx = self.lvl_idx[slot]
        tick = self.price[slot]
        self._level_remove(ladder, idx, slot)
        self.hash.remove(self.ref[slot], self.ref[slot] & MASK32, self.ref[slot] >> 32)
        self.free[self.stats[S_FREE_TOP]] = slot
        self.stats[S_FREE_TOP] += 1
        self.stats[S_LIVE] -= 1
        self.stats[S_DELETES] += 1
        self._best_after_removal(side, tick)

    def _apply_replace(self, o_ref, n_ref, shares, price, ts):
        orig = self.hash.find(o_ref, o_ref & MASK32, o_ref >> 32)
        if orig < 0:
            self.rejects[E_UNKNOWN_REF] += 1
            return
        if self.hash.find(n_ref, n_ref & MASK32, n_ref >> 32) >= 0:
            self.rejects[E_DUP_REF] += 1
            return
        idx = self._slot_idx_for_tick(price)
        if idx < 0:
            self.rejects[E_PRICE_RANGE] += 1
            return
        side = self.side[orig]
        locate = self.locate[orig]
        ladder = self.buy if side == SIDE_BUY else self.sell
        o_idx = self.lvl_idx[orig]
        o_tick = self.price[orig]
        self._level_remove(ladder, o_idx, orig)
        self.hash.remove(o_ref, o_ref & MASK32, o_ref >> 32)
        self.free[self.stats[S_FREE_TOP]] = orig
        self.stats[S_FREE_TOP] += 1
        self.stats[S_LIVE] -= 1
        self._best_after_removal(side, o_tick)
        ft = self.stats[S_FREE_TOP]
        if ft == 0:
            self.rejects[E_POOL_FULL] += 1
            return
        ft -= 1
        self.stats[S_FREE_TOP] = ft
        slot = self.free[ft]
        self.ref[slot] = n_ref
        self.price[slot] = price
        self.size[slot] = shares
        self.side[slot] = side
        self.locate[slot] = locate
        self.lvl_idx[slot] = idx
        self._level_append(ladder, idx, slot)
        if side == SIDE_BUY:
            if price > self.stats[S_BEST_BID]:
                self.stats[S_BEST_BID] = price
        elif self.stats[S_BEST_ASK] < 0 or price < self.stats[S_BEST_ASK]:
            self.stats[S_BEST_ASK] = price
        self.hash.insert(n_ref, n_ref & MASK32, n_ref >> 32, slot)
        self.stats[S_LIVE] += 1
        self.stats[S_REPLACES] += 1
        if ts > 0:
            self.stats[S_LAST_TS] = ts

    # -- dispatch ------------------------------------------------------------
    def apply_view(self, view):
        t = view.type
        if t == 0x41 or t == 0x46:  # A / F
            self._apply_add(view.ref, view.side, view.shares, view.price, view.locate, view.ts)
        elif t == 0x45:  # E
            self._apply_execute(view.ref, view.shares, view.match, 0)
        elif t == 0x43:  # C
            self._apply_execute(view.ref, view.shares, view.match, view.price)
        elif t == 0x58:  # X
            self._apply_cancel(view.ref, view.shares)
        elif t == 0x44:  # D
            self._apply_delete(view.ref)
        elif t == 0x55:  # U
            self._apply_replace(view.ref, view.ref2, view.shares, view.price, view.ts)
        elif t == 0x50:  # P
            self.stats[S_TRADES] += 1
            self.stats[S_LAST_MATCH] = view.match
            self.stats[S_LAST_EXEC] = view.price
            self.stats[S_MSGS] += 1
            return True
        elif t == 0x53:  # S
            self.stats[S_MSGS] += 1
            if view.ts > self.stats[S_LAST_TS]:
                self.stats[S_LAST_TS] = view.ts
            return True
        else:
            self.stats[S_SKIPPED] += 1
            return False
        self.stats[S_MSGS] += 1
        return True

    # -- introspection ---------------------------------------------------------
    # Public counters are READ-ONLY properties over the unboxed stats block:
    # identical API surface to the TS book, zero boxed-int residue (Law 1).
    @property
    def msgs_applied(self):
        return self.stats[S_MSGS]

    @property
    def skipped(self):
        return self.stats[S_SKIPPED]

    @property
    def live_orders(self):
        return self.stats[S_LIVE]

    @property
    def last_ts(self):
        return self.stats[S_LAST_TS]

    @property
    def last_match(self):
        return self.stats[S_LAST_MATCH]

    @property
    def last_exec_price(self):
        return self.stats[S_LAST_EXEC]

    @property
    def trade_count(self):
        return self.stats[S_TRADES]

    @property
    def adds(self):
        return self.stats[S_ADDS]

    @property
    def executes(self):
        return self.stats[S_EXECUTES]

    @property
    def cancels(self):
        return self.stats[S_CANCELS]

    @property
    def deletes(self):
        return self.stats[S_DELETES]

    @property
    def replaces(self):
        return self.stats[S_REPLACES]

    @property
    def best_bid_tick(self):
        return self.stats[S_BEST_BID]

    @property
    def best_ask_tick(self):
        return self.stats[S_BEST_ASK]

    @property
    def free_top(self):
        return self.stats[S_FREE_TOP]

    def best_bid(self):
        return self.stats[S_BEST_BID] if self.stats[S_BEST_BID] >= 0 else 0

    def best_ask(self):
        return self.stats[S_BEST_ASK] if self.stats[S_BEST_ASK] >= 0 else 0

    def total_rejects(self):
        return sum(self.rejects)

    def top_levels_of(self, side, out):
        ladder = self.buy if side == SIDE_BUY else self.sell
        t = self.stats[S_BEST_BID] if side == SIDE_BUY else self.stats[S_BEST_ASK]
        step = -1 if side == SIDE_BUY else 1
        base = self.base_tick
        top = base + self.tick_count
        count = ladder.order_count
        n = 0
        need = len(out)
        while base <= t < top and n < need:
            if count[t - base] > 0:
                out[n] = t
                n += 1
            t += step
        return n
