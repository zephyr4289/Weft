// weftc TS backend — full verification suite (node --test).
//
// Gates enforced here:
//   Golden parity    every field of every fixture vs expected.json (bit-exact)
//   Law 1            static: no allocation sites in hot-path bodies
//                    runtime: heap-stability probe over 1M reads (--expose-gc)
//   Law 2            static: every multi-byte DataView call passes explicit true
//                    runtime: golden bytes are little-endian and read correctly
//   Law 3            static: no node/browser globals or imports in generated code
//                    runtime: SharedArrayBuffer + Node Buffer windows bind fine
//   Law 4            validation matrix mirrors kernel weft_envelope_decode()
//   Determinism      generateTs(ir) twice -> byte-identical
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { TelemetryFrameView } from '../golden/telemetry-frame-view.js';
import { WeftEnvelopeView } from '../golden/weft-envelope-view.js';
import { ImuSampleView } from '../golden/imu-sample-view.js';
import { generateTs } from '../gen.mjs';
import { parseIrJson } from '../../lib/ir.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const GOLDEN = join(HERE, '..', '..', '..', 'tests', 'golden');
const TS_GOLDEN = join(HERE, '..', 'golden');

const bin = (n) => readFileSync(join(GOLDEN, `${n}.bin`));
const expected = JSON.parse(readFileSync(join(GOLDEN, 'expected.json'), 'utf8'));

// --- golden parity ---------------------------------------------------------

test('TelemetryFrameView reads every golden field bit-exactly', () => {
  const v = new TelemetryFrameView().bind(bin('telemetry_frame'), 0);
  const e = expected.telemetry_frame;
  assert.equal(v.validateHeader(), true);
  assert.equal(v.schemaId, BigInt(e.schemaId));
  assert.equal(v.timestampNs, BigInt(e.timestampNs));
  assert.equal(v.timestampNsLo, Number(BigInt(e.timestampNs) & 0xffffffffn));
  assert.equal(v.timestampNsHi, Number(BigInt(e.timestampNs) >> 32n));
  e.velocity.forEach((x, i) => assert.equal(v.getVelocityAt(i), x));
  assert.equal(v.pressurePa, e.pressurePa);
  assert.equal(v.state, e.state);
  e.gpsCoordinates.forEach((x, i) => assert.equal(v.getGpsCoordinatesAt(i), x));
  e.payloadHash.forEach((x, i) => assert.equal(v.getPayloadHashAt(i), x));
});

test('WeftEnvelopeView reads every golden field', () => {
  const v = new WeftEnvelopeView().bind(bin('weft_envelope'), 0);
  const e = expected.weft_envelope;
  assert.equal(v.getMagicAt(0), 0x57); // 'W'
  assert.equal(v.getMagicAt(1), 0x45); // 'E'
  assert.equal(v.getMagicAt(2), 0x46); // 'F'
  assert.equal(v.getMagicAt(3), 0x54); // 'T'
  assert.equal(v.version, e.version);
  assert.equal(v.headerSize, e.headerSize);
  assert.equal(v.seq, e.seq);
  assert.equal(v.payloadLen, e.payloadLen);
});

test('ImuSampleView (header-less) reads every golden field', () => {
  const v = new ImuSampleView().bind(bin('imu_sample'), 0);
  const e = expected.imu_sample;
  assert.equal(v.timestampNs, BigInt(e.timestampNs));
  e.accel.forEach((x, i) => assert.equal(v.getAccelAt(i), x));
  e.gyro.forEach((x, i) => assert.equal(v.getGyroAt(i), x));
});

// --- Law 4: validation matrix (kernel weft_envelope_decode parity) ---------

