// ir.mjs — weftc IR v1 loader + fail-closed validator.
//
// This module is the ONLY way backends obtain schema data. It enforces every
// rule in tools/weftc/schema/README.md §3 and rejects everything it does not
// understand (TIER4 input-wall style: unknown keys, wrong endian, overlapping
// or misaligned fields are all hard errors, never warnings).
//
// Zero dependencies; ESM; Node >= 18.

import { TYPES } from './types.mjs';

export class IrError extends Error {
  constructor(message) {
    super(`ir-v1: ${message}`);
    this.name = 'IrError';
  }
}

function isPlainObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

function checkKeys(obj, allowed, where) {
  for (const k of Object.keys(obj)) {
    if (!allowed.has(k)) throw new IrError(`${where}: unknown key "${k}"`);
  }
}

const DOC_KEYS = new Set(['irVersion', 'generator', 'schema']);
const STRUCT_KEYS = new Set([
  'name', 'namespace', 'schemaId', 'schemaVersion', 'byteLength', 'alignment',
  'endian', 'doc', 'fields',
]);
const FIELD_KEYS = new Set(['name', 'type', 'offset', 'count', 'const', 'role', 'doc']);
const TYPE_NAMES = new Set(Object.keys(TYPES));
const ROLES = new Set([
  'magic', 'schemaId', 'schemaVersion', 'headerSize', 'payloadLen', 'seq', 'reserved',
]);

/** Parse a u64 const: hex string (lossless) or safe integer. Returns BigInt. */
function parseU64(v, where) {
  if (typeof v === 'string' && /^0x[0-9a-fA-F]{1,16}$/.test(v)) {
    return BigInt(v);
  }
  if (typeof v === 'number' && Number.isSafeInteger(v) && v >= 0) {
    return BigInt(v);
  }
  throw new IrError(`${where}: u64 const must be a "0x…" hex string or a safe non-negative integer`);
}

function parseIntConst(v, bits, signed, where) {
  if (typeof v !== 'number' || !Number.isSafeInteger(v)) {
    throw new IrError(`${where}: const must be an integer (received ${typeof v})`);
  }
  if (signed) {
    const lo = -(2 ** (bits - 1));
    const hi = 2 ** (bits - 1) - 1;
    if (v < lo || v > hi) throw new IrError(`${where}: const ${v} out of i${bits} range`);
  } else {
    if (v < 0 || v > 2 ** bits - 1) throw new IrError(`${where}: const ${v} out of u${bits} range`);
  }
  return v;
}

function asciiBytes(v, where) {
  if (typeof v !== 'string') throw new IrError(`${where}: u8[] const must be an ASCII string`);
  const bytes = [];
  for (let i = 0; i < v.length; i++) {
    const c = v.charCodeAt(i);
    if (c > 0x7f) throw new IrError(`${where}: const string must be ASCII`);
    bytes.push(c);
  }
  return bytes;
}

/**
 * Validate and normalize an IR document.
 * @returns normalized struct: { name, namespace, schemaId (BigInt|null),
 *   schemaVersion, byteLength, alignment, endian, doc, fields[] } where each
 *   field carries resolved { name, type, offset, count, size, align, const,
 *   role, doc } and `const` is pre-parsed into { kind, bytes?, value? }.
 */
export function loadIr(doc) {
  if (!isPlainObject(doc)) throw new IrError('document must be a JSON object');
  checkKeys(doc, DOC_KEYS, 'root');
  if (doc.irVersion !== 1) throw new IrError(`unsupported irVersion ${JSON.stringify(doc.irVersion)}`);
  if (doc.generator !== undefined && typeof doc.generator !== 'string') {
    throw new IrError('generator must be a string');
  }
  return normalizeStruct(doc.schema, 'schema');
}

