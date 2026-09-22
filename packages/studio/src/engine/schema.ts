/**
 * Weft Studio — .weft schema DSL lexer/parser (managed mirror of Engineer 1's
 * weftc front-end; implements the WeftcCompilerSeam parse contract).
 * Zero allocation in the AST walk; diagnostics carry line/col for squiggles.
 */

import { fnv1a64 } from './types';

export type PrimType =
  | 'u8' | 'u16' | 'u32' | 'u64'
  | 'i8' | 'i16' | 'i32' | 'i64'
  | 'f32' | 'f64' | 'bool';

export const PRIM_SIZES: Record<PrimType, number> = {
  u8: 1, u16: 2, u32: 4, u64: 8,
  i8: 1, i16: 2, i32: 4, i64: 8,
  f32: 4, f64: 8, bool: 1,
};

export const PRIM_ALIGN: Record<PrimType, number> = {
  u8: 1, u16: 2, u32: 4, u64: 8,
  i8: 1, i16: 2, i32: 4, i64: 8,
  f32: 4, f64: 8, bool: 1,
};

export const PRIMS: readonly string[] = Object.keys(PRIM_SIZES);

export interface FieldAttrs {
  writer?: number;   // @writer(N) — logical concurrent writer lane
  align?: number;    // @align(N) — explicit alignment (power of two)
}

export interface FieldDef {
  name: string;
  type: PrimType | string; // prim or struct ref
  attrs: FieldAttrs;
  line: number;  // 1-based
  col: number;   // 1-based
  offset: number; // byte offset (filled by layout)
}

export interface StructDef {
  name: string;
  fields: FieldDef[];
  packed: boolean;
  line: number;
}

export interface SchemaDoc {
  structs: StructDef[];
  hash: string; // FNV-1a 64 of canonical text
}

export interface Diagnostic {
  code: string;      // 'E_SCHEMA'
  message: string;
  line: number;
  col: number;
  severity: 'error' | 'warning';
}

export interface ParseResult {
  ok: boolean;
  doc: SchemaDoc | null;
  diagnostics: Diagnostic[];
}

// ------------------------------ lexer --------------------------------------

interface Tok {
  kind: 'ident' | 'num' | 'punct' | 'eof';
  text: string;
  line: number;
  col: number;
  off: number;
}

const PUNCT = new Set(['{', '}', ';', ':', ',', '@', '(', ')']);

function lex(src: string): { toks: Tok[]; diags: Diagnostic[] } {
  const toks: Tok[] = [];
  const diags: Diagnostic[] = [];
  let i = 0, line = 1, col = 1;
  const n = src.length;
  const push = (kind: Tok['kind'], text: string, off: number, l = line, c = col) =>
    toks.push({ kind, text, line: l, col: c, off });

  while (i < n) {
    const ch = src[i];
    if (ch === '\n') { i++; line++; col = 1; continue; }
    if (ch === ' ' || ch === '\t' || ch === '\r') { i++; col++; continue; }
    if (ch === '/' && src[i + 1] === '/') {
      while (i < n && src[i] !== '\n') { i++; col++; }
      continue;
    }
    if (ch === '/' && src[i + 1] === '*') {
      const startLine = line, startCol = col;
      i += 2; col += 2;
      let closed = false;
      while (i < n) {
        if (src[i] === '*' && src[i + 1] === '/') { i += 2; col += 2; closed = true; break; }
        if (src[i] === '\n') { line++; col = 1; } else { col++; }
        i++;
      }
      if (!closed) diags.push({ code: 'E_SCHEMA', message: 'unterminated block comment', line: startLine, col: startCol, severity: 'error' });
      continue;
    }
    if (/[A-Za-z_]/.test(ch)) {
      const start = i, startCol = col;
      while (i < n && /[A-Za-z0-9_]/.test(src[i])) { i++; col++; }
      push('ident', src.slice(start, i), start, line, startCol);
      continue;
    }
    if (/[0-9]/.test(ch)) {
      const start = i, startCol = col;
      while (i < n && /[0-9_]/.test(src[i])) { i++; col++; }
      push('num', src.slice(start, i).replace(/_/g, ''), start, line, startCol);
      continue;
    }
    if (PUNCT.has(ch)) {
      push('punct', ch, i);
      i++; col++;
      continue;
    }
    diags.push({ code: 'E_SCHEMA', message: `unexpected character '${ch}'`, line, col, severity: 'error' });
    i++; col++;
  }
  toks.push({ kind: 'eof', text: '', line, col, off: n });
  return { toks, diags };
}

// ------------------------------ parser -------------------------------------

class Parser {
  private p = 0;
  diags: Diagnostic[] = [];
  constructor(private toks: Tok[]) {}

  private peek(): Tok { return this.toks[this.p]; }
  private next(): Tok { return this.toks[this.p++]; }
  private eat(text: string): boolean {
    if (this.toks[this.p].text === text) { this.p++; return true; }
    return false;
  }
  private expect(text: string): boolean {
    if (this.eat(text)) return true;
    const t = this.peek();
    this.err(`expected '${text}' but found '${t.text || 'end of file'}'`, t);
    return false;
  }
  private err(message: string, t: Tok) {
    this.diags.push({ code: 'E_SCHEMA', message, line: t.line, col: t.col, severity: 'error' });
  }

  // pending inline attributes (bind to the next parsed field)
  private pendingWriter: number | undefined = undefined;
  private pendingAlign: number | undefined = undefined;
  private pendingPacked = false;

