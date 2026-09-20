// exporters/perfetto.js — native .pftrace (Perfetto protobuf Trace) export.
//
// Hand-rolled protobuf wire encoding (varint + length-delimited) — no
// dependencies, no protobufjs. Emits the minimal-but-valid packet set the
// Perfetto UI / trace_processor accepts:
//   Trace { repeated TracePacket packet = 1 }
//   TracePacket { timestamp=8, track_event=11, track_descriptor=60,
//                 trusted_packet_sequence_id=10, sequence_flags=15 }
//   TrackEvent  { type=9, track_uuid=11, name=23 }   (1=BEGIN, 2=END, 3=INSTANT)
//   TrackDescriptor { uuid=1, name=2 }
// Cross-node packet transit (gossip hops, publish->consume slices) becomes
// nanosecond-visible lanes in ui.perfetto.dev.
//
// Cold path only: exporters run at shutdown/scrape time, never per frame.

const WT_VARINT = 0, WT_LEN = 2;

class PbWriter {
  constructor(cap = 4096) { this.buf = new Uint8Array(cap); this.len = 0; }

  ensure(n) {
    if (this.len + n <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + n) cap *= 2;
    const bigger = new Uint8Array(cap);
    bigger.set(this.buf.subarray(0, this.len));
    this.buf = bigger;
  }

  byte(b) { this.ensure(1); this.buf[this.len++] = b; }

  /** Unsigned varint; accepts number or BigInt (timestamps exceed 2^53). */
  varint(v) {
    let u = typeof v === 'bigint' ? v : BigInt(Math.max(0, Math.floor(v)));
    for (;;) {
      const b = Number(u & 0x7fn);
      u >>= 7n;
      if (u === 0n) { this.byte(b); return; }
      this.byte(b | 0x80);
    }
  }

  tag(field, wt) { this.varint((field << 3) | wt); }

  uint(field, v) { this.tag(field, WT_VARINT); this.varint(v); }

  /** Length-delimited field from a byte source (Uint8Array or ASCII string). */
  bytes(field, src) {
    const n = src.length;
    this.tag(field, WT_LEN);
    this.varint(n);
    this.ensure(n);
    if (typeof src === 'string') {
      for (let i = 0; i < n; i++) this.buf[this.len++] = src.charCodeAt(i);
    } else {
      this.buf.set(src, this.len);
      this.len += n;
    }
  }

  /**
   * Nested message: encode content into a child writer, then emit
   * tag + exact-length varint + bytes. Simple and always correct.
   */
  message(field, fill) {
    const child = new PbWriter(this.buf.length);
    fill(child);
    this.bytes(field, child.buf.subarray(0, child.len));
  }
}

/** Deterministic 64-bit-ish track uuid from a track name (FNV-1a x2 lanes). */
function trackUuid(name) {
  let h1 = 0x811c9dc5, h2 = 0xc9dc5118;
  for (let i = 0; i < name.length; i++) {
    h1 = (h1 ^ name.charCodeAt(i)) >>> 0;
    h1 = (Math.imul(h1, 0x01000193)) >>> 0;
    h2 = (h2 + name.charCodeAt(i) * (i + 1)) >>> 0;
    h2 = (Math.imul(h2, 0x85ebca6b)) >>> 0;
  }
  return ((BigInt(h1) << 32n) | BigInt(h2)) & 0x7fffffffffffffffn;
}

/**
 * Build a .pftrace byte buffer from events.
 * @param {Array<{ts: number|bigint, dur?: number|bigint, name: string,
 *                track: string, cat?: string}>} events
 * @returns {Uint8Array}
 */
export function buildPftrace(events) {
  const tracks = [...new Set(events.map((e) => e.track))];
  const uuidByTrack = new Map(tracks.map((t) => [t, trackUuid(t)]));
  const w = new PbWriter(4096);
  let firstSeq = true;
  const emitPacket = (fn) => w.message(1, fn);
  for (const e of events) {
    emitPacket((p) => {
      p.uint(8, typeof e.ts === 'bigint' ? e.ts : BigInt(Math.round(e.ts))); // timestamp ns
      if (firstSeq) {
        p.uint(15, 1); // sequence_flags: SEQ_INCREMENTAL_STATE_CLEARED
        firstSeq = false;
      }
      p.uint(10, 1);   // trusted_packet_sequence_id
      p.message(11, (te) => {
        te.uint(9, e.dur === undefined || e.dur === 0 || e.dur === 0n ? 3 : 1); // INSTANT / BEGIN
        te.uint(11, uuidByTrack.get(e.track));
        te.bytes(23, e.name);
      });
    });
    if (e.dur !== undefined && e.dur !== 0 && e.dur !== 0n) {
      emitPacket((p) => {
        const endTs = (typeof e.ts === 'bigint' ? e.ts : BigInt(Math.round(e.ts))) +
          (typeof e.dur === 'bigint' ? e.dur : BigInt(Math.round(e.dur)));
        p.uint(8, endTs);
        p.uint(10, 1);
        p.message(11, (te) => {
          te.uint(9, 2); // TYPE_SLICE_END
          te.uint(11, uuidByTrack.get(e.track));
        });
      });
    }
  }
  // Track descriptors appended after the event packets — trace_processor
  // accepts descriptors in any position relative to events.
  for (const t of tracks) {
    emitPacket((p) => {
      p.message(60, (td) => {
        td.uint(1, uuidByTrack.get(t));
        td.bytes(2, t);
      });
    });
  }
  return w.buf.subarray(0, w.len);
}

/** Write a .pftrace file (cold path). Returns bytes written. */
export async function writePftrace(path, events) {
  const fs = await import('node:fs/promises');
  const bytes = buildPftrace(events);
  await fs.writeFile(path, bytes);
  return bytes.length;
}
