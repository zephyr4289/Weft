// hot_plane.ts — the WHP1 plane VIEW: open (validate once, Law 4 refusal
// ladder), expose lanes through preallocated TypedArray views (Law 1), and
// the writer-side constructor `HotPlane.create` used by the host app, the
// synthetic producer and every test.
//
// WHY OPEN-TIME VALIDATION: the frame loop is a hot path that must never
// branch on "is this plane well-formed". Every structural question is
// answered HERE, once, before the first frame; the loop then only touches
// prevalidated numbers and the per-lane precomputed atomic indices. A
// malformed plane never reaches a renderer — it is refused with a named
// code from src/errors.ts, and the battery makes each code fire at least
// once (test/plane.test.ts) so the ladder is MEASURED.
//
// LAW 1 DISCIPLINE: open()/create() allocate (views, LaneView objects, the
// class itself) — that is initialization, allowed. AFTER open(), every
// read path touches only preallocated state or the SAB itself.
//
// THE VIEWS (why one Int32Array aliases the whole plane): JS Atomics
// operate on Integer TypedArrays over the SAME buffer as the data. One
// i32 view covers header + lane table; each LaneView carries the PRE-
// COMPUTED u32 indices of its write_pos / seq / dirty words so the frame
// loop does zero address arithmetic beyond an array load. The
// BigUint64Array exists for the producer's single-OR road (markDirty64).

import {
  HP_DTYPE,
  HP_KIND,
  HP_KIND_STRIDE,
  WHP1_DATA_START,
  WHP1_HEADER_BYTES,
  WHP1_LANE_FLAG_ACTIVE,
  WHP1_LANE_MAGIC,
  WHP1_LANE_OFF_CAPACITY,
  WHP1_LANE_OFF_DIRTY_HI,
  WHP1_LANE_OFF_DIRTY_LO,
  WHP1_LANE_OFF_DTYPE,
  WHP1_LANE_OFF_FLAGS,
  WHP1_LANE_OFF_GRANULARITY,
  WHP1_LANE_OFF_KIND,
  WHP1_LANE_OFF_MAGIC,
  WHP1_LANE_OFF_OFFSET,
  WHP1_LANE_OFF_SEQ,
  WHP1_LANE_OFF_STRIDE,
  WHP1_LANE_OFF_WRITE_POS,
  WHP1_LANE_STRIDE_TABLE,
  WHP1_LANE_TABLE,
  WHP1_MAGIC,
  WHP1_MAX_LANES,
  WHP1_MAX_LANE_CAPACITY,
  WHP1_OFF_DATA_START,
  WHP1_OFF_EPOCH,
  WHP1_OFF_FLAGS,
  WHP1_OFF_HEADER_BYTES,
  WHP1_OFF_LANE_COUNT,
  WHP1_OFF_MAGIC,
  WHP1_OFF_PLANE_BYTES,
  WHP1_OFF_PRODUCER_SEQ,
  WHP1_OFF_VERSION,
  WHP1_VERSION,
  whp1PlaneBytes,
} from './whp1.ts';
import { HeddleError } from '../errors.ts';

/** A lane as the writer declares it (HotPlane.create input). */
export interface HpLaneSpec {
  kind: number; // HP_KIND_*
  capacity: number; // element count (> 0)
  stride: number; // bytes per element (HP_KIND_STRIDE law)
  /** Elements per dirty bit (>= ceil(capacity/64), <= capacity). */
  granularity: number;
}

/**
 * Read-side view of one lane, carved ONCE at open(). Field names are the
 * HAL's vocabulary: f32/u32 (data views), strideBytes, the atomic u32
 * indices, and dirtyGranularity.
 */
export interface LaneView {
  readonly laneIndex: number;
  readonly kind: number;
  readonly offset: number; // byte offset into the plane
  readonly capacity: number; // elements
  readonly strideBytes: number; // bytes per element
  /** f32 alias over the lane data (samples / row payloads). */
  readonly f32: Float32Array;
  /** u32 alias over the lane data (row payload words, bit-exact reads). */
  readonly u32: Uint32Array;
  // --- precomputed atomic indices into PlaneView.i32 (u32 word indices) --
  readonly writePosI32: number;
  readonly seqI32: number;
  readonly dirtyLoI32: number;
  readonly dirtyHiI32: number;
  readonly dirtyGranularity: number;
}

/** The whole-plane read view. `sab` is the zero-copy substrate itself. */
export interface PlaneView {
  readonly sab: SharedArrayBuffer;
  /** i32 atomic alias over header + lane table (atomics-capable). */
  readonly i32: Int32Array;
  /** big64 alias for the producer's single-OR dirty road. */
  readonly big64: BigUint64Array;
  readonly laneCount: number;
  readonly lanes: readonly LaneView[];
}

