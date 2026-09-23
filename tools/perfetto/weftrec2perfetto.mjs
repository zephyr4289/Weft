#!/usr/bin/env node
// weftrec2perfetto.mjs — RFC 0016 §7: the Perfetto bridge.
//
// Converts a .weftrec v4 kernel trace (RFC-0014, byte-identical across all
// ports) plus its optional .wsid v1 sidecar timeline (RFC-0016 §4) into the
// Chrome/Perfetto JSON trace format that ui.perfetto.dev renders as an
// interactive timeline: VSYNC alignment, governor decisions, claim
// latencies, ring depth, and drop bursts, per lane.
//
// Usage:
//   node weftrec2perfetto.mjs capture.weftrec [--sidecar capture.wsid]
//        [--out trace.json] [--selftest] [--golden path]
//
// DETERMINISM (Law 4): identical inputs produce byte-identical JSON. All
// arithmetic is integer; events are sorted by (ts, ph-rank, lane, name)
// with a stable merge; object keys are emitted in a fixed order; the
// output is UTF-8 with a trailing newline and nothing else.
//
// TIME MODEL: with a sidecar, ts = the injected t_ns (converted to integer
// microseconds; the full nanosecond value is preserved in args.ns). Without
// a sidecar the container carries NO timestamps (RFC-0014 keeps the event
// stream clock-free), so the bridge renders in EVENT-ORDER TIME: 1 event
// position = 1000 µs of timeline — a declared visualization convention,
// never a timing claim.

import { readFileSync, writeFileSync } from 'node:fs';

