/**
 * Weft Studio — .weft syntax highlighter + gutter info markers.
 * Line-oriented tokenizer producing highlighted HTML; diagnostics become
 * squiggle spans and gutter markers. Runs per keystroke on the cold plane.
 */

import { STUDIO_THEME as T } from './theme';

export interface TokSpan {
  text: string;
  cls: 'kw' | 'prim' | 'num' | 'attr' | 'comment' | 'field' | 'type' | 'punct' | 'plain';
}

const KEYWORDS = new Set(['struct', 'enum', 'packed']);
const PRIMS = new Set(['u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'f32', 'f64', 'bool']);

/** Tokenize one physical line (block comments handled by caller state). */
export function tokenizeLine(line: string, inBlockComment: boolean): { spans: TokSpan[]; inBlockComment: boolean } {
  const spans: TokSpan[] = [];
  let i = 0;
  let comment = inBlockComment;
  const push = (text: string, cls: TokSpan['cls']) => { if (text) spans.push({ text, cls }); };

  while (i < line.length) {
    if (comment) {
      const end = line.indexOf('*/', i);
      if (end < 0) { push(line.slice(i), 'comment'); i = line.length; }
      else { push(line.slice(i, end + 2), 'comment'); i = end + 2; comment = false; }
      continue;
    }
    const ch = line[i];
    if (ch === ' ' || ch === '\t') { push(ch, 'plain'); i++; continue; }
    if (ch === '/' && line[i + 1] === '/') { push(line.slice(i), 'comment'); break; }
    if (ch === '/' && line[i + 1] === '*') {
      comment = true;
      continue;
    }
    if (ch === '@') {
      let j = i + 1;
      while (j < line.length && /[A-Za-z0-9_]/.test(line[j])) j++;
      push(line.slice(i, j), 'attr');
      i = j;
      continue;
    }
    if (/[0-9]/.test(ch)) {
      let j = i;
      while (j < line.length && /[0-9]/.test(line[j])) j++;
      push(line.slice(i, j), 'num');
      i = j;
      continue;
    }
    if (/[A-Za-z_]/.test(ch)) {
      let j = i;
      while (j < line.length && /[A-Za-z0-9_]/.test(line[j])) j++;
      const word = line.slice(i, j);
      const isColonType = line[j] === ':'; // name position
      if (KEYWORDS.has(word)) push(word, 'kw');
      else if (PRIMS.has(word)) push(word, 'prim');
      else if (isColonType) push(word, 'field');
      else push(word, 'type');
      i = j;
      continue;
    }
    push(ch, 'punct');
    i++;
  }
  return { spans, inBlockComment: comment };
}

const CLS_COLOR: Record<TokSpan['cls'], string> = {
  kw: T.tokKeyword,
  prim: T.tokPrim,
  num: T.tokNumber,
  attr: T.tokAttr,
  comment: T.tokComment,
  field: T.tokField,
  type: T.tokType,
  punct: T.tokPunct,
  plain: T.tokText,
};

function esc(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

/** Full-document highlight → array of line HTML strings. */
export function highlightDocument(src: string): string[] {
  const lines = src.split('\n');
  const out: string[] = [];
  let inBlock = false;
  for (const line of lines) {
    const { spans, inBlockComment } = tokenizeLine(line, inBlock);
    inBlock = inBlockComment;
    let html = '';
    for (const s of spans) {
      if (s.cls === 'plain') html += esc(s.text);
      else html += `<span style="color:${CLS_COLOR[s.cls]}">${esc(s.text)}</span>`;
    }
    out.push(html || '&nbsp;');
  }
  return out;
}

/** Editor autocomplete vocabulary (keyword + prim + attr + struct names). */
export const COMPLETIONS: readonly string[] = [
  'struct', 'packed', '@writer(', '@align(',
  'u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'f32', 'f64', 'bool',
];

export function completionsFor(prefix: string, structNames: string[]): string[] {
  const vocab = COMPLETIONS.concat(structNames);
  if (!prefix) return vocab.slice(0, 12);
  return vocab.filter((v) => v.startsWith(prefix) && v !== prefix).slice(0, 12);
}