/**
 * Concrete plane object. Implements PlaneView; `open()` returns it after
 * the full validation ladder, `create()` builds a valid plane from specs
 * and re-opens it (the writer is not trusted either — create is
 * convenience, open is law).
 */
export class HotPlane implements PlaneView {
  readonly sab: SharedArrayBuffer;
  readonly i32: Int32Array;
  readonly big64: BigUint64Array;
  readonly laneCount: number;
  readonly lanes: readonly LaneView[];

  private constructor(
    sab: SharedArrayBuffer,
    i32: Int32Array,
    big64: BigUint64Array,
    lanes: LaneView[],
  ) {
    this.sab = sab;
    this.i32 = i32;
    this.big64 = big64;
    this.laneCount = lanes.length;
    this.lanes = lanes;
  }

  // ------------------------------------------------------------------ writer

  /** Allocate a plane SAB and write a valid WHP1 header + lane table. */
  static create(lanes: readonly HpLaneSpec[]): HotPlane {
    if (lanes.length === 0 || lanes.length > WHP1_MAX_LANES) {
      throw new HeddleError(
        'HC_E_PLANE_LANE_COUNT',
        `lane count ${lanes.length} outside 1..${WHP1_MAX_LANES}`,
      );
    }
    for (let i = 0; i < lanes.length; i++) {
      const s = lanes[i];
      if (HP_KIND_STRIDE[s.kind] === undefined) {
        throw new HeddleError('HC_E_LANE_KIND', `lane ${i}: unknown kind ${s.kind}`);
      }
      if (s.stride !== HP_KIND_STRIDE[s.kind]) {
        throw new HeddleError(
          'HC_E_LANE_STRIDE',
          `lane ${i}: kind ${s.kind} requires stride ${HP_KIND_STRIDE[s.kind]} (Law 2), got ${s.stride}`,
        );
      }
      if (s.capacity <= 0 || s.capacity > WHP1_MAX_LANE_CAPACITY) {
        throw new HeddleError(
          'HC_E_LANE_CAPACITY',
          `lane ${i}: capacity ${s.capacity} outside 1..${WHP1_MAX_LANE_CAPACITY}`,
        );
      }
      validateGranularity(i, s.capacity, s.granularity);
    }
    const planeBytes = whp1PlaneBytes(lanes);
    if (planeBytes > 0xffffffff) {
      throw new HeddleError(
        'HC_E_PLANE_TOO_SMALL',
        `plane of ${planeBytes} B exceeds the v1 4-GiB envelope`,
      );
    }
    const sab = new SharedArrayBuffer(planeBytes);
    const u32 = new Uint32Array(sab);
    // Header (little-endian words; see whp1.ts for the map).
    u32[WHP1_OFF_MAGIC / 4] = WHP1_MAGIC;
    u32[WHP1_OFF_VERSION / 4] = WHP1_VERSION;
    u32[WHP1_OFF_HEADER_BYTES / 4] = WHP1_HEADER_BYTES;
    u32[WHP1_OFF_LANE_COUNT / 4] = lanes.length;
    u32[WHP1_OFF_DATA_START / 4] = WHP1_DATA_START;
    u32[WHP1_OFF_DATA_START / 4 + 1] = 0;
    u32[WHP1_OFF_PLANE_BYTES / 4] = planeBytes;
    u32[WHP1_OFF_PLANE_BYTES / 4 + 1] = 0;
    // Lane table + data region.
    let off = WHP1_DATA_START;
    for (let i = 0; i < lanes.length; i++) {
      const s = lanes[i];
      off = (off + 127) & ~127; // 128-B align each lane (Law 2)
      const base = (WHP1_LANE_TABLE + i * WHP1_LANE_STRIDE_TABLE) / 4;
      u32[base + WHP1_LANE_OFF_MAGIC / 4] = WHP1_LANE_MAGIC;
      u32[base + WHP1_LANE_OFF_KIND / 4] = s.kind;
      u32[base + WHP1_LANE_OFF_DTYPE / 4] = HP_DTYPE.F32;
      u32[base + WHP1_LANE_OFF_GRANULARITY / 4] = s.granularity;
      u32[base + WHP1_LANE_OFF_OFFSET / 4] = off;
      u32[base + WHP1_LANE_OFF_OFFSET / 4 + 1] = 0;
      u32[base + WHP1_LANE_OFF_CAPACITY / 4] = s.capacity;
      u32[base + WHP1_LANE_OFF_CAPACITY / 4 + 1] = 0;
      u32[base + WHP1_LANE_OFF_STRIDE / 4] = s.stride;
      u32[base + WHP1_LANE_OFF_STRIDE / 4 + 1] = 0;
      u32[base + WHP1_LANE_OFF_FLAGS / 4] = WHP1_LANE_FLAG_ACTIVE;
      off += s.stride * s.capacity;
    }
    return HotPlane.open(sab);
  }

