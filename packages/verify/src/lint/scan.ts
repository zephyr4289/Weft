// scan.ts — the zero-allocation hot-path scanner orchestration.

import * as fs from 'node:fs';
import path from 'node:path';
import { fullMask, semiMask } from './mask.ts';
import { findHotRegions, type HotRegion } from './hot.ts';
import { languageForPath, SIGNATURE_TAIL_ARROW, type LangConfig, type Rule } from './rules.ts';

export interface Finding {
  file: string;
  line: number; // 1-based
  col: number; // 1-based
  ruleId: string;
  ruleName: string;
  message: string;
  hint: string;
  snippet: string;
}

export interface FileScanResult {
  file: string;
  lang: string | null;
  hotFunctions: number;
  findings: Finding[];
}

export interface ScanRun {
  files: FileScanResult[];
  totalFindings: number;
  totalHotFunctions: number;
}

const MAX_FILES = 20000;

function scanLines(
  file: string,
  srcLines: string[],
  maskedLines: string[],
  region: HotRegion,
  lang: LangConfig,
): Finding[] {
  const findings: Finding[] = [];
  for (let li = region.startLine; li <= region.endLine; li++) {
    const masked = maskedLines[li] ?? '';
    if (masked.trim().length === 0) continue;
    for (const rule of lang.rules) {
      let text = masked;
      if (li === region.sigLine && rule.sig !== 'none') {
        if (rule.sig === 'skip') continue;
        text = masked.replace(SIGNATURE_TAIL_ARROW, ' ');
      }
      rule.re.lastIndex = 0;
      let m: RegExpExecArray | null;
      while ((m = rule.re.exec(text)) !== null) {
        if (m[0].length === 0) {
          rule.re.lastIndex++;
          continue;
        }
        findings.push({
          file,
          line: li + 1,
          col: m.index + 1,
          ruleId: rule.id,
          ruleName: rule.name,
          message: rule.message,
          hint: rule.hint,
          snippet: (srcLines[li] ?? '').replace(/\t/g, '    '),
        });
      }
    }
  }
  return findings;
}

export function scanSource(file: string, src: string, lang: LangConfig): FileScanResult {
  const srcLines = src.split('\n');
  const semi = semiMask(src, lang.mask);
  const full = fullMask(src, lang.mask);
  const regions = findHotRegions(semi, full, {
    annotationRe: lang.annotationRe,
    window: 12,
    ...(lang.prefixRe ? { prefixRe: lang.prefixRe } : {}),
  });
  const maskedLines = full.split('\n');

  let findings: Finding[] = [];
  for (const region of regions) {
    findings = findings.concat(scanLines(file, srcLines, maskedLines, region, lang));
  }
  findings.sort((a, b) => a.line - b.line || a.col - b.col || (a.ruleId < b.ruleId ? -1 : 1));
  return { file, lang: lang.id, hotFunctions: regions.length, findings };
}

export function scanFile(filePath: string): FileScanResult | null {
  const lang = languageForPath(filePath);
  if (lang === null) return null;
  let src: string;
  try {
    src = fs.readFileSync(filePath, 'utf8');
  } catch {
    return null; // unreadable: skip silently, caller decides fail-closed on missing inputs
  }
  return scanSource(filePath, src, lang);
}

const MAX_SCAN_BYTES = 2 * 1024 * 1024;

function walkDir(dir: string, out: string[], depth: number): void {
  if (out.length >= MAX_FILES || depth > 12) return;
  let entries: fs.Dirent[];
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  entries.sort((a, b) => (a.name < b.name ? -1 : 1));
  for (const e of entries) {
    if (out.length >= MAX_FILES) return;
    const p = path.join(dir, e.name);
    if (e.isDirectory()) {
      if (e.name === 'node_modules' || e.name === '.git') continue;
      walkDir(p, out, depth + 1);
    } else if (e.isFile()) {
      if (languageForPath(e.name) !== null) out.push(p);
    }
  }
}

/** Collect scannable files from the given paths (files and/or directories), sorted. */
export function collectFiles(paths: string[]): string[] {
  const out: string[] = [];
  for (const p of paths) {
    let st: fs.Stats;
    try {
      st = fs.statSync(p);
    } catch {
      continue;
    }
    if (st.isDirectory()) walkDir(p, out, 0);
    else if (st.isFile() && st.size <= MAX_SCAN_BYTES && languageForPath(p) !== null) out.push(p);
  }
  out.sort((a, b) => (a < b ? -1 : 1));
  return out;
}

export function scanPaths(paths: string[]): ScanRun {
  const files = collectFiles(paths);
  const results: FileScanResult[] = [];
  let totalFindings = 0;
  let totalHotFunctions = 0;
  for (const f of files) {
    const r = scanFile(f);
    if (r === null) continue;
    results.push(r);
    totalFindings += r.findings.length;
    totalHotFunctions += r.hotFunctions;
  }
  return { files: results, totalFindings, totalHotFunctions };
}

const RULE_WIDTH = 14;

export function renderFinding(f: Finding): string {
  const caretPad = ' '.repeat(Math.max(0, f.col - 1));
  const ruleTag = `[${f.ruleId} ${f.ruleName}]`.padEnd(RULE_WIDTH, ' ');
  const head = `${f.file}:${f.line}:${f.col}  ${ruleTag} ${f.message}`;
  const snippet = `    ${f.snippet}`;
  const caret = `    ${caretPad}^~~~`;
  const hint = `    hint: ${f.hint}`;
  return `${head}\n${snippet}\n${caret}\n${hint}`;
}

export function renderScanText(run: ScanRun, title: string): string {
  const lines: string[] = [];
  lines.push(`== weft verify --lint-alloc — ${title}`);
  if (run.totalFindings === 0) {
    lines.push(
      `OK ${run.files.length} file(s) scanned, ${run.totalHotFunctions} hot function(s), 0 hot-path allocation findings`,
    );
    return lines.join('\n');
  }
  for (const fr of run.files) {
    for (const f of fr.findings) lines.push(renderFinding(f));
  }
  lines.push(
    `FAIL ${run.totalFindings} hot-path allocation finding(s) across ${run.files.length} file(s) ` +
      `(${run.totalHotFunctions} hot function(s))`,
  );
  return lines.join('\n');
}