function normalizeStruct(s, where) {
  if (!isPlainObject(s)) throw new IrError(`${where} must be an object`);
  checkKeys(s, STRUCT_KEYS, where);
  if (typeof s.name !== 'string' || !/^[A-Z][A-Za-z0-9]*$/.test(s.name)) {
    throw new IrError(`${where}.name must be PascalCase`);
  }
  if (!Number.isSafeInteger(s.byteLength) || s.byteLength < 1) {
    throw new IrError(`${where}.byteLength must be a positive integer`);
  }
  // Law 2 is structural: little-endian or nothing.
  if (s.endian !== 'little') throw new IrError(`${where}.endian must be "little" (Law 2)`);

  const alignment = s.alignment ?? 8;
  if (![1, 2, 4, 8].includes(alignment)) {
    throw new IrError(`${where}.alignment must be one of 1|2|4|8`);
  }
  if (s.schemaId !== undefined) {
    if (typeof s.schemaId !== 'string' || !/^0x[0-9a-fA-F]{1,16}$/.test(s.schemaId)) {
      throw new IrError(`${where}.schemaId must be a "0x…" hex string`);
    }
  }
  if (s.namespace !== undefined && typeof s.namespace !== 'string') {
    throw new IrError(`${where}.namespace must be a string`);
  }

  if (!Array.isArray(s.fields) || s.fields.length < 1) {
    throw new IrError(`${where}.fields must be a non-empty array`);
  }

  const fields = s.fields.map((f, i) => normalizeField(f, `${where}.fields[${i}]`));

  // Rule 3/4: in-bounds + disjoint byte ranges.
  const sorted = [...fields].sort((a, b) => a.offset - b.offset);
  for (const f of sorted) {
    if (f.offset + f.size > s.byteLength) {
      throw new IrError(
        `field "${f.name}" [${f.offset}..${f.offset + f.size}) exceeds byteLength ${s.byteLength}`,
      );
    }
  }
  // Authoritative pairwise overlap check (n is small; clarity over cleverness).
  for (let i = 0; i < sorted.length; i++) {
    for (let j = i + 1; j < sorted.length; j++) {
      const a = sorted[i];
      const b = sorted[j];
      if (b.offset < a.offset + a.size) {
        throw new IrError(`fields "${a.name}" and "${b.name}" overlap at offset ${b.offset}`);
      }
    }
  }
  // Rule 5: natural alignment.
  for (const f of fields) {
    if (f.align > 1 && f.offset % f.align !== 0) {
      throw new IrError(
        `field "${f.name}" (${f.type}) at offset ${f.offset} is not ${f.align}-byte aligned`,
      );
    }
  }

  return {
    name: s.name,
    namespace: s.namespace ?? null,
    schemaId: s.schemaId ? BigInt(s.schemaId) : null,
    schemaVersion: s.schemaVersion ?? null,
    byteLength: s.byteLength,
    alignment,
    endian: 'little',
    doc: s.doc ?? null,
    fields,
  };
}

function normalizeField(f, where) {
  if (!isPlainObject(f)) throw new IrError(`${where} must be an object`);
  checkKeys(f, FIELD_KEYS, where);
  if (typeof f.name !== 'string' || !/^[A-Za-z][A-Za-z0-9_]*$/.test(f.name)) {
    throw new IrError(`${where}.name must match [A-Za-z][A-Za-z0-9_]*`);
  }
  const t = TYPES[f.type];
  if (!t) throw new IrError(`${where}.type "${f.type}" is not a known scalar`);
  const count = f.count ?? 1;
  if (!Number.isSafeInteger(count) || count < 1) {
    throw new IrError(`${where}.count must be a positive integer`);
  }
  if (!Number.isSafeInteger(f.offset) || f.offset < 0) {
    throw new IrError(`${where}.offset must be a non-negative integer`);
  }
  if (f.role !== undefined && !ROLES.has(f.role)) {
    throw new IrError(`${where}.role "${f.role}" is not a known role`);
  }

  let kconst = null;
  if (f.const !== undefined) {
    // u8 arrays may carry an ASCII-string const (frame magic) — check FIRST,
    // before scalar int parsing, since "WEFT" is not a numeric literal.
    if (f.type === 'u8' && count > 1 && typeof f.const === 'string') {
      kconst = { kind: 'bytes', bytes: asciiBytes(f.const, `${where}.const`) };
    } else if (t.int) {
      if (f.type === 'u64' || f.type === 'i64') {
        const v = parseU64(f.const, `${where}.const`);
        if (f.type === 'i64' && v >= 2n ** 63n) throw new IrError(`${where}.const out of i64 range`);
        kconst = { kind: 'int', value: v };
      } else if (f.type === 'bool') {
        if (typeof f.const !== 'number' || ![0, 1].includes(f.const)) {
          throw new IrError(`${where}.const for bool must be 0 or 1`);
        }
        kconst = { kind: 'int', value: f.const };
      } else if (typeof f.const === 'string' && /^0x[0-9a-fA-F]+$/.test(f.const)) {
        kconst = { kind: 'int', value: Number(BigInt(f.const)) };
      } else {
        kconst = {
          kind: 'int',
          value: parseIntConst(f.const, t.size * 8, !t.unsigned, `${where}.const`),
        };
      }
    } else if (t.float) {
      if (typeof f.const !== 'number' || !Number.isFinite(f.const)) {
        throw new IrError(`${where}.const must be a finite number`);
      }
      kconst = { kind: 'float', value: f.const };
    } else {
      throw new IrError(`${where}.const is not supported for this type`);
    }
  }

  return {
    name: f.name,
    type: f.type,
    offset: f.offset,
    count,
    size: t.size * count,
    align: t.align,
    const: kconst,
    role: f.role ?? null,
    doc: f.doc ?? null,
  };
}

/** Read + validate an IR document from a JSON string. */
export function parseIrJson(text) {
  let doc;
  try {
    doc = JSON.parse(text);
  } catch (e) {
    throw new IrError(`invalid JSON: ${e.message}`);
  }
  return loadIr(doc);
}