  // ------------------------------------------------------------------ reader

  /**
   * Validate a plane SAB and carve the read-side views. The ladder order
   * is the documented audit order in RFC-0022 §3.4 — structural checks
   * first, per-lane checks in descriptor order.
   */
  static open(sab: unknown): HotPlane {
    if (!(sab instanceof SharedArrayBuffer)) {
      throw new HeddleError('HC_E_NOT_SAB', `expected SharedArrayBuffer, got ${typeName(sab)}`);
    }
    const buf = sab as SharedArrayBuffer;
    if (buf.byteLength < WHP1_DATA_START + 128) {
      throw new HeddleError(
        'HC_E_PLANE_TOO_SMALL',
        `${buf.byteLength} B < minimum ${WHP1_DATA_START + 128} B (header + lane table + one 128-B lane)`,
      );
    }
    const u32 = new Uint32Array(buf);
    if (u32[WHP1_OFF_MAGIC / 4] !== WHP1_MAGIC) {
      throw new HeddleError(
        'HC_E_PLANE_MAGIC',
        `magic 0x${hex(u32[WHP1_OFF_MAGIC / 4])} != 0x${hex(WHP1_MAGIC)}`,
      );
    }
    if (u32[WHP1_OFF_VERSION / 4] !== WHP1_VERSION) {
      throw new HeddleError('HC_E_PLANE_VERSION', `version ${u32[WHP1_OFF_VERSION / 4]} != ${WHP1_VERSION}`);
    }
    if (u32[WHP1_OFF_HEADER_BYTES / 4] !== WHP1_HEADER_BYTES) {
      throw new HeddleError(
        'HC_E_PLANE_HEADER_BYTES',
        `header_bytes ${u32[WHP1_OFF_HEADER_BYTES / 4]} != ${WHP1_HEADER_BYTES} (Law 2: one cache line)`,
      );
    }
    if (u32[WHP1_OFF_DATA_START / 4] !== WHP1_DATA_START || u32[WHP1_OFF_DATA_START / 4 + 1] !== 0) {
      throw new HeddleError(
        'HC_E_PLANE_HEADER_BYTES',
        `data_start ${u32[WHP1_OFF_DATA_START / 4]} != fixed ${WHP1_DATA_START}`,
      );
    }
    const planeBytes = u32[WHP1_OFF_PLANE_BYTES / 4] + u32[WHP1_OFF_PLANE_BYTES / 4 + 1] * 2 ** 32;
    if (planeBytes !== buf.byteLength) {
      throw new HeddleError('HC_E_PLANE_HEADER_BYTES', `plane_bytes ${planeBytes} != buffer ${buf.byteLength}`);
    }
    const laneCount = u32[WHP1_OFF_LANE_COUNT / 4];
    if (laneCount < 1 || laneCount > WHP1_MAX_LANES) {
      throw new HeddleError('HC_E_PLANE_LANE_COUNT', `lane_count ${laneCount} outside 1..${WHP1_MAX_LANES}`);
    }
    const lanes: LaneView[] = [];
    let prevEnd = WHP1_DATA_START;
    for (let i = 0; i < laneCount; i++) {
      const byteBase = WHP1_LANE_TABLE + i * WHP1_LANE_STRIDE_TABLE;
      const b = byteBase / 4;
      if (u32[b + WHP1_LANE_OFF_MAGIC / 4] !== WHP1_LANE_MAGIC) {
        throw new HeddleError(
          'HC_E_LANE_MAGIC',
          `lane ${i}: magic 0x${hex(u32[b + WHP1_LANE_OFF_MAGIC / 4])} != 0x${hex(WHP1_LANE_MAGIC)}`,
        );
      }
      const kind = u32[b + WHP1_LANE_OFF_KIND / 4];
      if (HP_KIND_STRIDE[kind] === undefined) {
        throw new HeddleError('HC_E_LANE_KIND', `lane ${i}: unknown kind ${kind}`);
      }
      const dtype = u32[b + WHP1_LANE_OFF_DTYPE / 4];
      if (dtype !== HP_DTYPE.F32) {
        throw new HeddleError('HC_E_LANE_KIND', `lane ${i}: dtype ${dtype} unsupported (v1 renders F32 only)`);
      }
      const granularity = u32[b + WHP1_LANE_OFF_GRANULARITY / 4];
      const offset = u32[b + WHP1_LANE_OFF_OFFSET / 4] + u32[b + WHP1_LANE_OFF_OFFSET / 4 + 1] * 2 ** 32;
      const capacity =
        u32[b + WHP1_LANE_OFF_CAPACITY / 4] + u32[b + WHP1_LANE_OFF_CAPACITY / 4 + 1] * 2 ** 32;
      const stride = u32[b + WHP1_LANE_OFF_STRIDE / 4] + u32[b + WHP1_LANE_OFF_STRIDE / 4 + 1] * 2 ** 32;
      if (capacity <= 0 || capacity > WHP1_MAX_LANE_CAPACITY) {
        throw new HeddleError(
          'HC_E_LANE_CAPACITY',
          `lane ${i}: capacity ${capacity} outside 1..${WHP1_MAX_LANE_CAPACITY}`,
        );
      }
      if (stride !== HP_KIND_STRIDE[kind]) {
        throw new HeddleError(
          'HC_E_LANE_STRIDE',
          `lane ${i}: kind ${kind} requires stride ${HP_KIND_STRIDE[kind]} (Law 2), got ${stride}`,
        );
      }
      validateGranularity(i, capacity, granularity);
      if (offset % 128 !== 0) {
        throw new HeddleError(
          'HC_E_LANE_OFFSET',
          `lane ${i}: offset ${offset} not 128-B aligned (Law 2: GPU storage/vertex binding granularity)`,
        );
      }
      if (offset < prevEnd) {
        throw new HeddleError(
          'HC_E_LANE_OFFSET',
          `lane ${i}: offset ${offset} overlaps lane ${i - 1} (ends at ${prevEnd}) — lanes must be disjoint`,
        );
      }
      if (offset + stride * capacity > planeBytes) {
        throw new HeddleError(
          'HC_E_LANE_OFFSET',
          `lane ${i}: ${offset} + ${stride}×${capacity} = ${offset + stride * capacity} runs past plane end ${planeBytes}`,
        );
      }
      prevEnd = offset + stride * capacity;
      const words = (stride / 4) * capacity;
      lanes.push({
        laneIndex: i,
        kind,
        offset,
        capacity,
        strideBytes: stride,
        f32: new Float32Array(buf, offset, words),
        u32: new Uint32Array(buf, offset, words),
        writePosI32: b + WHP1_LANE_OFF_WRITE_POS / 4,
        seqI32: b + WHP1_LANE_OFF_SEQ / 4,
        dirtyLoI32: b + WHP1_LANE_OFF_DIRTY_LO / 4,
        dirtyHiI32: b + WHP1_LANE_OFF_DIRTY_HI / 4,
        dirtyGranularity: granularity,
      });
    }
    return new HotPlane(buf, new Int32Array(buf), new BigUint64Array(buf), lanes);
  }

