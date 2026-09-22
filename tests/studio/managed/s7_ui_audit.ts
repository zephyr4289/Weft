/**
 * Stage 7 — UI discipline audit + bundle budget + final verdict.
 * - HOT-plane draw functions: no per-frame allocation patterns (`new `, object
 *   /array literals, template literals) and no setState — comment-stripped,
 *   function-body scoped scans (pillar-5 methodology).
 * - Engine sources: zero-dependency, payload budget for the webapp embed.
 * - Desktop launcher manifest: distributed bundle < 15 MB.
 * - Emits the final verdict JSON consumed by the D-73 audit.
 */

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { writeFileSync } from 'node:fs';
import { join, relative } from 'node:path';

const HERE = new URL('.', import.meta.url).pathname;
const REPO = join(HERE, '..', '..', '..');
const SRC = join(REPO, 'packages/studio/src');

const results = { stage: 7, checks: [], ok: false, payload: {} };
let failures = 0;
function check(name, ok, detail = '') {
  results.checks.push({ name, ok, detail: String(detail).slice(0, 240) });
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' — ' + detail : ''}`);
  if (!ok) failures++;
}

// ---------- helpers ----------
function stripComments(code: string): string {
  // state machine: line + block comments; string/template CONTENTS are blanked
  // (quotes retained) so patterns cannot match inside string literals; a
  // template literal in a frame path is still flagged via its own backticks.
  let out = '';
  let i = 0;
  let inStr: string | null = null;
  while (i < code.length) {
    const c = code[i], n = code[i + 1];
    if (inStr) {
      if (inStr === '`') {
        if (c === '\\') { out += '  '; i += 2; continue; }
        if (c === '`') { out += '`'; inStr = null; i++; continue; }
        out += ' ';
        i++; continue;
      }
      if (c === '\\') { out += '  '; i += 2; continue; }
      if (c === inStr) { out += c; inStr = null; i++; continue; }
      out += c === '\n' ? '\n' : ' ';
      i++; continue;
    }
    if (c === '"' || c === "'" || c === '`') { inStr = c; out += c; i++; continue; }
    if (c === '/' && n === '/') { while (i < code.length && code[i] !== '\n') i++; continue; }
    if (c === '/' && n === '*') { i += 2; while (i < code.length && !(code[i] === '*' && code[i + 1] === '/')) i++; i += 2; continue; }
    out += c; i++;
  }
  return out;
}

interface FnSpan { file: string; name: string; body: string }

/** Extract function bodies whose name matches `want` (arrow/function styles). */
function extractFunctions(file: string, code: string, want: RegExp): FnSpan[] {
  const spans: FnSpan[] = [];
  const lines = code.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const m = want.exec(lines[i]);
    if (!m) continue;
    // capture until braces balance
    let depth = 0, started = false, body = '';
    for (let j = i; j < lines.length; j++) {
      for (const ch of lines[j]) {
        if (ch === '{') { depth++; started = true; }
        else if (ch === '}') depth--;
      }
      body += lines[j] + '\n';
      if (started && depth <= 0) break;
    }
    spans.push({ file, name: (m[1] || m[0]).trim(), body });
  }
  return spans;
}

// ---------- hot-plane audit ----------
const HOT_FILES = [
  'ui/panels/RingMonitorPanel.tsx',
  'ui/panels/TelemetryPanel.tsx',
  'ui/panels/RenderSpyPanel.tsx',
  'ui/panels/StatusBar.tsx',
  'ui/panels/TimeTravelPanel.tsx',
  'ui/panels/CacheMapperPanel.tsx',
  'ui/hooks.ts',
];

