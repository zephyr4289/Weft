// book.test.mjs — O(1) order book semantics: pools, hash, ladders, rejects.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  OrderBook, E_DUP_REF, E_UNKNOWN_REF, E_POOL_FULL, E_PRICE_RANGE, E_BAD_SIZE,
} from '../src/book.js';

function book(opts = {}) {
  return new OrderBook({ baseTick: 900_000, tickCount: 262_144, ...opts });
}

function fakeView(type, fields) {
  return {
    type,
    ts: fields.ts ?? 1,
    refHi: fields.refHi ?? 0,
    refLo: fields.refLo ?? 0,
    ref2Hi: fields.ref2Hi ?? 0,
    ref2Lo: fields.ref2Lo ?? 0,
    side: fields.side ?? 0x42,
    shares: fields.shares ?? 0,
    price: fields.price ?? 0,
    match: fields.match ?? 0,
    locate: 7777,
  };
}

const A = 0x41, E = 0x45, X = 0x58, D = 0x44, U = 0x55, P = 0x50;

test('aggregation: levels accumulate size + order count; best tracks extremes', () => {
  const b = book();
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 999000 }));
  b.applyView(fakeView(A, { refLo: 2, side: 0x42, shares: 200, price: 999000 }));
  b.applyView(fakeView(A, { refLo: 3, side: 0x42, shares: 300, price: 998000 }));
  b.applyView(fakeView(A, { refLo: 4, side: 0x53, shares: 400, price: 1001000 }));
  assert.equal(b.liveOrders, 4);
  assert.equal(b.bestBid(), 999000);
  assert.equal(b.bestAsk(), 1001000);
  const bids = new Int32Array(10);
  const n = b.topLevelsOf(0, bids);
  assert.equal(n, 2);
  assert.equal(bids[0], 999000);
  assert.equal(bids[1], 998000);
  const buy = b.buy;
  assert.equal(buy.aggSize[999000 - b.baseTick], 300);
  assert.equal(buy.orderCount[999000 - b.baseTick], 2);
});

test('partial execute keeps level; full execute frees slot back to the ring', () => {
  const b = book({ poolCapacity: 4 });
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 999000 }));
  b.applyView(fakeView(E, { refLo: 1, shares: 40, match: 7 }));
  assert.equal(b.liveOrders, 1);
  assert.equal(b.tradeCount, 1);
  assert.equal(b.lastMatch, 7);
  assert.equal(b.size[0], 60); // slot 0: remaining shares after partial execute
  assert.equal(b.buy.aggSize[999000 - b.baseTick], 60);
  b.applyView(fakeView(E, { refLo: 1, shares: 60, match: 8 }));
  assert.equal(b.liveOrders, 0);
  assert.equal(b.bestBid(), 0); // side empty -> best resets
  assert.equal(b.tradeCount, 2);
});

test('cancel reduces; delete removes; both O(1) with free-ring reuse', () => {
  const b = book({ poolCapacity: 2 });
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 999000 }));
  b.applyView(fakeView(A, { refLo: 2, side: 0x53, shares: 100, price: 1000000 }));
  b.applyView(fakeView(X, { refLo: 1, shares: 30 }));
  assert.equal(b.buy.aggSize[999000 - b.baseTick], 70);
  b.applyView(fakeView(D, { refLo: 1 }));
  assert.equal(b.liveOrders, 1);
  assert.equal(b.deletes, 1);
  // pool reuse: free ring handed back slot 0, now reused for a new order
  b.applyView(fakeView(A, { refLo: 3, side: 0x42, shares: 50, price: 997000 }));
  assert.equal(b.liveOrders, 2);
  assert.equal(b.poolCapacity, 2); // never grew
});

test('replace moves the order and resets time priority (ITCH U semantics)', () => {
  const b = book();
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 999000 }));
  b.applyView(fakeView(U, { refLo: 1, ref2Lo: 2, shares: 150, price: 999500 }));
  assert.equal(b.liveOrders, 1);
  assert.equal(b.replaces, 1);
  assert.equal(b.bestBid(), 999500);
  const buy = b.buy;
  assert.equal(buy.aggSize[999500 - b.baseTick], 150);
  assert.equal(buy.orderCount[999500 - b.baseTick], 1);
  assert.equal(buy.orderCount[999000 - b.baseTick], 0); // original gone
});

test('reject paths are counted, never thrown, and leave state consistent', () => {
  const b = book({ poolCapacity: 2 });
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 999000 }));
  // duplicate ref
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 999000 }));
  assert.equal(b.rejects[E_DUP_REF], 1);
  // unknown refs
  b.applyView(fakeView(E, { refLo: 99, shares: 10, match: 1 }));
  b.applyView(fakeView(X, { refLo: 99, shares: 10 }));
  b.applyView(fakeView(D, { refLo: 99 }));
  b.applyView(fakeView(U, { refLo: 99, ref2Lo: 100, shares: 10, price: 999000 }));
  assert.equal(b.rejects[E_UNKNOWN_REF], 4);
  // out-of-band price
  b.applyView(fakeView(A, { refLo: 5, side: 0x42, shares: 10, price: 100 }));
  assert.equal(b.rejects[E_PRICE_RANGE], 1);
  // oversize execute is clamped and flagged
  b.applyView(fakeView(E, { refLo: 1, shares: 1000, match: 2 }));
  assert.equal(b.rejects[E_BAD_SIZE], 1);
  assert.equal(b.liveOrders, 0); // clamped to full size -> fully executed
});

test('pool exhaustion counting is exact', () => {
  const c = book({ poolCapacity: 1 });
  c.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 10, price: 999000 }));
  c.applyView(fakeView(A, { refLo: 2, side: 0x42, shares: 10, price: 999000 }));
  assert.equal(c.rejects[E_POOL_FULL], 1);
  assert.equal(c.totalRejects(), 1);
});

test('u64 refs beyond 2^53 are exact through lo/hi pairs (no double materialization)', () => {
  const b = book();
  const hi = 0x00c0ffee, lo = 0xdeadbeef;
  b.applyView(fakeView(A, { refHi: hi, refLo: lo, side: 0x42, shares: 100, price: 999000 }));
  assert.equal(b.liveOrders, 1);
  // execute by the same pair — hash must find it
  b.applyView(fakeView(E, { refHi: hi, refLo: lo, shares: 100, match: 1 }));
  assert.equal(b.liveOrders, 0);
  assert.equal(b.rejects[E_UNKNOWN_REF], 0);
});

test('trade messages never rest on the book', () => {
  const b = book();
  b.applyView(fakeView(P, { refLo: 1, side: 0x53, shares: 500, price: 1000000, match: 55 }));
  assert.equal(b.liveOrders, 0);
  assert.equal(b.tradeCount, 1);
  assert.equal(b.lastExecPrice, 1000000);
  assert.equal(b.lastMatch, 55);
});

test('best prices converge when both sides rest at the same tick', () => {
  const b = book();
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 10, price: 1000000 }));
  b.applyView(fakeView(A, { refLo: 2, side: 0x53, shares: 10, price: 1000000 }));
  assert.equal(b.bestBid(), 1000000);
  assert.equal(b.bestAsk(), 1000000);
});