// ---------------------------------------------------------------------------
// CRC-32/zlib (reflected 0xEDB88320, init 0xFFFFFFFF, final xor) — RFC-0014 §1.4
// ---------------------------------------------------------------------------
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();
function crc32(buf, start = 0, end = buf.length) {
  let c = 0xffffffff;
  for (let i = start; i < end; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// ---------------------------------------------------------------------------
// Format constants (must match core/c/trace_rec.h and core/c/weft_trace.h)
// ---------------------------------------------------------------------------
const WEFTREC_MAGIC = 0x43455257; // "WREC" LE
const WEFTREC_VERSION = 4;
const WEFTREC_HDR = 32;
const WEFTREC_REC = 12;
const SIDECAR_MAGIC = 0x44495357; // "WSID" LE
const SIDECAR_VERSION = 1;
const SIDECAR_HDR = 32;
const SIDECAR_REC = 20;
const SIDECAR_NO_REF = 0xffffffff;

const KERNEL_KINDS = {
  1: 'publish', 2: 'claim', 3: 'drop', 4: 'revoke',
  5: 'ack', 6: 'stall', 7: 'tear', 8: 'canary_fail',
};
// runtime kinds live in [16, 0x8000) — disjoint from kernel kinds 1..8
const XT = {
  16: 'vsync_tick', 17: 'governor_action', 18: 'trend_verdict', 19: 'gc_pause',
  20: 'ring_depth', 21: 'freshness', 22: 'claim_latency', 23: 'drop_burst',
  24: 'marker',
};
// lanes (pid 1 "weft"); tid assignment is part of the determinism contract
const LANES = {
  writer: 1, reader: 2, governor: 3, display: 4, host: 5, user: 6, counters: 7,
};

function fail(msg) {
  console.error(`weftrec2perfetto: ${msg}`);
  process.exit(1);
}

// ---------------------------------------------------------------------------
// Parsers (strict: version gates + CRC verification, per RFC-0014)
// ---------------------------------------------------------------------------
export function parseWeftrec(buf) {
  if (buf.length < WEFTREC_HDR) fail('weftrec: shorter than the 32B header');
  const magic = buf.readUInt32LE(0);
  const version = buf.readUInt16LE(4);
  const hdrSize = buf.readUInt16LE(6);
  const count = buf.readUInt32LE(16);
  const crc = buf.readUInt32LE(20);
  if (magic !== WEFTREC_MAGIC) fail('weftrec: bad magic (not a .weftrec)');
  if (version !== WEFTREC_VERSION) {
    fail(`weftrec: version gate — got ${version}, this bridge speaks exactly 4`);
  }
  if (hdrSize !== WEFTREC_HDR) fail(`weftrec: header size ${hdrSize} != 32`);
  if (crc !== crc32(buf, 0, 20)) fail('weftrec: header CRC mismatch');
  const need = WEFTREC_HDR + count * WEFTREC_REC;
  if (buf.length < need) fail(`weftrec: truncated (${buf.length} < ${need})`);
  const events = [];
  for (let i = 0; i < count; i++) {
    const off = WEFTREC_HDR + i * WEFTREC_REC;
    const kind = buf.readUInt16LE(off);
    const aux = buf.readUInt16LE(off + 2);
    const data = buf.readUInt32LE(off + 4);
    const rcrc = buf.readUInt32LE(off + 8);
    if (rcrc !== crc32(buf, off, off + 8)) {
      fail(`weftrec: record ${i} CRC mismatch`);
    }
    if (!KERNEL_KINDS[kind]) fail(`weftrec: record ${i} unknown kernel kind ${kind}`);
    events.push({ i, kind, kindName: KERNEL_KINDS[kind], aux, data });
  }
  return { events, count };
}

export function parseSidecar(buf) {
  if (!buf) return null;
  if (buf.length < SIDECAR_HDR) fail('sidecar: shorter than the 32B header');
  const magic = buf.readUInt32LE(0);
  const version = buf.readUInt32LE(4);
  const count = buf.readUInt32LE(16);
  const crc = buf.readUInt32LE(20);
  const epoch = buf.readBigUInt64LE(24);
  if (magic !== SIDECAR_MAGIC) fail('sidecar: bad magic (not a .wsid)');
  if (version !== SIDECAR_VERSION) fail(`sidecar: version gate — got ${version}`);
  if (crc !== crc32(buf, 0, 20)) fail('sidecar: header CRC mismatch');
  if (buf.length < SIDECAR_HDR + count * SIDECAR_REC) fail('sidecar: truncated');
  const recs = [];
  for (let i = 0; i < count; i++) {
    const off = SIDECAR_HDR + i * SIDECAR_REC;
    recs.push({
      t_ns: buf.readBigUInt64LE(off),
      back_ref: buf.readUInt32LE(off + 8),
      producer: buf.readUInt16LE(off + 12),
      kind: buf.readUInt16LE(off + 14),
      data: buf.readUInt32LE(off + 16),
    });
  }
  return { epoch, recs, count };
}

// ---------------------------------------------------------------------------
// Chrome/Perfetto JSON construction
// ---------------------------------------------------------------------------
const PH_RANK = { M: 0, B: 1, E: 2, X: 3, i: 4, C: 5 };

function ev(name, cat, ph, tsUs, tid, args, ns) {
  // fixed key order — the determinism contract
  const e = { name, cat, ph, ts: tsUs, pid: 1, tid };
  if (args !== undefined || ns !== undefined) {
    e.args = { ...(args || {}), ...(ns !== undefined ? { ns } : {}) };
  }
  return e;
}

function laneOf(producer, kind) {
  if (producer === null) {
    return kind === 1 || kind === 3 || kind === 4 ? LANES.writer : LANES.reader;
  }
  switch (producer) {
    case 0: return LANES.writer;
    case 1: return LANES.reader;
    case 2: return LANES.governor;
    case 3: return LANES.display;
    case 4: return LANES.host;
    default: return LANES.user;
  }
}

export function buildTraceEvents(wft, side) {
  const out = [];
  // metadata first (ts 0, PH_RANK 0)
  out.push(ev('weft', '__metadata', 'M', 0, 0, { name: 'process_name' }));
  for (const [lane, tid] of Object.entries(LANES)) {
    out.push(ev(lane, '__metadata', 'M', 0, tid, { name: 'thread_name' }));
  }

  const sideByRef = new Map();
  const sideRuntime = [];
  if (side) {
    for (const r of side.recs) {
      if (r.back_ref === SIDECAR_NO_REF) sideRuntime.push(r);
      else sideByRef.set(r.back_ref, r);
    }
  }
  const noSide = !side;
  const posToUs = (pos) => pos * 1000; // event-order time (declared)
  const tsOf = (pos, nsBig) => (nsBig !== null ? Number(nsBig / 1000n) : posToUs(pos));
  const nsOf = (pos, nsBig) => (nsBig !== null ? Number(nsBig) : pos * 1000000);

  // pass 1: kernel instants + flow endpoint collection
  const pubAt = new Map();   // seq -> {ts, lane}
  const endAt = new Map();   // seq -> {ts, lane, kind}
  for (const e of wft.events) {
    const rec = side ? sideByRef.get(e.i) || null : null;
    const nsBig = rec ? rec.t_ns : null;
    const lane = laneOf(rec ? rec.producer : null, e.kind);
    const ts = tsOf(e.i, nsBig);
    const args = { seq: e.data };
    if (e.kind === 1) { // publish: aux = payload_len
      args.payload_len = e.aux;
      pubAt.set(e.data, { ts, lane });
    }
    if (e.kind === 2 || e.kind === 3 || e.kind === 5) {
      endAt.set(e.data, { ts, lane, kind: e.kindName });
    }
    out.push(ev(e.kindName, 'kernel', 'i', ts, lane, args, nsOf(e.i, nsBig)));
  }
  // pass 2: async flow links publish(seq) -> {claim|drop|ack}(seq)
  for (const [seq, pub] of pubAt) {
    const end = endAt.get(seq);
    if (!end) continue;
    out.push({
      name: `frame ${seq}`, cat: 'flow', ph: 'b', ts: pub.ts,
      pid: 1, tid: pub.lane, id: seq, args: {},
    });
    out.push({
      name: `frame ${seq}`, cat: 'flow', ph: 'e', ts: end.ts,
      pid: 1, tid: end.lane ?? pub.lane, id: seq, args: { end: end.kind },
    });
  }

  // pass 3: runtime records
  for (const r of sideRuntime) {
    const ts = Number(r.t_ns / 1000n);
    const ns = Number(r.t_ns);
    switch (r.kind) {
      case 16: // vsync_tick
        out.push(ev('vsync', 'display', 'i', ts, LANES.display, { frame: r.data }, ns));
        break;
      case 17: { // governor_action: bits 31..24 action, 23..0 param
        const action = ['FastPath', 'Skip', 'Snapshot', 'Reseed'][r.data >>> 24] || 'action';
        out.push(ev(`gov:${action}`, 'governor', 'X', ts, LANES.governor,
          { param: r.data & 0xffffff }, ns));
        break;
      }
      case 18: { // trend_verdict
        const verdict = ['STABLE', 'RISING', 'FALLING', 'BURST'][r.data >>> 24] || 'v?';
        out.push(ev(`trend:${verdict}`, 'governor', 'i', ts, LANES.governor,
          { level_q16: r.data & 0xffffff }, ns));
        break;
      }
      case 19:
        out.push(ev('gc_pause', 'host', 'X', ts, LANES.host, { dur_ns: r.data }, ns));
        break;
      case 20:
        out.push(ev('ring_depth', 'metric', 'C', ts, LANES.counters, { depth: r.data }, ns));
        break;
      case 21:
        out.push(ev('freshness', 'metric', 'C', ts, LANES.counters, { behind: r.data }, ns));
        break;
      case 22:
        out.push(ev('claim_latency', 'metric', 'C', ts, LANES.counters,
          { latency_ns: r.data }, ns));
        break;
      case 23:
        out.push(ev('drops', 'metric', 'C', ts, LANES.counters, { burst: r.data }, ns));
        break;
      case 24:
        out.push(ev('marker', 'user', 'i', ts, LANES.user, { id: r.data }, ns));
        break;
      default:
        fail(`sidecar: unknown runtime kind ${r.kind} (registry gap — extend the bridge)`);
    }
  }

  // deterministic order: (ts, PH_RANK, tid, name) stable merge — metadata
  // (ts 0, rank 0) first by construction.
  const idx = out.map((e, i) => i);
  idx.sort((a, b) => {
    const ea = out[a], eb = out[b];
    if (ea.ts !== eb.ts) return ea.ts - eb.ts;
    const ra = PH_RANK[ea.ph] ?? 9, rb = PH_RANK[eb.ph] ?? 9;
    if (ra !== rb) return ra - rb;
    if (ea.tid !== eb.tid) return ea.tid - eb.tid;
    if (ea.name !== eb.name) return ea.name < eb.name ? -1 : 1;
    return a - b; // stable
  });
  return idx.map((i) => out[i]);
}

// ---------------------------------------------------------------------------
// Deterministic rendering (fixed separators, trailing newline)
// ---------------------------------------------------------------------------
export function render(events) {
  return `{"traceEvents":[\n${events.map((e) => JSON.stringify(e)).join(',\n')}\n]}\n`;
}

// ---------------------------------------------------------------------------
// Selftest: synthetic capture -> structural invariants + byte-identity
// ---------------------------------------------------------------------------
export function selftest() {
  // encode a synthetic v4 + sidecar (mirrors core/c test T4/T5 encodings)
  const nEvents = 12;
  const v4 = Buffer.alloc(WEFTREC_HDR + nEvents * WEFTREC_REC);
  v4.writeUInt32LE(WEFTREC_MAGIC, 0);
  v4.writeUInt16LE(WEFTREC_VERSION, 4);   // u16 version (the codec layout)
  v4.writeUInt16LE(WEFTREC_HDR, 6);       // u16 header size (32)
  v4.writeUInt32LE(0x4, 8);               // flags: TRACE bit
  v4.writeUInt32LE(0, 12);
  v4.writeUInt32LE(nEvents, 16);
  v4.writeUInt32LE(crc32(v4, 0, 20), 20);
  const kinds = [1, 2, 1, 2, 1, 3, 1, 2, 1, 2, 6, 5];
  for (let i = 0; i < nEvents; i++) {
    const off = WEFTREC_HDR + i * WEFTREC_REC;
    const kind = kinds[i];
    const aux = kind === 1 ? 64 : 0;
    const data = kind === 1 ? Math.floor(i / 2) + 1 : 1;
    v4.writeUInt16LE(kind, off);
    v4.writeUInt16LE(aux, off + 2);
    v4.writeUInt32LE(data, off + 4);
    v4.writeUInt32LE(crc32(v4, off, off + 8), off + 8);
  }
  const nRecs = nEvents + 3;
  const wsid = Buffer.alloc(SIDECAR_HDR + nRecs * SIDECAR_REC);
  wsid.writeUInt32LE(SIDECAR_MAGIC, 0);
  wsid.writeUInt32LE(SIDECAR_VERSION, 4);
  wsid.writeUInt32LE(nRecs, 16);
  wsid.writeUInt32LE(crc32(wsid, 0, 20), 20);
  wsid.writeBigUInt64LE(1000000n, 24);
  for (let i = 0; i < nEvents; i++) {
    const off = SIDECAR_HDR + i * SIDECAR_REC;
    wsid.writeBigUInt64LE(BigInt((i + 1) * 1000), off);
    wsid.writeUInt32LE(i, off + 8);
    wsid.writeUInt16LE(i % 2, off + 12);
    wsid.writeUInt16LE(0, off + 14); // unused for kernel refs
    wsid.writeUInt32LE(0, off + 16);
  }
  const runtime = [
    { t: 15500n, kind: 16, data: 42 }, { t: 16000n, kind: 17, data: (1 << 24) | 3 },
    { t: 16500n, kind: 20, data: 2 },
  ];
  runtime.forEach((r, k) => {
    const off = SIDECAR_HDR + (nEvents + k) * SIDECAR_REC;
    wsid.writeBigUInt64LE(r.t, off);
    wsid.writeUInt32LE(SIDECAR_NO_REF, off + 8);
    wsid.writeUInt16LE(2, off + 12);
    wsid.writeUInt16LE(r.kind, off + 14);
    wsid.writeUInt32LE(r.data, off + 16);
  });

  const wft = parseWeftrec(v4);
  const side = parseSidecar(wsid);
  const events = buildTraceEvents(wft, side);
  const json1 = render(events);
  const json2 = render(buildTraceEvents(parseWeftrec(v4), parseSidecar(wsid)));
  const checks = [
    [wft.count === nEvents, 'v4 parses 12 events'],
    [side.count === nRecs, 'sidecar parses 15 records'],
    [json1 === json2, 'byte-identity across two builds'],
    [events[0].ph === 'M', 'metadata first'],
    [events.some((e) => e.ph === 'b') && events.some((e) => e.ph === 'e'),
      'flow endpoints present'],
    [events.some((e) => e.name === 'ring_depth' && e.ph === 'C'),
      'counter track present'],
    [events.every((e, i) => i === 0 || events[i - 1].ts <= e.ts),
      'non-decreasing ts'],
    [json1.endsWith('}\n'), 'trailing newline'],
  ];
  let bad = 0;
  for (const [ok, name] of checks) {
    console.log(`  ${ok ? 'PASS' : 'FAIL'} ${name}`);
    if (!ok) bad++;
  }
  return { json: json1, bad };
}

// ---------------------------------------------------------------------------
// CLI (guarded: importing this module as a library must not execute it)
// ---------------------------------------------------------------------------
const isMain = process.argv[1] && import.meta.url.endsWith(process.argv[1].split('/').pop());
if (isMain) {
  const argv = process.argv.slice(2);
  if (argv.includes('--selftest')) {
    const { json, bad } = selftest();
    if (argv.includes('--golden')) {
      const path = argv[argv.indexOf('--golden') + 1];
      writeFileSync(path, json);
      console.log(`golden written: ${path}`);
    }
    const { createHash } = await import('node:crypto');
    console.log(`sha256 ${createHash('sha256').update(json).digest('hex')}`);
    process.exit(bad === 0 ? 0 : 1);
  }
  const input = argv.find((a) => !a.startsWith('--'));
  if (!input) {
    fail('usage: weftrec2perfetto.mjs <capture.weftrec> [--sidecar <c.wsid>] [--out <trace.json>]');
  }
  const wft = parseWeftrec(readFileSync(input));
  const sidePath = argv.includes('--sidecar') ? argv[argv.indexOf('--sidecar') + 1] : null;
  const side = sidePath ? parseSidecar(readFileSync(sidePath)) : null;
  const json = render(buildTraceEvents(wft, side));
  const outPath = argv.includes('--out') ? argv[argv.indexOf('--out') + 1] : null;
  if (outPath) {
    writeFileSync(outPath, json);
    const { createHash } = await import('node:crypto');
    console.log(`wrote ${outPath} (${json.length} bytes, ${wft.count} kernel events` +
      `${side ? `, ${side.count} sidecar records` : ''}) sha256 ` +
      createHash('sha256').update(json).digest('hex'));
  } else {
    process.stdout.write(json);
  }
}