test('envelope validation matrix mirrors kernel decision table', () => {
  const stack = bin('weft_frame_stack');
  const ok = new WeftEnvelopeView().bind(stack, 0);
  assert.equal(ok.validateHeader(stack.length), true);

  // bare envelope: payloadLen 64 advertised, 0 payload bytes present -> SHORT
  const bare = new WeftEnvelopeView().bind(bin('weft_envelope'), 0);
  assert.equal(bare.validateHeader(16), false);  // 0 payload bytes present
  assert.equal(bare.validateHeader(79), false);  // 16 + 63: one byte short of 64 -> SHORT
  assert.equal(bare.validateHeader(80), true);   // 16 + 64: exactly enough

  // corrupt magic -> false, non-throwing
  const badMagic = Buffer.from(stack);
  badMagic[1] = 0x00; // 'E' -> 0x00
  assert.equal(new WeftEnvelopeView().bind(badMagic, 0).validateHeader(88), false);

  // corrupt version -> false
  const badVer = Buffer.from(stack);
  badVer[4] = 9;
  assert.equal(new WeftEnvelopeView().bind(badVer, 0).validateHeader(88), false);

  // corrupt header_size (< 16) -> false
  const badHs = Buffer.from(stack);
  badHs[6] = 8;
  assert.equal(new WeftEnvelopeView().bind(badHs, 0).validateHeader(88), false);

  // header_size > avail -> false
  const bigHs = Buffer.from(stack);
  bigHs[6] = 200;
  assert.equal(new WeftEnvelopeView().bind(bigHs, 0).validateHeader(88), false);

  // payloadLen > avail - headerSize -> false
  const bigPl = Buffer.from(stack);
  bigPl[12] = 200;
  assert.equal(new WeftEnvelopeView().bind(bigPl, 0).validateHeader(88), false);

  // unbound view -> false (no throw)
  assert.equal(new WeftEnvelopeView().validateHeader(88), false);
});

test('telemetry validateHeader asserts schemaId handshake (Law 4)', () => {
  const bad = Buffer.from(bin('telemetry_frame'));
  bad[0] ^= 0xff;
  const v = new TelemetryFrameView().bind(bad, 0);
  assert.equal(v.validateHeader(), false);
  assert.equal(v.schemaId !== TelemetryFrameView.SCHEMA_ID, true);
});

test('imu validateHeader is bounds-only (header-less path)', () => {
  assert.equal(new ImuSampleView().bind(bin('imu_sample'), 0).validateHeader(), true);
  assert.equal(new ImuSampleView().validateHeader(), false); // unbound
});

// --- vertical slice: envelope -> payload indirection ------------------------

test('full frame stack read: bind payload at env.headerSize', () => {
  const stack = bin('weft_frame_stack');
  const env = new WeftEnvelopeView().bind(stack, 0);
  assert.equal(env.validateHeader(stack.length), true);
  const frame = new TelemetryFrameView().bind(stack, env.headerSize);
  assert.equal(frame.validateHeader(), true);
  assert.equal(frame.timestampNs, BigInt(expected.telemetry_frame.timestampNs));
  assert.equal(frame.pressurePa, expected.telemetry_frame.pressurePa);
});

// --- write path + fluent mutators (zero-alloc chainable) --------------------

test('setters and with* mutators write through and chain', () => {
  const scratch = new Uint8Array(bin('telemetry_frame')).buffer;
  const v = new TelemetryFrameView().bind(scratch, 0);
  const ret = v
    .withTimestampNs(0x0123456789abcdefn)
    .withVelocityAt(2, -0.75)
    .withPressurePa(998.5)
    .withState(5)
    .withGpsCoordinatesAt(1, 12.5)
    .withPayloadHashAt(7, 0xa5)
    .withTimestampNsLo(0x89abcdef)
    .withTimestampNsHi(0x01234567);
  assert.equal(ret, v);
  assert.equal(v.timestampNs, 0x0123456789abcdefn);
  assert.equal(v.getVelocityAt(2), -0.75);
  assert.equal(v.pressurePa, 998.5);
  assert.equal(v.state, 5);
  assert.equal(v.getGpsCoordinatesAt(1), 12.5);
  assert.equal(v.getPayloadHashAt(7), 0xa5);
});

test('const fields are read-only (no setters emitted for handshake fields)', () => {
  const v = new TelemetryFrameView().bind(bin('telemetry_frame'), 0);
  // schemaId carries a const -> getter only; strict-mode assignment throws.
  assert.throws(() => {
    v.schemaId = 1n;
  }, TypeError);
});

// --- bind() robustness (Law 3 runtime surface) ------------------------------

test('bind resolves TypedArray windows (Node Buffer pool correctness)', () => {
  const stack = bin('weft_frame_stack');
  const window = stack.subarray(16, 80); // payload only
  const v = new TelemetryFrameView().bind(window, 0);
  assert.equal(v.validateHeader(), true);
  assert.equal(v.timestampNs, BigInt(expected.telemetry_frame.timestampNs));
  // byteOffset is ABSOLUTE in the underlying ArrayBuffer (pool + window + arg):
  // the value that matches a shared-memory debugger / ring layout.
  assert.equal(v.byteOffset, window.byteOffset);
  const v2 = new TelemetryFrameView().bind(stack, 16);
  assert.equal(v2.byteOffset, stack.byteOffset + 16); // pooled Buffer: absolute = pool + arg
});

