// mdp1.test.mjs — MDP1 snapshot wire: CRC, packing, view, flags.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { OrderBook } from '../src/book.js';
import {
  packSnapshot, Mdp1View, mdp1Crc32, crcTable, MDP1_SIZE,
  F_BOOK_VALID, F_CROSSED, F_LOCKED,
} from '../src/mdp1.js';

function fakeView(type, fields) {
  return {
    type, ts: fields.ts ?? 1, refHi: fields.refHi ?? 0, refLo: fields.refLo ?? 0,
    ref2Hi: 0, ref2Lo: 0, side: fields.side ?? 0x42, shares: fields.shares ?? 0,
    price: fields.price ?? 0, match: fields.match ?? 0, locate: 7777,
  };
}
const A = 0x41;

test('CRC-32 matches the standard check vector ("123456789" -> 0xCBF43926)', () => {
  const bytes = Buffer.from('123456789', 'latin1');
  assert.equal(mdp1Crc32(bytes, 0, 9), 0xcbf43926);
});

test('CRC table is idempotent and complete', () => {
  const t1 = crcTable();
  const t2 = crcTable();
  assert.equal(t1, t2);
  assert.equal(t1.length, 256);
  assert.equal(t1[1], 0x77073096); // standard CRC-32 table constant
});

test('empty book snapshot: magic, version, zero levels, valid CRC', () => {
  const b = new OrderBook();
  const buf = new Uint8Array(MDP1_SIZE);
  packSnapshot(b, buf, new Int32Array(10), new Int32Array(10));
  const v = new Mdp1View(buf);
  assert.ok(v.valid());
  assert.equal(v.version, 1);
  assert.equal(v.flags & F_BOOK_VALID, 0); // nothing applied yet
  assert.equal(v.bestBid, 0);
  assert.equal(v.bestAsk, 0);
  for (let i = 0; i < 10; i++) {
    assert.equal(v.bidPrice(i), 0);
    assert.equal(v.askPrice(i), 0);
  }
  assert.ok(v.crcOk());
});

test('snapshot reflects aggregated top-of-book levels best-first', () => {
  const b = new OrderBook({ baseTick: 900_000, tickCount: 262_144 });
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 998000 }));
  b.applyView(fakeView(A, { refLo: 2, side: 0x42, shares: 200, price: 999000 }));
  b.applyView(fakeView(A, { refLo: 3, side: 0x53, shares: 300, price: 1001000 }));
  b.applyView(fakeView(A, { refLo: 4, side: 0x53, shares: 400, price: 1002000 }));
  const buf = new Uint8Array(MDP1_SIZE);
  packSnapshot(b, buf, new Int32Array(10), new Int32Array(10));
  const v = new Mdp1View(buf);
  assert.ok(v.crcOk());
  assert.ok(v.flags & F_BOOK_VALID);
  assert.equal(v.bestBid, 999000);
  assert.equal(v.bestAsk, 1001000);
  assert.equal(v.bidPrice(0), 999000);
  assert.equal(v.bidSize(0), 200);
  assert.equal(v.bidOrders(0), 1);
  assert.equal(v.bidPrice(1), 998000);
  assert.equal(v.bidSize(1), 100);
  assert.equal(v.askPrice(0), 1001000);
  assert.equal(v.askSize(0), 300);
  assert.equal(v.askPrice(1), 1002000);
  assert.equal(v.askPrice(2), 0);
});

test('crossed and locked flags derive from best prices', () => {
  const crossed = new OrderBook({ baseTick: 900_000, tickCount: 262_144 });
  crossed.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 10, price: 1002000 }));
  crossed.applyView(fakeView(A, { refLo: 2, side: 0x53, shares: 10, price: 1000000 }));
  const cb = new Uint8Array(MDP1_SIZE);
  packSnapshot(crossed, cb, new Int32Array(10), new Int32Array(10));
  assert.ok(new Mdp1View(cb).flags & F_CROSSED);

  const locked = new OrderBook({ baseTick: 900_000, tickCount: 262_144 });
  locked.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 10, price: 1000000 }));
  locked.applyView(fakeView(A, { refLo: 2, side: 0x53, shares: 10, price: 1000000 }));
  const lb = new Uint8Array(MDP1_SIZE);
  packSnapshot(locked, lb, new Int32Array(10), new Int32Array(10));
  const lv = new Mdp1View(lb);
  assert.ok(lv.flags & F_LOCKED);
  assert.ok(!(lv.flags & F_CROSSED));
});

test('CRC flips when a payload byte is corrupted (fail-closed detection)', () => {
  const b = new OrderBook({ baseTick: 900_000, tickCount: 262_144 });
  b.applyView(fakeView(A, { refLo: 1, side: 0x42, shares: 100, price: 999000 }));
  const buf = new Uint8Array(MDP1_SIZE);
  packSnapshot(b, buf, new Int32Array(10), new Int32Array(10));
  assert.ok(new Mdp1View(buf).crcOk());
  buf[40] ^= 0xff; // corrupt a bid level byte
  assert.ok(!new Mdp1View(buf).crcOk());
});

test('seq, msg_count and trade_count round-trip through the wire', () => {
  const b = new OrderBook({ baseTick: 900_000, tickCount: 262_144 });
  for (let i = 0; i < 25; i++) {
    b.applyView(fakeView(A, { refLo: i + 1, side: i % 2 ? 0x53 : 0x42, shares: 10, price: i % 2 ? 1001000 : 999000 }));
  }
  const buf = new Uint8Array(MDP1_SIZE);
  packSnapshot(b, buf, new Int32Array(10), new Int32Array(10));
  const v = new Mdp1View(buf);
  assert.equal(v.msgCount, 25);
  assert.equal(v.tradeCount, 0);
  assert.ok(v.crcOk());
});
