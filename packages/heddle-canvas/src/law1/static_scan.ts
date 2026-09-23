// static_scan.ts — the textual Law-1 discipline gate (heuristic, labeled).
//
// WHY EXISTS: the heap gate proves the RUNTIME behavior; this scanner
// enforces the SOURCE discipline so violations are caught at review time,
// not after a red heap gate on CI. It is deliberately a HEURISTIC — a
// conservative deny-list inside frame-time method bodies — and it says so
// everywhere (the heap gate remains the load-bearing proof; this is the
// tripwire).
//
// WHAT IT FLAGS inside a frame-time method body:
//   new ...           — every allocation constructor
//   =>                — arrow functions close over fresh state (allocation)
//   `...`             — template strings allocate
//   .map( .filter( .slice( .concat( .subarray( .splice(
//   JSON.              — serialization allocates strings
//   BigInt / 0n..n     — BigInt boxes (the dirty-mask lesson, RFC-0022 §3)
//   ...spread          — spread materializes arrays
// The scanner is token-level: a comment mentioning "new" inside a method
// is a false positive the caller allowlists explicitly — we prefer ten
// false positives to one silent allocator.

export interface ScanViolation {
  readonly method: string;
  readonly line: number;
  readonly token: string;
  readonly text: string;
}

const FORBIDDEN: readonly RegExp[] = [
  /\bnew\s+[A-Za-z_$]/,      // constructors
  /=>/,                       // closures
  /`/,                        // template strings
  /\.map\s*\(/,
  /\.filter\s*\(/,
  /\.slice\s*\(/,
  /\.concat\s*\(/,
  /\.subarray\s*\(/,
  /\.splice\s*\(/,
  /\bJSON\./,
  /\bBigInt\b/,
  /\b\d+n\b/,                 // BigInt literals
  /\.\.\./,                   // spread
];

/**
 * Extract a method body from class source text by brace matching from the
 * `methodName(` signature to its matching close. Returns null when the
 * method is absent (callers decide whether that is a violation).
 */
export function extractMethodBody(source: string, methodName: string): string | null {
  const sig = new RegExp(`\\b${methodName}\\s*\\(`).exec(source);
  if (sig === null) return null;
  let i = source.indexOf('{', sig.index);
  if (i === -1) return null;
  let depth = 0;
  for (; i < source.length; i++) {
    const ch = source[i];
    if (ch === '{') depth++;
    else if (ch === '}') {
      depth--;
      if (depth === 0) return source.slice(source.indexOf('{', sig.index), i + 1);
    }
  }
  return null;
}

/**
 * Scan frame-time method bodies for allocation tokens. `allow` lists
 * literal line contents to permit (documented false positives).
 */
export function scanFrameMethods(
  source: string,
  frameMethods: readonly string[],
  allow: readonly string[] = [],
): ScanViolation[] {
  const violations: ScanViolation[] = [];
  for (const m of frameMethods) {
    const body = extractMethodBody(source, m);
    if (body === null) continue;
    const lines = body.split('\n');
    for (let li = 0; li < lines.length; li++) {
      const line = lines[li];
      if (allow.some((a) => line.includes(a))) continue;
      for (const re of FORBIDDEN) {
        const hit = re.exec(line);
        if (hit !== null) {
          violations.push({ method: m, line: li, token: hit[0], text: line.trim() });
          break; // one report per line is enough
        }
      }
    }
  }
  return violations;
}