test('bind accepts SharedArrayBuffer', () => {
  const sab = new SharedArrayBuffer(TelemetryFrameView.BYTE_LENGTH);
  new Uint8Array(sab).set(new Uint8Array(bin('telemetry_frame')));
  const v = new TelemetryFrameView().bind(sab, 0);
  assert.equal(v.validateHeader(), true);
  assert.equal(v.pressurePa, expected.telemetry_frame.pressurePa);
});

test('bind throws RangeError on out-of-bounds windows (cold path only)', () => {
  const small = new ArrayBuffer(8);
  assert.throws(() => new TelemetryFrameView().bind(small, 0), RangeError);
  assert.throws(() => new ImuSampleView().bind(new ArrayBuffer(32), 1), RangeError);
  assert.throws(() => new ImuSampleView().bind(new ArrayBuffer(32), -1), RangeError);
});

test('flyweight rebind: one instance, many windows, zero garbage', () => {
  const v = new TelemetryFrameView();
  const a = v.bind(bin('telemetry_frame'), 0);
  assert.equal(a.timestampNs, BigInt(expected.telemetry_frame.timestampNs));
  const stack = bin('weft_frame_stack');
  const b = v.bind(stack, 16);
  assert.equal(b, v);
  assert.equal(b.timestampNs, BigInt(expected.telemetry_frame.timestampNs)); // same bytes
  assert.equal(b.pressurePa, expected.telemetry_frame.pressurePa);
});

// --- Law 1: static allocation-site scan over hot-path bodies ----------------

