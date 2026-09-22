// hot.ts — hot-region extraction.

export interface HotRegion {
  /** 0-based first line of the annotation. */
  annotateLine: number;
  /** 0-based line holding the body-opening brace (signature line). */
  sigLine: number;
  /** 0-based first line of the body (same as sigLine). */
  startLine: number;
  /** 0-based last line of the body (inclusive). */
  endLine: number;
  /** True when no braced body was found within the window (fallback region). */
  fallback: boolean;
}

export interface HotConfig {
  /** Annotation regex, applied per line on the semi-masked view. */
  annotationRe: RegExp;
  /** How many lines after the annotation to look for the body brace. */
  window: number;
  /** When set, the text BEFORE the match must fully match this pattern
   *  (whitespace + comment markers only) so prose mentions of the
   *  annotation mid-sentence never open a hot region. Languages whose
   *  annotation forms are unambiguous (C attribute syntax) may omit it. */
  prefixRe?: RegExp;
}

/**
 * Words that may legitimately follow a hot annotation on the same line
 * (signature starters for attribute-prefixed declarations). Any other
 * trailing word means the match is prose (e.g. a comment talking ABOUT
 * the annotation) and must not open a hot region.
 */
const SIGNATURE_STARTERS = new Set([
  'fn', 'pub', 'func', 'void', 'static', 'inline', 'export', 'function',
  'class', 'final', 'private', 'public', 'protected', 'override', 'def',
  'suspend', 'operator', 'extern', 'unsafe', 'async', 'const', 'let',
  'var', 'int', 'mut', 'fun', 'internal', 'sealed',
]);

/** True when the annotation match on this line is a real annotation. */
function isRealAnnotation(lineText: string, matchEnd: number): boolean {
  const rest = lineText.slice(matchEnd).trim();
  if (rest.length === 0) return true;
  if (/^[\s*/]+$/.test(rest)) return true;
  const word = /^[A-Za-z_]\w*/.exec(rest);
  if (word === null) return true; // punctuation like '(' — attribute args
  return SIGNATURE_STARTERS.has(word[0]);
}

function findBodyEnd(maskedLines: string[], sigLine: number): number {
  const line = maskedLines[sigLine] ?? '';
  const openIdx = line.indexOf('{');
  if (openIdx < 0) return sigLine; // should not happen (caller guarantees)
  let depth = 0;
  for (let k = sigLine; k < maskedLines.length; k++) {
    const text = maskedLines[k] ?? '';
    const from = k === sigLine ? openIdx : 0;
    for (let c = from; c < text.length; c++) {
      const ch = text[c];
      if (ch === '{') depth++;
      else if (ch === '}') {
        depth--;
        if (depth === 0) return k;
      }
    }
  }
  return maskedLines.length - 1; // unterminated: treat rest of file as body
}

export function findHotRegions(semiMasked: string, masked: string, cfg: HotConfig): HotRegion[] {
  const semiLines = semiMasked.split('\n');
  const maskedLines = masked.split('\n');
  const regions: HotRegion[] = [];

  for (let line = 0; line < semiLines.length; line++) {
    const text = semiLines[line] ?? '';
    cfg.annotationRe.lastIndex = 0;
    const m = cfg.annotationRe.exec(text);
    if (m === null) continue;
    if (!isRealAnnotation(text, m.index + m[0].length)) continue;
    if (cfg.prefixRe !== undefined && !cfg.prefixRe.test(text.slice(0, m.index))) continue;

    let sigLine = -1;
    const limit = Math.min(line + cfg.window, maskedLines.length - 1);
    for (let k = line; k <= limit; k++) {
      if ((maskedLines[k] ?? '').indexOf('{') >= 0) {
        sigLine = k;
        break;
      }
    }
    if (sigLine < 0) {
      regions.push({
        annotateLine: line,
        sigLine: Math.min(line + 1, maskedLines.length - 1),
        startLine: line,
        endLine: Math.min(line + 1, maskedLines.length - 1),
        fallback: true,
      });
      continue;
    }
    const endLine = findBodyEnd(maskedLines, sigLine);
    regions.push({ annotateLine: line, sigLine, startLine: sigLine, endLine, fallback: false });
  }

  if (regions.length <= 1) return regions;
  regions.sort((a, b) => a.startLine - b.startLine || a.endLine - b.endLine);
  const merged: HotRegion[] = [];
  for (const r of regions) {
    const last = merged[merged.length - 1];
    if (last !== undefined && r.startLine <= last.endLine + 1) {
      last.endLine = Math.max(last.endLine, r.endLine);
      last.fallback = last.fallback && r.fallback;
    } else {
      merged.push({ ...r });
    }
  }
  return merged;
}