  // ---------------------------------------------------------------- hot path

  /** Plane liveness heartbeat (HUD "is the producer alive" probe). */
  epoch(): number {
    return Atomics.load(this.i32, WHP1_OFF_EPOCH / 4);
  }

  /** Teardown flag — the engine drains and stops without throwing. */
  teardownRequested(): boolean {
    return (Atomics.load(this.i32, WHP1_OFF_FLAGS / 4) & 1) !== 0;
  }
}

function validateGranularity(i: number, capacity: number, granularity: number): void {
  // 64 bits × granularity must cover the whole lane (else dirt beyond the
  // last bit is unexpressible — a silent under-render), and granularity
  // must not exceed capacity (else bit 1 already covers everything and
  // the sub-range upload degenerates to full re-upload every frame).
  const minGran = Math.ceil(capacity / 64);
  if (granularity < minGran || granularity > capacity) {
    throw new HeddleError(
      'HC_E_LANE_GRANULARITY',
      `lane ${i}: dirty granularity ${granularity} outside [ceil(${capacity}/64)=${minGran}, ${capacity}]`,
    );
  }
}

function typeName(v: unknown): string {
  if (v === null) return 'null';
  if (typeof v !== 'object') return typeof v;
  if (ArrayBuffer.isView(v)) return (v as object).constructor.name;
  return (v as object).constructor?.name ?? 'object';
}

function hex(n: number): string {
  return (n >>> 0).toString(16).padStart(8, '0');
}

// Re-exported for the engine's draw dispatch (kind → family switch).
export { HP_KIND };