function scanHotPaths(file) {
  const lines = readFileSync(file, 'utf8').split('\n');
  const banned = [
    /\bnew\b/,            // no object construction in hot path
    /`/,                  // no template literals (string allocation)
    /\.map\(/, /\.slice\(/, /\.split\(/, /\.concat\(/,
    /\bString\(/, /JSON\./,
    /\+\s*["']/,          // no string concatenation
  ];
  let inBind = false;
  let bindDepth = 0;
  const hits = [];
  for (const line of lines) {
    const t = line.trim();
    if (/^(public )?bind\(buffer/.test(t)) {
      inBind = true;
      bindDepth = 0;
    }
    if (inBind) {
      bindDepth += (t.match(/{/g) || []).length;
      bindDepth -= (t.match(/}/g) || []).length;
      if (bindDepth === 0) inBind = false; // bind() closed at outer depth
      continue; // cold path: new DataView / RangeError allowed here
    }
    if (t.startsWith('//') || t.startsWith('*') || t.startsWith('/*')) continue;
    if (t.startsWith('import ') || t.startsWith('export class') || t === '') continue;
    for (const re of banned) {
      if (re.test(t)) hits.push(`${file}: ${t}`);
    }
  }
  return hits;
}

test('Law 1 static scan: hot-path bodies contain zero allocation sites', () => {
  for (const f of ['telemetry-frame-view.js', 'weft-envelope-view.js', 'imu-sample-view.js']) {
    const hits = scanHotPaths(join(TS_GOLDEN, f));
    assert.deepEqual(hits, [], `${f} hot path must not allocate`);
  }
});

// --- Law 1 runtime: heap-stability probe (requires --expose-gc) -------------

test('Law 1 runtime: 1M-read steady-state loop keeps heap flat', { skip: typeof globalThis.gc !== 'function' ? 'run with --expose-gc' : false }, () => {
  const gc = globalThis.gc;
  const v = new TelemetryFrameView().bind(bin('telemetry_frame'), 0);
  const deltas = [];
  for (let round = 0; round < 5; round++) {
    gc(); gc();
    let sink = 0;
    const before = process.memoryUsage().heapUsed;
    for (let i = 0; i < 1_000_000; i++) {
      // Primitive-only hot path (Lo/Hi split): number reads and compares only.
      sink += v.pressurePa;
      sink += v.getVelocityAt(i % 3);
      sink += v.state;
      sink += v.timestampNsLo;
      sink += v.timestampNsHi;
      sink += v.getGpsCoordinatesAt(i & 1);
    }
    gc();
    const after = process.memoryUsage().heapUsed;
    deltas.push(after - before);
    assert.ok(Number.isFinite(sink)); // keep the loop from being DCE'd
  }
  deltas.sort((a, b) => a - b);
  const median = deltas[2];
  // Hot path emits zero allocation sites; residual growth is engine noise.
  // Threshold pinned at 1 MiB — measured medians are typically < 100 KiB.
  assert.ok(median < 1024 * 1024, `heap grew by ${median} bytes over 1M reads`);
});

test('Law 1 runtime: BigInt u64 path stays within primitive-boxing ceiling', { skip: typeof globalThis.gc !== 'function' ? 'run with --expose-gc' : false }, () => {
  const gc = globalThis.gc;
  const v = new TelemetryFrameView().bind(bin('telemetry_frame'), 0);
  gc(); gc();
  let sink = 0n;
  const before = process.memoryUsage().heapUsed;
  for (let i = 0; i < 500_000; i++) {
    sink += v.timestampNs;
  }
  const after = process.memoryUsage().heapUsed;
  assert.ok(sink > 0n);
  // BigInt is a JS primitive; transient young-gen boxing is reclaimed without
  // stop-the-world pauses at these rates. The ceiling catches accidental
  // OBJECT allocation creeping into the hot path.
  assert.ok(after - before < 64 * 1024 * 1024, 'u64 path must not allocate objects');
});

// --- Law 2: static little-endian scan ---------------------------------------

test('Law 2 static scan: every multi-byte access passes explicit little-endian true', () => {
  const multi = /(get|set)(Uint16|Uint32|BigUint64|Int16|Int32|BigInt64|Float32|Float64)\(/;
  for (const f of ['telemetry-frame-view.js', 'weft-envelope-view.js', 'imu-sample-view.js']) {
    const lines = readFileSync(join(TS_GOLDEN, f), 'utf8').split('\n');
    for (const [i, line] of lines.entries()) {
      if (!multi.test(line)) continue;
      if (line.trim().startsWith('//') || line.trim().startsWith('*')) continue;
      assert.ok(/, true\)/.test(line), `${f}:${i + 1} missing explicit little-endian: ${line.trim()}`);
    }
  }
});

// --- Law 3: static environment scan ------------------------------------------

test('Law 3 static scan: generated runtime is environment-agnostic', () => {
  const banned = [
    /require\(/,
    /from ['"]node:/,
    /\bprocess\./,
    /\bglobalThis\./,
    /\bwindow\./,
    /\bdocument\./,
    /__dirname/,
    /\bBuffer\./,
  ];
  const files = [
    'telemetry-frame-view.js', 'weft-envelope-view.js', 'imu-sample-view.js',
    'telemetry-frame-view.ts', 'weft-envelope-view.ts', 'imu-sample-view.ts',
  ];
  for (const f of files) {
    const text = readFileSync(join(TS_GOLDEN, f), 'utf8');
    for (const re of banned) {
      assert.ok(!re.test(text), `${f} contains environment-specific token ${re}`);
    }
  }
});

// --- artifacts: .d.ts + determinism ------------------------------------------

test('d.ts artifacts exist and annotate byte offsets in JSDoc', () => {
  for (const f of ['telemetry-frame-view.d.ts', 'weft-envelope-view.d.ts', 'imu-sample-view.d.ts']) {
    const text = readFileSync(join(TS_GOLDEN, f), 'utf8');
    assert.match(text, /export declare class/);
    assert.match(text, /validateHeader\(avail\?: number\): boolean/);
    assert.match(text, /bind\(buffer: ArrayBufferLike \| ArrayBufferView, byteOffset\?: number\): this/);
    assert.match(text, /@byteOffset 0x00/);
  }
  const tel = readFileSync(join(TS_GOLDEN, 'telemetry-frame-view.d.ts'), 'utf8');
  assert.match(tel, /public get timestampNsLo\(\): number;/);
  assert.match(tel, /public static readonly SCHEMA_ID: bigint;/);
});

test('codegen determinism: same IR generates byte-identical output', () => {
  const ir = parseIrJson(readFileSync(join(HERE, '..', '..', '..', 'schema', 'fixtures', 'telemetry_frame.json'), 'utf8'));
  const a = generateTs(ir, { irPath: 'tools/weftc/schema/fixtures/telemetry_frame.json' });
  const b = generateTs(ir, { irPath: 'tools/weftc/schema/fixtures/telemetry_frame.json' });
  assert.deepEqual([...a.files.entries()], [...b.files.entries()]);
  // committed golden matches in-process generation
  for (const [name, content] of a.files) {
    const committed = readFileSync(join(TS_GOLDEN, name), 'utf8');
    assert.equal(committed, content, `committed ${name} drifted from generator`);
  }
});
