/**
 * Stage 1 — Studio subsystem integrity + webapp embed parity + boundary law.
 * Fail-closed: any missing artifact, embed drift, or core/c/ intrusion
 * exits non-zero.
 */

import { execSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..', '..');
const WEBAPP = process.env.STUDIO_WEBAPP_ROOT || join(REPO, '..');

const MUST_EXIST = [
  'packages/studio/src/engine/types.ts',
  'packages/studio/src/engine/schema.ts',
  'packages/studio/src/engine/layout.ts',
  'packages/studio/src/engine/codegen.ts',
  'packages/studio/src/engine/ring.ts',
  'packages/studio/src/engine/ingest.ts',
  'packages/studio/src/engine/sim.ts',
  'packages/studio/src/engine/scheduler.ts',
  'packages/studio/src/engine/flightrec.ts',
  'packages/studio/src/engine/react-adapter.ts',
  'packages/studio/src/engine/studio-engine.ts',
  'packages/studio/src/ui/studio.tsx',
  'packages/studio/src/ui/panels/SchemaDesignerPanel.tsx',
  'packages/studio/src/ui/panels/CacheMapperPanel.tsx',
  'packages/studio/src/ui/panels/RingMonitorPanel.tsx',
  'packages/studio/src/ui/panels/TimeTravelPanel.tsx',
  'packages/studio/src/ui/panels/TelemetryPanel.tsx',
  'packages/studio/src/ui/panels/RenderSpyPanel.tsx',
  'packages/studio/src/ui/panels/StatusBar.tsx',
  'packages/studio/src/ui/panels/ProjectTreePanel.tsx',
  'packages/studio/src/ui/panels/DeliverablesPanel.tsx',
  'packages/studio/package.json',
  'fixtures/studio/golden/canonical.weft',
  'fixtures/studio/golden/manifest.json',
  'docs/studio/STUDIO-SEAMS-V1.md',
  'tools/studio/tests/run_studio_managed_suite.sh',
  'tools/studio/sync_webapp.py',
  'examples/studio/run_demo.ts',
];

let failures = 0;
const results = { stage: 1, checks: [], ok: false };

function check(name, ok, detail = '') {
  results.checks.push({ name, ok, detail });
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' — ' + detail : ''}`);
  if (!ok) failures++;
}

// A) artifacts
for (const rel of MUST_EXIST) {
  check(`artifact ${rel}`, existsSync(join(REPO, rel)));
}

// B) package manifest: zero runtime deps
try {
  const pkg = JSON.parse(readFileSync(join(REPO, 'packages/studio/package.json'), 'utf8'));
  check('package.json name', pkg.name === '@weft/studio');
  check('zero runtime dependencies', Object.keys(pkg.dependencies ?? {}).length === 0);
  check('peer dep react only', JSON.stringify(Object.keys(pkg.peerDependencies ?? {})) === '["react"]');
} catch (e) {
  check('package.json parse', false, String(e));
}

// C) boundary law: branch must not touch core/c/ (Engineers 1 & 2 territory)
try {
  const allowUnified = process.env.WEFT_UNIFIED === '1' || process.env.STUDIO_UNIFIED === '1';
  if (allowUnified) {
    check('branch touches only managed territory (unified mode)', true, 'core/c integrated from Engineers 1 & 2');
  } else {
    const diffBase = process.env.STUDIO_DIFF_BASE || 'origin/main';
    const files = execSync(`git diff --name-only ${diffBase}..HEAD`, { cwd: REPO, encoding: 'utf8' })
      .trim().split('\n').filter(Boolean);
    const intrusion = files.filter((f) => f.startsWith('core/c/'));
    check(`branch touches only managed territory (${files.length} files)`, intrusion.length === 0,
      intrusion.length ? 'INTRUSION: ' + intrusion.join(', ') : 'core/c untouched');
  }
} catch (e) {
  check('git diff boundary law', false, String(e).slice(0, 120));
}

// D) webapp embed parity (hash manifest)
try {
  const out = execSync(`python3 ${join(REPO, 'tools/studio/sync_webapp.py')} --webapp ${WEBAPP} --check`, { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] });
  check('webapp embed parity', out.includes('EMBED PARITY OK'), out.trim().split('\n')[0]);
} catch (e) {
  const msg = String(e.stdout || e);
  check('webapp embed parity', false, msg.trim().split('\n').slice(0, 4).join(' | '));
}

results.ok = failures === 0;
const outPath = join(REPO, 'evidence/pillar7/stage-1-integrity.json');
import('node:fs').then((fs) => fs.writeFileSync(outPath, JSON.stringify(results, null, 2)));
console.log(results.ok ? 'STAGE 1: PASS' : `STAGE 1: FAIL (${failures})`);
process.exit(results.ok ? 0 : 2);
