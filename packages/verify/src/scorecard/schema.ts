// schema.ts — scorecard JSON schema (weft-verify-scorecard/1).

import type { FormalResult, WideResult, SmallModelResult, Theorem } from '../formal/types.ts';
import type { ScanRun } from '../lint/scan.ts';
import type { ChaosResult } from '../chaos/index.ts';
import * as fs from 'node:fs';
import path from 'node:path';

export interface FormalStable {
  schema: string;
  small: SmallModelResult[];
  wide: Omit<WideResult, 'seconds'>;
  theorems: Theorem[];
  statesTotal: number;
  verdict: 'PASS' | 'FAIL';
}

export interface AllocationRow {
  path: string;
  lang: string;
  hotFunctions: number;
  findings: number;
}

export interface AllocationSection {
  scanned: AllocationRow[];
  summary: { files: number; hotFunctions: number; findings: number; compliant: boolean };
}

export interface Scorecard {
  schema: 'weft-verify-scorecard/1';
  tool: { name: string; version: string };
  node: string;
  platform: string;
  verdict: 'PASS' | 'FAIL';
  formal: FormalStable;
  allocation: AllocationSection;
  chaos: ChaosResult;
  resilience: { score: number; breakdown: { delivery1pct: number; delivery5pct: number; recovery: number } };
  package: { bundleKiB: number; runtimeDependencies: number } | null;
}

export function stabilizeFormal(formal: FormalResult): FormalStable {
  const { seconds: _seconds, ...wide } = formal.wide;
  return {
    schema: formal.schema,
    small: formal.small,
    wide,
    theorems: formal.theorems,
    statesTotal: formal.statesTotal,
    verdict: formal.verdict,
  };
}

export function buildAllocationSection(run: ScanRun): AllocationSection {
  const scanned: AllocationRow[] = run.files.map((f) => ({
    path: f.file,
    lang: f.lang ?? '?',
    hotFunctions: f.hotFunctions,
    findings: f.findings.length,
  }));
  scanned.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  const hotFunctions = scanned.reduce((acc, r) => acc + r.hotFunctions, 0);
  const findings = scanned.reduce((acc, r) => acc + r.findings, 0);
  return {
    scanned,
    summary: { files: scanned.length, hotFunctions, findings, compliant: findings === 0 },
  };
}

export function buildResilience(chaos: ChaosResult): Scorecard['resilience'] {
  const rates = chaos.network.rates;
  const at = (r: number): NetworkRateLike | undefined => rates.find((p) => Math.abs(p.dropRate - r) < 1e-9);
  const r1 = at(0.01);
  const r5 = at(0.05);
  const r2 = at(0.02);
  return {
    score: chaos.network.resilienceScore,
    breakdown: {
      delivery1pct: r1 !== undefined ? Math.round(r1.deliveredRatio * 10000) / 100 : 0,
      delivery5pct: r5 !== undefined ? Math.round(r5.deliveredRatio * 10000) / 100 : 0,
      recovery: r2 !== undefined ? Math.max(0, Math.min(100, 100 - 5 * r2.avgRecoveryTicks)) : 0,
    },
  };
}

interface NetworkRateLike {
  dropRate: number;
  deliveredRatio: number;
  avgRecoveryTicks: number;
}

export function measureBundle(rootDir: string): { bundleKiB: number; runtimeDependencies: number } | null {
  try {
    const st = fs.statSync(rootDir);
    if (!st.isDirectory()) return null;
    let total = 0;
    const walk = (dir: string): void => {
      for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) walk(p);
        else total += fs.statSync(p).size;
      }
    };
    walk(rootDir);
    const pkg = JSON.parse(fs.readFileSync(path.join(rootDir, 'package.json'), 'utf8')) as {
      dependencies?: Record<string, string>;
    };
    return {
      bundleKiB: Math.round((total / 1024) * 10) / 10,
      runtimeDependencies: Object.keys(pkg.dependencies ?? {}).length,
    };
  } catch {
    return null;
  }
}

export function buildScorecard(parts: {
  formal: FormalStable;
  allocation: AllocationSection;
  chaos: ChaosResult;
  package: { bundleKiB: number; runtimeDependencies: number } | null;
  version?: string;
}): Scorecard {
  const verdict =
    parts.formal.verdict === 'PASS' &&
    parts.allocation.summary.compliant &&
    parts.chaos.thermal.pass &&
    parts.chaos.bus.pass &&
    parts.chaos.network.pass
      ? 'PASS'
      : 'FAIL';
  return {
    schema: 'weft-verify-scorecard/1',
    tool: { name: '@weft/verify', version: parts.version ?? '1.0.0' },
    node: process.version,
    platform: `${process.platform} ${process.arch}`,
    verdict,
    formal: parts.formal,
    allocation: parts.allocation,
    chaos: parts.chaos,
    resilience: buildResilience(parts.chaos),
    package: parts.package,
  };
}

const THEOREM_RE = /^TH-\d{2}$/;

/** Fail-closed structural validation of a scorecard (Stage 5). */
export function validateScorecard(card: unknown): { ok: boolean; errors: string[] } {
  const errors: string[] = [];
  if (typeof card !== 'object' || card === null) return { ok: false, errors: ['scorecard is not an object'] };
  const c = card as Record<string, unknown>;
  if (c.schema !== 'weft-verify-scorecard/1') errors.push('schema mismatch');
  for (const key of ['tool', 'node', 'platform', 'verdict', 'formal', 'allocation', 'chaos', 'resilience']) {
    if (!(key in c)) errors.push(`missing top-level key: ${key}`);
  }
  const formal = c.formal as Record<string, unknown> | undefined;
  if (formal !== undefined) {
    if (!Array.isArray(formal.small) || (formal.small as unknown[]).length !== 2) {
      errors.push('formal.small must contain 2 models');
    }
    if (!Array.isArray(formal.theorems) || (formal.theorems as unknown[]).length < 9) {
      errors.push('formal.theorems must contain >= 9 theorems');
    } else {
      for (const t of formal.theorems as Array<Record<string, unknown>>) {
        if (typeof t.id !== 'string' || !THEOREM_RE.test(t.id)) errors.push(`bad theorem id: ${String(t.id)}`);
        if (t.verdict !== 'PROVED' && t.verdict !== 'FAILED') errors.push(`bad theorem verdict: ${String(t.verdict)}`);
      }
    }
    const wide = formal.wide as Record<string, unknown> | undefined;
    if (wide === undefined || typeof wide.explored !== 'number') errors.push('formal.wide.explored missing');
  }
  const allocation = c.allocation as Record<string, unknown> | undefined;
  if (allocation !== undefined) {
    const summary = allocation.summary as Record<string, unknown> | undefined;
    if (summary === undefined || typeof summary.compliant !== 'boolean') errors.push('allocation.summary.compliant missing');
  }
  const chaos = c.chaos as Record<string, unknown> | undefined;
  if (chaos !== undefined) {
    const thermal = chaos.thermal as Record<string, unknown> | undefined;
    if (thermal !== undefined) {
      const curve = thermal.curve as unknown[] | undefined;
      if (!Array.isArray(curve) || curve.length !== 7) errors.push('chaos.thermal.curve must have 7 steps');
    }
  }
  if (c.verdict !== 'PASS' && c.verdict !== 'FAIL') errors.push('verdict must be PASS|FAIL');
  return { ok: errors.length === 0, errors };
}
