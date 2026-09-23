// mask.ts — lexical masking of comments and string literals.

export interface StringRule {
  open: string;
  close: string;
  escape: boolean;
  rawPrefixes?: string[];
}

export interface BlockCommentRule {
  open: string;
  close: string;
  nested: boolean;
}

export interface MaskConfig {
  lineComments: string[];
  blockComments: BlockCommentRule[];
  strings: StringRule[];
  tripleQuote?: string;
}

export interface MaskOptions {
  /** Keep comment text visible (semi-mask mode for annotation detection). */
  keepCommentText?: boolean;
}

const SENTINEL_NEWLINE = '\n';

export function maskCommentsAndStrings(src: string, cfg: MaskConfig, opts: MaskOptions = {}): string {
  const out = src.split('');
  const n = src.length;
  const keepComments = opts.keepCommentText === true;

  const blank = (from: number, to: number): void => {
    for (let k = from; k < to && k < n; k++) {
      if (out[k] !== SENTINEL_NEWLINE) out[k] = ' ';
    }
  };

  let i = 0;
  while (i < n) {
    const ch = src[i];

    let blockHit: BlockCommentRule | null = null;
    for (const bc of cfg.blockComments) {
      if (src.startsWith(bc.open, i)) {
        blockHit = bc;
        break;
      }
    }
    if (blockHit !== null && ch !== undefined) {
      const bc = blockHit;
      let depth = 1;
      let j = i + bc.open.length;
      while (j < n && depth > 0) {
        if (bc.nested && src.startsWith(bc.open, j)) {
          depth++;
          j += bc.open.length;
          continue;
        }
        if (src.startsWith(bc.close, j)) {
          depth--;
          j += bc.close.length;
          continue;
        }
        j++;
      }
      if (!keepComments) blank(i, j);
      i = j;
      continue;
    }

    let lineHit: string | null = null;
    for (const lc of cfg.lineComments) {
      if (src.startsWith(lc, i)) {
        lineHit = lc;
        break;
      }
    }
    if (lineHit !== null) {
      let j = src.indexOf('\n', i);
      if (j < 0) j = n;
      if (!keepComments) blank(i, j);
      i = j;
      continue;
    }

    if (cfg.tripleQuote !== undefined && src.startsWith(cfg.tripleQuote, i)) {
      const q = cfg.tripleQuote;
      let j = src.indexOf(q, i + q.length);
      if (j < 0) j = n;
      else j += q.length;
      blank(i, j);
      i = j;
      continue;
    }

    let strHit: StringRule | null = null;
    for (const s of cfg.strings) {
      if (ch === s.open[0] && src.startsWith(s.open, i)) {
        strHit = s;
        break;
      }
    }
    if (strHit !== null) {
      const s = strHit;
      let j = i + s.open.length;

      let raw = false;
      if (s.rawPrefixes !== undefined) {
        for (const p of s.rawPrefixes) {
          if (i >= p.length && src.startsWith(p, i - p.length)) {
            raw = true;
            break;
          }
        }
      }
      if (raw) {
        let hashes = 0;
        while (j + hashes < n && src[j + hashes] === '#') hashes++;
        const close = s.close + (hashes > 0 ? '#'.repeat(hashes) : '');
        const bodyStart = j + hashes;
        let e = src.indexOf(close, bodyStart);
        if (e < 0) e = n;
        else e += close.length;
        blank(i, e);
        i = e;
        continue;
      }

      let k = j;
      while (k < n) {
        const c = src[k];
        if (s.escape && c === '\\') {
          k += 2;
          continue;
        }
        if (src.startsWith(s.close, k)) {
          k += s.close.length;
          break;
        }
        if (c === '\n' && s.open !== '`') break; // unterminated single-line string
        k++;
      }
      if (k > n) k = n;
      blank(i, k);
      i = k;
      continue;
    }

    i++;
  }
  return out.join('');
}

/** Full mask: comments + strings blanked. For primitive scanning. */
export function fullMask(src: string, cfg: MaskConfig): string {
  return maskCommentsAndStrings(src, cfg, { keepCommentText: false });
}

/** Semi mask: strings blanked, comment text kept. For annotation detection. */
export function semiMask(src: string, cfg: MaskConfig): string {
  return maskCommentsAndStrings(src, cfg, { keepCommentText: true });
}
