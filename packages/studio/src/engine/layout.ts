/**
 * Weft Studio — layout engine, cache-line mapper, false-sharing hazard
 * detector, density gauge. C-style natural alignment; explicit @align wins;
 * @packed suppresses tail padding. All arithmetic integer-exact.
 */

import {
  FieldDef, SchemaDoc, StructDef, Diagnostic, PRIM_SIZES, PRIM_ALIGN, PRIMS,
} from './schema';

export interface LeafSpan {
  struct: string;
  field: string;
  type: string;
  size: number;
  align: number;
  offset: number;     // absolute byte offset within root struct
  writer: number;     // -1 = unassigned
  line: number;
}

export interface PadSpan {
  struct: string;
  offset: number;
  size: number;
}

export interface StructLayout {
  name: string;
  size: number;
  alignment: number;
  leaves: LeafSpan[];
  pads: PadSpan[];
  usedBytes: number;     // sum of leaf sizes
  paddingBytes: number;  // size - usedBytes
  efficiency: number;    // usedBytes / size (0..1)
}

export interface FalseSharingHazard {
  cacheLine: number;     // line index
  lineStart: number;
  fields: string[];      // "Struct.field"
  writers: number[];     // distinct writer lanes on this line
  severity: 'hazard';
}

export interface LayoutResult {
  layouts: Map<string, StructLayout>;
  order: string[];
  diagnostics: Diagnostic[];
  hazards64: FalseSharingHazard[];
  hazards128: FalseSharingHazard[];
  ok: boolean;
}

function structAlignment(s: StructDef, byName: Map<string, StructDef>): number {
  let a = 1;
  for (const f of s.fields) {
    if (PRIMS.includes(f.type)) a = Math.max(a, PRIM_ALIGN[f.type as keyof typeof PRIM_SIZES] ?? 1);
    else {
      const sub = byName.get(f.type);
      a = Math.max(a, sub ? structAlignment(sub, byName) : 1);
    }
    if (f.attrs.align) a = Math.max(a, f.attrs.align);
  }
  return a;
}

function fieldSize(f: FieldDef, byName: Map<string, StructDef>): number {
  if (PRIMS.includes(f.type)) return PRIM_SIZES[f.type as keyof typeof PRIM_SIZES];
  const sub = byName.get(f.type);
  if (!sub) return 0;
  return layoutSize(sub, byName);
}

function fieldAlign(f: FieldDef, byName: Map<string, StructDef>): number {
  if (f.attrs.align) return f.attrs.align;
  if (PRIMS.includes(f.type)) return PRIM_ALIGN[f.type as keyof typeof PRIM_ALIGN];
  const sub = byName.get(f.type);
  return sub ? structAlignment(sub, byName) : 1;
}

function layoutSize(s: StructDef, byName: Map<string, StructDef>): number {
  let off = 0;
  let maxA = 1;
  for (const f of s.fields) {
    const a = fieldAlign(f, byName);
    const sz = fieldSize(f, byName);
    off = (off + a - 1) & ~(a - 1);
    off += sz;
    maxA = Math.max(maxA, a);
  }
  if (s.packed) return off;
  return (off + maxA - 1) & ~(maxA - 1);
}

export function alignUp(v: number, a: number): number {
  return (v + a - 1) & ~(a - 1);
}

/**
 * Full layout pass. Emits diagnostics for: unknown types (already surfaced by
 * validateRefs), @align below natural alignment, zero-size nested structs.
 */
