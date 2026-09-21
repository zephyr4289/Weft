// itch.test.mjs — ITCH 5.0 flyweight parser + engine framing semantics.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildAll, itchAdd, itchExecute, itchCancel, itchDelete, itchReplace, itchTrade, itchSystemEvent, BIG_REF_HI } from '../../../tests/adapters/managed/fixtures/generate.mjs';
import { ItchView, ItchEngine, frameOffsets, E_TRUNC, T_ADD, T_EXEC, T_REPLACE } from '../src/itch.js';
import { OrderBook } from '../src/book.js';

const { stream, scenario } = buildAll();

test('view decodes big-endian wire fields exactly', () => {
  const buf = itchAdd(123456789, BIG_REF_HI, 77, 'B', 500, 999900);
  const view = new ItchView(new DataView(buf.buffer, buf.byteOffset + 2, 36));
  view.bind(0);
  assert.equal(view.type, T_ADD);
  assert.equal(view.locate, 7777);
  assert.equal(view.tracking, 42);
  assert.equal(view.ts, 123456789);
  assert.equal(view.refHi, BIG_REF_HI);
  assert.equal(view.refLo, 77);
  assert.equal(view.side, 0x42);
  assert.equal(view.shares, 500);
  assert.equal(view.price, 999900);
  const scratch = new Uint8Array(8);
  view.stockInto(scratch);
  assert.equal(String.fromCharCode(...scratch).trim(), 'WEFTUSD');
});

test('execute / cancel / replace / trade field offsets are per-spec', () => {
  const ex = itchExecute(1, 0, 42, 300, 9000);
  const exV = new ItchView(new DataView(ex.buffer, ex.byteOffset + 2, 31));
  exV.bind(0);
  assert.equal(exV.shares, 300); // 'E' shares at +19
  assert.equal(exV.match, 9000);

  const ca = itchCancel(2, 0, 42, 150);
  const caV = new ItchView(new DataView(ca.buffer, ca.byteOffset + 2, 23));
  caV.bind(0);
  assert.equal(caV.shares, 150);

  const re = itchReplace(3, 0, 42, BIG_REF_HI, 43, 250, 999000);
  const reV = new ItchView(new DataView(re.buffer, re.byteOffset + 2, 35));
  reV.bind(0);
  assert.equal(reV.shares, 250); // 'U' shares at +27
  assert.equal(reV.price, 999000); // 'U' price at +31
  assert.equal(reV.ref2Hi, BIG_REF_HI);
  assert.equal(reV.ref2Lo, 43);

  const tr = itchTrade(4, 0, 42, 'S', 700, 1000500, 4294967296 + 5);
  const trV = new ItchView(new DataView(tr.buffer, tr.byteOffset + 2, 44));
  trV.bind(0);
  assert.equal(trV.match, 4294967301); // u64 match via hi*2^32+lo
});

test('golden stream framing: message count and type census match scenario', () => {
  const offs = frameOffsets(stream, stream.length);
  assert.equal(offs.length, scenario.message_count);
  const book = new OrderBook();
  const engine = new ItchEngine(book);
  assert.equal(engine.process(stream), 0);
  assert.equal(book.msgsApplied, scenario.message_count);
  assert.equal(book.skipped, 0);
  assert.equal(book.adds, scenario.counts.A);
  assert.equal(book.executes, scenario.counts.E);
  assert.equal(book.cancels, scenario.counts.X);
  assert.equal(book.deletes, scenario.counts.D);
  assert.equal(book.replaces, scenario.counts.U);
  assert.equal(book.tradeCount, scenario.counts.P + scenario.counts.E);
});

test('unknown message types are skipped by length prefix, never fatal', () => {
  // craft: valid add, then a fake type 0xZZ with 20B payload, then valid add
  const a1 = itchAdd(1, 0, 1, 'B', 100, 999000);
  const unk = Buffer.concat([Buffer.from([0x00, 20]), Buffer.alloc(20, 0xab)]);
  unk[2] = 0x7f; // unknown type byte
  const a2 = itchAdd(2, 0, 2, 'S', 100, 1000000);
  const feed = Buffer.concat([a1, unk, a2]);
  const book = new OrderBook();
  const engine = new ItchEngine(book);
  assert.equal(engine.process(feed), 0);
  assert.equal(book.skipped, 1);
  assert.equal(book.adds, 2);
  assert.equal(book.liveOrders, 2);
});

test('corrupt framing (length beyond buffer) fails closed with E_TRUNC', () => {
  const a1 = itchAdd(1, 0, 1, 'B', 100, 999000);
  const liar = Buffer.alloc(2); liar.writeUInt16BE(500, 0); // claims 500B
  const feed = Buffer.concat([a1, liar, Buffer.alloc(10)]);
  const book = new OrderBook();
  const engine = new ItchEngine(book);
  assert.equal(engine.process(feed), E_TRUNC);
  assert.equal(engine.truncated, 1);
  assert.equal(book.adds, 1); // first message still applied
});

test('incremental pumping via resume produces identical state', () => {
  const offs = frameOffsets(stream, stream.length);
  const a = new OrderBook();
  const ea = new ItchEngine(a);
  ea.process(stream, offs[1000]);
  ea.process(stream); // resume
  const b = new OrderBook();
  const eb = new ItchEngine(b);
  eb.process(stream);
  assert.equal(a.msgsApplied, b.msgsApplied);
  assert.equal(a.liveOrders, b.liveOrders);
  assert.equal(a.bestBid(), b.bestBid());
  assert.equal(a.bestAsk(), b.bestAsk());
  assert.equal(a.tradeCount, b.tradeCount);
});

test('system events advance lastTs but never rest orders', () => {
  const s1 = itchSystemEvent(5000, 'O');
  const book = new OrderBook();
  const engine = new ItchEngine(book);
  engine.process(s1);
  assert.equal(book.msgsApplied, 1);
  assert.equal(book.lastTs, 5000);
  assert.equal(book.liveOrders, 0);
});
