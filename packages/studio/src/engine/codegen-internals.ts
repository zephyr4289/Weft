/**
 * Weft Studio — codegen internals: doc-scoped struct resolution helpers.
 * WeakMap-cached so repeated codegen passes avoid rebuilding resolution maps.
 */

import { SchemaDoc, StructDef, FieldDef, PRIM_SIZES, PRIM_ALIGN, PRIMS } from './schema';
import { alignUp } from './layout';

const RES_CACHE = new WeakMap<SchemaDoc, Map<string, StructDef>>();

function resolutionMap(doc: SchemaDoc): Map<string, StructDef> {
  let m = RES_CACHE.get(doc);
  if (!m) {
    m = new Map(doc.structs.map((s) => [s.name, s]));
    RES_CACHE.set(doc, m);
  }
  return m;
}

export function fieldSize(st: StructDef, f: FieldDef, doc: SchemaDoc): number {
  void st;
  if (PRIMS.includes(f.type)) return PRIM_SIZES[f.type as keyof typeof PRIM_SIZES];
  const sub = resolutionMap(doc).get(f.type);
  if (!sub) return 0;
  return structSize(sub, doc);
}

export function fieldAlign(st: StructDef, f: FieldDef, doc: SchemaDoc): number {
  void st;
  if (f.attrs.align) return f.attrs.align;
  if (PRIMS.includes(f.type)) return PRIM_ALIGN[f.type as keyof typeof PRIM_ALIGN];
  const sub = resolutionMap(doc).get(f.type);
  return sub ? structAlignment(sub, doc) : 1;
}

export function structAlignment(s: StructDef, doc: SchemaDoc): number {
  let a = 1;
  for (const f of s.fields) a = Math.max(a, fieldAlign(s, f, doc));
  return a;
}

export function structSize(s: StructDef, doc: SchemaDoc): number {
  let off = 0;
  let maxA = 1;
  for (const f of s.fields) {
    const a = fieldAlign(s, f, doc);
    const sz = fieldSize(s, f, doc);
    off = alignUp(off, a) + sz;
    maxA = Math.max(maxA, a);
  }
  if (s.packed) return off;
  return alignUp(off, maxA);
}