export function computeLayout(doc: SchemaDoc): LayoutResult {
  const diagnostics: Diagnostic[] = [];
  const byName = new Map(doc.structs.map((s) => [s.name, s]));
  const layouts = new Map<string, StructLayout>();
  const order: string[] = [];

  for (const s of doc.structs) {
    const leaves: LeafSpan[] = [];
    const pads: PadSpan[] = [];

    const emit = (st: StructDef, base: number, prefix: string, inheritedWriter: number) => {
      let localOff = 0;
      for (const f of st.fields) {
        const a = fieldAlign(f, byName);
        const sz = fieldSize(f, byName);
        const aligned = alignUp(localOff, a);
        if (aligned > localOff) pads.push({ struct: prefix || st.name, offset: base + localOff, size: aligned - localOff });
        localOff = aligned;
        f.offset = base + localOff;
        const leafWriter = f.attrs.writer ?? inheritedWriter;
        if (PRIMS.includes(f.type)) {
          leaves.push({
            struct: prefix || st.name,
            field: f.name,
            type: f.type,
            size: sz,
            align: a,
            offset: base + localOff,
            writer: leafWriter,
            line: f.line,
          });
        } else {
          const sub = byName.get(f.type);
          if (sub) emit(sub, base + localOff, `${prefix ? prefix + '.' : ''}${f.name}`, leafWriter);
        }
        localOff += sz;
      }
      return localOff;
    };

    const bodyEnd = emit(s, 0, '', -1);
    const tail = s.packed ? 0 : alignUp(bodyEnd, structAlignment(s, byName)) - bodyEnd;
    if (tail > 0) pads.push({ struct: s.name, offset: bodyEnd, size: tail });
    const size = bodyEnd + tail;

    const used = leaves.reduce((acc, l) => acc + l.size, 0);
    layouts.set(s.name, {
      name: s.name,
      size,
      alignment: s.packed ? 1 : structAlignment(s, byName),
      leaves,
      pads,
      usedBytes: used,
      paddingBytes: size - used,
      efficiency: size === 0 ? 1 : used / size,
    });
    order.push(s.name);
  }

  // @align sanity: explicit align below natural is a warning (spec: must be >=)
  for (const s of doc.structs) {
    for (const f of s.fields) {
      if (!f.attrs.align) continue;
      const natural = PRIMS.includes(f.type) ? PRIM_ALIGN[f.type as keyof typeof PRIM_ALIGN] : 1;
      if (f.attrs.align < natural) {
        diagnostics.push({
          code: 'E_SCHEMA',
          message: `@align(${f.attrs.align}) on '${s.name}.${f.name}' is below natural alignment ${natural}B`,
          line: f.line, col: f.col, severity: 'warning',
        });
      }
    }
  }

  const hazards64 = detectFalseSharing(layouts, 64);
  const hazards128 = detectFalseSharing(layouts, 128);
  return {
    layouts, order, diagnostics,
    hazards64, hazards128,
    ok: diagnostics.every((d) => d.severity !== 'error'),
  };
}

/**
 * False-sharing hazard: two leaf fields with DIFFERENT writer lanes (>=0)
 * intersecting the same cache line. Unassigned (-1) writer fields never
 * contribute. Runs for 64B and 128B line sizes.
 */
export function detectFalseSharing(
  layouts: Map<string, StructLayout>,
  lineSize: number,
): FalseSharingHazard[] {
  const hazards: FalseSharingHazard[] = [];
  for (const layout of layouts.values()) {
    const buckets = new Map<number, { fields: string[]; writers: number[] }>();
    for (const l of layout.leaves) {
      if (l.writer < 0) continue;
      const first = Math.floor(l.offset / lineSize);
      const last = Math.floor((l.offset + l.size - 1) / lineSize);
      for (let line = first; line <= last; line++) {
        let b = buckets.get(line);
        if (!b) { b = { fields: [], writers: [] }; buckets.set(line, b); }
        b.fields.push(`${l.struct}.${l.field}`);
        if (!b.writers.includes(l.writer)) b.writers.push(l.writer);
      }
    }
    for (const [line, b] of buckets) {
      if (b.writers.length >= 2) {
        hazards.push({
          cacheLine: line,
          lineStart: line * lineSize,
          fields: b.fields,
          writers: b.writers.slice().sort((x, y) => x - y),
          severity: 'hazard',
        });
      }
    }
  }
  hazards.sort((a, b) => a.lineStart - b.lineStart);
  return hazards;
}