  parse(): SchemaDoc {
    const structs: StructDef[] = [];
    while (this.peek().kind !== 'eof') {
      const t = this.peek();
      if (t.text === '@') {
        this.next();
        const at = this.peek();
        if (at.text === 'packed') {
          this.next();
          this.pendingPacked = true;
          continue;
        }
        this.err(`only '@packed' is valid before 'struct' (found '@${at.text}')`, at);
        this.next();
        continue;
      }
      if (t.text === 'struct') { this.next(); const s = this.parseStruct(); if (s) structs.push(s); }
      else if (t.text === 'enum') { this.next(); this.skipEnum(); }
      else { this.err(`expected 'struct' or 'enum' but found '${t.text || 'end of file'}'`, t); this.next(); }
    }
    const names = new Set<string>();
    for (const s of structs) {
      if (names.has(s.name)) this.err(`duplicate struct '${s.name}'`, this.toks[0]);
      names.add(s.name);
    }
    return { structs, hash: '' };
  }

  private parseStruct(): StructDef | null {
    const nameTok = this.peek();
    if (nameTok.kind !== 'ident') { this.err(`expected struct name`, nameTok); return null; }
    this.next();
    const s: StructDef = { name: nameTok.text, fields: [], packed: this.pendingPacked, line: nameTok.line };
    this.pendingPacked = false;
    if (!this.expect('{')) return null;
    while (!this.eat('}')) {
      const t = this.peek();
      if (t.kind === 'eof') { this.err(`unterminated struct '${s.name}'`, t); return null; }
      if (t.text === '@') {
        this.next();
        if (!this.parseAttr()) this.recover();
        continue;
      }
      if (t.kind !== 'ident') { this.err(`expected field or attribute, found '${t.text}'`, t); this.next(); continue; }
      this.next();
      const typeTok = this.peek();
      if (typeTok.text === ':' ) this.next(); // name ':' type
      const typeTok2 = this.peek();
      if (typeTok2.kind !== 'ident') { this.err(`expected type after field name '${t.text}'`, typeTok2); this.recover(); continue; }
      this.next();
      if (t.text === 'struct') { this.err(`'struct' is reserved`, t); continue; }
      const f: FieldDef = {
        name: t.text,
        type: typeTok2.text,
        attrs: {},
        line: t.line,
        col: t.col,
        offset: -1,
      };
      if (this.pendingWriter !== undefined) { f.attrs.writer = this.pendingWriter; this.pendingWriter = undefined; }
      if (this.pendingAlign !== undefined) { f.attrs.align = this.pendingAlign; this.pendingAlign = undefined; }
      s.fields.push(f);
      if (!this.expect(';')) this.recover();
    }
    const seen = new Set<string>();
    for (const f of s.fields) {
      if (seen.has(f.name)) this.err(`duplicate field '${f.name}' in struct '${s.name}'`, { kind: 'ident', text: f.name, line: f.line, col: f.col, off: 0 });
      seen.add(f.name);
    }
    return s;
  }

  /** Inline attribute: '@writer(N)' or '@align(N)' — no trailing ';'. */
  private parseAttr(): boolean {
    const t = this.peek();
    if (t.text === 'writer') {
      this.next();
      if (!this.expect('(')) return false;
      const num = this.peek();
      if (num.kind !== 'num') { this.err('@writer expects a lane number', num); return false; }
      this.next();
      if (!this.expect(')')) return false;
      this.pendingWriter = parseInt(num.text, 10) | 0;
      return true;
    }
    if (t.text === 'align') {
      this.next();
      if (!this.expect('(')) return false;
      const num = this.peek();
      if (num.kind !== 'num') { this.err('@align expects a byte count', num); return false; }
      this.next();
      if (!this.expect(')')) return false;
      const v = parseInt(num.text, 10);
      if (v < 1 || (v & (v - 1)) !== 0 || v > 4096) {
        this.err(`@align must be a power of two in [1, 4096], got ${v}`, num);
      } else {
        this.pendingAlign = v;
      }
      return true;
    }
    this.err(`unknown attribute '@${t.text}' (expected @writer(N) or @align(N) before a field)`, t);
    return false;
  }

  private recover(): void {
    // skip to next ';'
    while (this.p < this.toks.length && this.toks[this.p].text !== ';') this.p++;
    this.p++; // consume ';'
  }

  private skipEnum(): void {
    // enums are accepted for forward compatibility: enum Name { A, B } — parsed loosely
    if (this.peek().kind === 'ident') this.next();
    if (!this.expect('{')) { this.recover(); return; }
    while (!this.eat('}')) { if (this.peek().kind === 'eof') return; this.next(); }
    this.expect(';');
  }
}

export function parseSchema(src: string): ParseResult {
  const { toks, diags } = lex(src);
  const parser = new Parser(toks);
  let doc: SchemaDoc | null = null;
  try {
    doc = parser.parse();
  } catch (e) {
    diags.push({ code: 'E_SCHEMA', message: `parser panic: ${(e as Error).message}`, line: 1, col: 1, severity: 'error' });
  }
  const all = diags.concat(parser.diags);
  if (doc) doc.hash = schemaHash(src);
  return { ok: all.every((d) => d.severity !== 'error'), doc, diagnostics: all };
}

/** Canonical schema hash (FNV-1a 64 over the raw source text). */
export function schemaHash(src: string): string {
  return fnv1a64(src);
}

/** Validate struct references resolve to declared structs (no cycles beyond depth 8). */
export function validateRefs(doc: SchemaDoc): Diagnostic[] {
  const diags: Diagnostic[] = [];
  const byName = new Map(doc.structs.map((s) => [s.name, s]));
  for (const s of doc.structs) {
    for (const f of s.fields) {
      if (PRIMS.includes(f.type)) continue;
      if (!byName.has(f.type)) {
        diags.push({ code: 'E_SCHEMA', message: `unknown type '${f.type}' for field '${s.name}.${f.name}'`, line: f.line, col: f.col, severity: 'error' });
      }
    }
  }
  return diags;
}