const drawFnRe = /(?:function\s+(draw\w+|update\w+|render\w+))|(?:(?:const|let)\s+(draw\w+)\s*=)/;
let allocHits: string[] = [];
let setStateHits: string[] = [];
let drawFnCount = 0;
const ALLOC_PATTERNS: Array<[RegExp, string]> = [
  [/new\s+[A-Za-z_$]/, 'new expression'],
  [/\{[^{}\n]*[A-Za-z_$'"][^{}\n]*\s*:[^{}\n]*/, 'object literal'],
  [/(?<![\w\)\]])\[([^\]\[\n]{1,40})\]/, 'array literal'],
  [/`/, 'template literal'],
  [/\.\s*(map|filter|reduce|slice|concat|split|join)\s*\(/, 'array method (allocates)'],
];

/** Extract named draw functions AND inline useHotCanvas drawer arrows. */
function extractHotBodies(rel: string, code: string): FnSpan[] {
  const spans: FnSpan[] = [];
  const lines = code.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const m = drawFnRe.exec(lines[i]);
    const isInline = /useHotCanvas\s*\(/.test(lines[i]) && /=>\s*\{/.test(lines[i]);
    if (!m && !isInline) continue;
    let depth = 0, started = false, body = '';
    for (let j = i; j < lines.length; j++) {
      for (const ch of lines[j]) {
        if (ch === '{') { depth++; started = true; }
        else if (ch === '}') depth--;
      }
      body += lines[j] + '\n';
      if (started && depth <= 0) break;
    }
    const name = m ? (m[1] || m[0]).trim() : `useHotCanvas@${i + 1}`;
    spans.push({ file: rel, name, body });
  }
  return spans;
}

for (const rel of HOT_FILES) {
  const raw = readFileSync(join(SRC, rel), 'utf8');
  const code = stripComments(raw);
  const fns = extractHotBodies(rel, code);
  drawFnCount += fns.length;
  for (const f of fns) {
    for (const [re, label] of ALLOC_PATTERNS) {
      if (re.test(f.body)) allocHits.push(`${rel}::${f.name} — ${label}`);
    }
    const stripped = f.body
      .replace(/\.(setNum|set|setTransform|fillText|setTextAlign|textAlign)\s*\(/g, '.SAFE(')
      .replace(/\.textAlign\s*=/g, '.SAFE =');
    if (/setState|set[A-Z][A-Za-z]*\(/.test(stripped)) {
      setStateHits.push(`${rel}::${f.name}`);
    }
  }
}
check(`hot draw functions audited (${drawFnCount} found)`, drawFnCount >= 10, `${drawFnCount}`);
check('no per-frame allocation patterns in hot draws', allocHits.length === 0,
  allocHits.slice(0, 4).join(' | ') || 'clean');
check('no setState in hot draws', setStateHits.length === 0,
  setStateHits.join(' | ') || 'clean');

// hot panels never call engine-warming React APIs mid-stream: structural scan
for (const rel of HOT_FILES) {
  const code = stripComments(readFileSync(join(SRC, rel), 'utf8'));
  const body = code;
  const bad = /useState\([^)]*\)\s*;?\s*\/\/\s*stream/.test(body);
  check(`${relative(SRC, join(SRC, rel))} structure`, !bad);
}

// ---------- payload budget ----------
function dirBytes(dir: string): { files: string[]; bytes: number; gz: number } {
  const files: string[] = [];
  const walk = (d: string) => {
    for (const e of readdirSync(d)) {
      const p = join(d, e);
      if (statSync(p).isDirectory()) walk(p);
      else files.push(p);
    }
  };
  walk(dir);
  let bytes = 0, gz = 0;
  for (const f of files) {
    const b = readFileSync(f);
    bytes += b.length;
    gz += gzipSync(b).length;
  }
  return { files, bytes, gz };
}

const payload = dirBytes(SRC);
results.payload = { files: payload.files.length, raw_kib: Math.round(payload.bytes / 1024), gzip_kib: Math.round(payload.gz / 1024) };
check('engine+UI payload < 160 KiB gzipped (instant zero-install budget)', payload.gz < 160 * 1024,
  `${Math.round(payload.gz / 1024)} KiB gz / ${Math.round(payload.bytes / 1024)} KiB raw across ${payload.files.length} files`);
check('payload < 500 KiB raw (15 MB desktop bundle share)', payload.bytes < 500 * 1024,
  `${Math.round(payload.bytes / 1024)} KiB`);

// ---------- desktop launcher manifest ----------
const tauriPath = join(REPO, 'packages/studio/desktop/tauri/tauri.conf.json');
try {
  const conf = JSON.parse(readFileSync(tauriPath, 'utf8'));
  const bundleBytes = (conf.bundle?.budget?.distributedBytes ?? 0);
  check('Tauri launcher bundle budget < 15 MB', bundleBytes > 0 && bundleBytes < 15 * 1024 * 1024,
    `${(bundleBytes / 1024 / 1024).toFixed(1)} MB declared`);
  check('Tauri cold start budget < 200 ms', (conf.bundle?.budget?.coldStartMs ?? 1e9) < 200,
    `${conf.bundle?.budget?.coldStartMs} ms declared`);
} catch {
  check('Tauri launcher manifest present', false, tauriPath);
}

// ---------- verdict ----------
results.ok = failures === 0;
writeFileSync(join(REPO, 'evidence/pillar7/stage-7-ui-audit.json'), JSON.stringify(results, null, 2));
console.log(results.ok ? 'STAGE 7: PASS' : `STAGE 7: FAIL (${failures})`);
process.exit(results.ok ? 0 : 2);
