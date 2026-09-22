// cli.ts — `weft verify` unified developer CLI.

import * as fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { scanPaths, renderScanText } from './lint/scan.ts';
import { runFormal, DEFAULT_FORMAL_TARGET, DEFAULT_DEADLINE_MS } from './formal/index.ts';
import { runChaos, DEFAULT_SEED } from './chaos/index.ts';
import {
  stabilizeFormal,
  buildAllocationSection,
  buildScorecard,
  validateScorecard,
  measureBundle,
} from './scorecard/schema.ts';
import { renderScorecardHtml } from './scorecard/html.ts';

const VERSION = '1.0.0';
const PKG_ROOT = path.resolve(fileURLToPath(new URL('.', import.meta.url)), '..', '..');
const REPO_ROOT = path.resolve(PKG_ROOT, '..', '..');

const USAGE = `weft verify — Weft developer verification suite (v${VERSION})

USAGE
  weft verify --all [--states N] [--seed N] [--deadline-ms MS] [--out DIR]
      Run the complete formal proof verification, static alloc linter and
      synthetic silicon chaos suite; write scorecard.{json,html} + formal.json.

  weft verify --lint-alloc [paths...] [--format text|json]
      AST-zero-allocation scanner across TypeScript, C, C++, Rust, Swift and
      Dart @hot sources. Exit 1 on any hot-path allocation finding.
      Default paths: <repo>/packages <repo>/tools <repo>/examples

  weft verify --formal [--states N] [--deadline-ms MS] [--json] [--out FILE]
      Exhaustive TLA-aligned model checks (triad handoff, seqlock parity)
      plus the >= 1e7 state-space exploration tally (default 10000000).

  weft verify --chaos [thermal|bus|network|all] [--bench] [--seed N] [--json]
      Deterministic synthetic thermal (3.2GHz->800MHz), memory-bus and
      network split-brain chaos testbeds. --bench adds a real-time overlay.

  weft verify --report html|json [--input FILE] [--out FILE] [--stdout]
      Generate the verification scorecard. --input accepts a formal.json
      artifact; otherwise formal proofs re-run with the default target.

  weft verify --help | --version

EXIT CODES
  0  verified        1  findings/failures (fail-closed)        2  usage error
`;

interface Parsed {
  command: 'all' | 'lint' | 'formal' | 'chaos' | 'report' | 'help' | 'version';
  paths: string[];
  states?: number;
  seed: number;
  deadlineMs?: number;
  format: string;
  json: boolean;
  chaosProfile: string;
  bench: boolean;
  reportKind: string;
  input?: string;
  out?: string;
  stdout: boolean;
}

function failUsage(msg: string): never {
  process.stderr.write(`weft verify: ${msg}\n\n${USAGE}`);
  process.exit(2);
}

function parseArgs(argv: string[]): Parsed {
  const primaries: string[] = [];
  const p: Parsed = {
    command: 'help',
    paths: [],
    seed: DEFAULT_SEED,
    format: 'text',
    json: false,
    chaosProfile: 'all',
    bench: false,
    reportKind: 'json',
    stdout: false,
  };

  let i = 0;
  let collectingPaths = false;
  while (i < argv.length) {
    const arg = argv[i];
    if (arg === undefined) break;
    if (!arg.startsWith('--')) {
      if (collectingPaths) p.paths.push(arg);
      else failUsage(`unexpected argument '${arg}' (paths must come after --lint-alloc)`);
      i++;
      continue;
    }
    collectingPaths = false;
    let name = arg;
    let inlineVal: string | undefined;
    const eq = arg.indexOf('=');
    if (eq > 0) {
      name = arg.slice(0, eq);
      inlineVal = arg.slice(eq + 1);
    }
    const nextVal = (): string => {
      if (inlineVal !== undefined) return inlineVal;
      const v = argv[i + 1];
      if (v === undefined || v.startsWith('--')) failUsage(`flag ${name} requires a value`);
      i++;
      return v;
    };
    switch (name) {
      case '--all':
        primaries.push('all');
        break;
      case '--lint-alloc':
        primaries.push('lint');
        collectingPaths = true;
        break;
      case '--formal':
        primaries.push('formal');
        break;
      case '--chaos':
        primaries.push('chaos');
        if (inlineVal !== undefined || (argv[i + 1] !== undefined && !argv[i + 1].startsWith('--'))) {
          p.chaosProfile = nextVal();
        }
        break;
      case '--report':
        primaries.push('report');
        if (inlineVal !== undefined || (argv[i + 1] !== undefined && !argv[i + 1].startsWith('--'))) {
          p.reportKind = nextVal();
        }
        break;
      case '--help':
      case '-h':
        primaries.push('help');
        break;
      case '--version':
        primaries.push('version');
        break;
      case '--states':
        p.states = Number(nextVal());
        if (!Number.isFinite(p.states) || p.states < 1) failUsage('--states must be a positive integer');
        break;
      case '--seed':
        p.seed = Number(nextVal()) >>> 0;
        break;
      case '--deadline-ms':
        p.deadlineMs = Number(nextVal());
        if (!Number.isFinite(p.deadlineMs) || p.deadlineMs < 1000) failUsage('--deadline-ms must be >= 1000');
        break;
      case '--format':
        p.format = nextVal();
        if (p.format !== 'text' && p.format !== 'json') failUsage('--format must be text|json');
        break;
      case '--bench':
        p.bench = true;
        break;
      case '--json':
        p.json = true;
        break;
      case '--input':
        p.input = nextVal();
        break;
      case '--out':
        p.out = nextVal();
        break;
      case '--stdout':
        p.stdout = true;
        break;
      default:
        failUsage(`unknown flag '${name}'`);
    }
    i++;
  }

  const unique = [...new Set(primaries)];
  if (unique.length === 0) failUsage('no command given (try --help)');
  if (unique.length > 1) failUsage(`commands are mutually exclusive: ${unique.join(', ')}`);
  if (p.reportKind !== 'html' && p.reportKind !== 'json') failUsage('--report kind must be html|json');
  if (!['thermal', 'bus', 'network', 'all'].includes(p.chaosProfile)) {
    failUsage('--chaos profile must be thermal|bus|network|all');
  }
  p.command = unique[0] as Parsed['command'];
  return p;
}

function relToRoot(p: string): string {
  const rel = path.relative(REPO_ROOT, p);
  return rel.startsWith('..') ? p : rel;
}

function writeOut(file: string, content: string): void {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, content);
}

function defaultLintPaths(): string[] {
  return ['packages', 'tools', 'examples'].map((d) => path.join(REPO_ROOT, d)).filter((d) => fs.existsSync(d));
}

function printFormalHud(f: ReturnType<typeof runFormal>): void {
  process.stdout.write('== weft verify --formal\n');
  for (const t of f.theorems) {
    process.stdout.write(`  ${t.id} ${t.name.padEnd(34)} ${t.verdict.padEnd(7)} ${t.basis}\n`);
  }
  const w = f.wide;
  process.stdout.write(
    `  state-space tally: ${w.explored.toLocaleString('en-US')} distinct states` +
      ` (${w.exhaustive ? 'exhausted — proof by closure' : 'stop-at-target'}) in ${w.seconds}s\n`,
  );
  process.stdout.write(`  verdict: ${f.verdict}\n`);
}

function printChaos(c: ReturnType<typeof runChaos>): void {
  process.stdout.write('== weft verify --chaos\n');
  process.stdout.write('  thermal (deterministic synthetic, cycles/op = ' + c.thermal.cyclesPerOp + ')\n');
  for (const p of c.thermal.curve) {
    process.stdout.write(`    ${String(p.mhz).padStart(5)} MHz  ${p.nsPerOp.toFixed(2).padStart(7)} ns/op\n`);
  }
  process.stdout.write('  memory-bus saturation\n');
  for (const p of c.bus.curve) {
    process.stdout.write(`    ${(p.utilization * 100).toFixed(0).padStart(3)}% bus  ${p.nsPerOp.toFixed(2).padStart(7)} ns/op\n`);
  }
  process.stdout.write('  network (3-node ring, split-brain episode ticks 3000..5000)\n');
  for (const r of c.network.rates) {
    process.stdout.write(
      `    drop ${(r.dropRate * 100).toFixed(1).padStart(4)}%  delivery ${(r.deliveredRatio * 100).toFixed(2).padStart(6)}%  recovery ${r.avgRecoveryTicks} ticks  maxQueue ${r.maxQueue}\n`,
    );
  }
  process.stdout.write(
    `  resilience score: ${c.network.resilienceScore}/100  ` +
      `[thermal ${c.thermal.pass ? 'PASS' : 'FAIL'}] [bus ${c.bus.pass ? 'PASS' : 'FAIL'}] [network ${c.network.pass ? 'PASS' : 'FAIL'}]\n`,
  );
}

function buildCard(parsed: Parsed, formalInputPath?: string) {
  let formal;
  if (formalInputPath !== undefined) {
    let raw: string;
    try {
      raw = fs.readFileSync(formalInputPath, 'utf8');
    } catch {
      failUsage(`--input file not readable: ${formalInputPath}`);
    }
    let parsedJson: unknown;
    try {
      parsedJson = JSON.parse(raw) as unknown;
    } catch (e) {
      failUsage(`--input is not valid JSON: ${(e as Error).message}`);
    }
    const obj = parsedJson as Record<string, unknown>;
    if (obj.schema !== 'weft-verify-formal/1' || !Array.isArray(obj.theorems)) {
      failUsage('--input is not a weft-verify-formal/1 artifact');
    }
    formal = obj as unknown as ReturnType<typeof runFormal>;
  } else {
    formal = runFormal({ states: parsed.states, deadlineMs: parsed.deadlineMs });
  }

  const selfPkgSrc = path.join(REPO_ROOT, 'packages', 'verify', 'src');
  const scanRun = fs.existsSync(selfPkgSrc) ? scanPaths([selfPkgSrc]) : { files: [], totalFindings: 0, totalHotFunctions: 0 };
  const chaos = runChaos('all', parsed.seed, false);
  const pkg = measureBundle(PKG_ROOT);
  const card = buildScorecard({
    formal: stabilizeFormal(formal),
    allocation: buildAllocationSection(scanRun),
    chaos,
    package: pkg,
    version: VERSION,
  });
  return card;
}

async function main(): Promise<void> {
  const parsed = parseArgs(process.argv.slice(2));

  switch (parsed.command) {
    case 'help':
      process.stdout.write(USAGE);
      return;
    case 'version': {
      process.stdout.write(`weft-verify ${VERSION} (node ${process.version}, formal target default ${DEFAULT_FORMAL_TARGET.toLocaleString('en-US')})\n`);
      return;
    }
    case 'lint': {
      const targets = parsed.paths.length > 0 ? parsed.paths : defaultLintPaths();
      const missing = targets.filter((t) => !fs.existsSync(t));
      if (missing.length > 0) failUsage(`path(s) not found: ${missing.join(', ')}`);
      const run = scanPaths(targets);
      if (parsed.format === 'json') {
        process.stdout.write(
          JSON.stringify(
            {
              schema: 'weft-verify-lint/1',
              files: run.files,
              totalFindings: run.totalFindings,
              totalHotFunctions: run.totalHotFunctions,
            },
            null,
            2,
          ) + '\n',
        );
      } else {
        process.stdout.write(renderScanText(run, targets.map(relToRoot).join(' ')) + '\n');
      }
      process.exit(run.totalFindings === 0 ? 0 : 1);
      break;
    }
    case 'formal': {
      const formal = runFormal({ states: parsed.states, deadlineMs: parsed.deadlineMs });
      if (parsed.out !== undefined) writeOut(parsed.out, JSON.stringify(formal, null, 2) + '\n');
      if (parsed.json) process.stdout.write(JSON.stringify(formal, null, 2) + '\n');
      else printFormalHud(formal);
      process.exit(formal.verdict === 'PASS' ? 0 : 1);
      break;
    }
    case 'chaos': {
      const chaos = runChaos(parsed.chaosProfile as 'thermal' | 'bus' | 'network' | 'all', parsed.seed, parsed.bench);
      if (parsed.json) process.stdout.write(JSON.stringify(chaos, null, 2) + '\n');
      else printChaos(chaos);
      const pass =
        (chaos.thermal.curve.length === 0 || chaos.thermal.pass) &&
        (chaos.bus.curve.length === 0 || chaos.bus.pass) &&
        (chaos.network.rates.length === 0 || chaos.network.pass);
      process.exit(pass ? 0 : 1);
      break;
    }
    case 'report': {
      const card = buildCard(parsed, parsed.input);
      const check = validateScorecard(card);
      if (!check.ok) {
        process.stderr.write(`weft verify: scorecard validation failed: ${check.errors.join('; ')}\n`);
        process.exit(1);
      }
      const ext = parsed.reportKind === 'html' ? 'html' : 'json';
      const content =
        parsed.reportKind === 'html' ? renderScorecardHtml(card) : JSON.stringify(card, null, 2) + '\n';
      if (parsed.stdout) {
        process.stdout.write(content);
      } else {
        const out = parsed.out ?? path.join(REPO_ROOT, '.weft-verify', `scorecard.${ext}`);
        writeOut(out, content);
        process.stdout.write(`scorecard written: ${relToRoot(out)} (${card.verdict})\n`);
      }
      process.exit(card.verdict === 'PASS' ? 0 : 1);
      break;
    }
    case 'all': {
      const outDir = parsed.out ?? path.join(REPO_ROOT, '.weft-verify');
      process.stdout.write('== weft verify --all\n');
      const formal = runFormal({ states: parsed.states, deadlineMs: parsed.deadlineMs });
      printFormalHud(formal);
      const lintTargets = parsed.paths.length > 0 ? parsed.paths : defaultLintPaths();
      const run = scanPaths(lintTargets);
      process.stdout.write(renderScanText(run, lintTargets.map(relToRoot).join(' ')) + '\n');
      const chaos = runChaos('all', parsed.seed, false);
      printChaos(chaos);
      const pkg = measureBundle(PKG_ROOT);
      const card = buildScorecard({
        formal: stabilizeFormal(formal),
        allocation: buildAllocationSection(run),
        chaos,
        package: pkg,
        version: VERSION,
      });
      const check = validateScorecard(card);
      if (!check.ok) {
        process.stderr.write(`weft verify: scorecard validation failed: ${check.errors.join('; ')}\n`);
        process.exit(1);
      }
      writeOut(path.join(outDir, 'formal.json'), JSON.stringify(formal, null, 2) + '\n');
      writeOut(path.join(outDir, 'scorecard.json'), JSON.stringify(card, null, 2) + '\n');
      writeOut(path.join(outDir, 'scorecard.html'), renderScorecardHtml(card));
      writeOut(path.join(outDir, 'last-run.json'), JSON.stringify({ formal: stabilizeFormal(formal) }, null, 2) + '\n');
      process.stdout.write(`scorecards written: ${relToRoot(outDir)}/{scorecard.json,scorecard.html,formal.json}\n`);
      const pass =
        formal.verdict === 'PASS' &&
        run.totalFindings === 0 &&
        chaos.thermal.pass &&
        chaos.bus.pass &&
        chaos.network.pass;
      process.stdout.write(`verdict: ${pass ? 'PASS' : 'FAIL'}\n`);
      process.exit(pass ? 0 : 1);
      break;
    }
  }
}

main().catch((e: unknown) => {
  process.stderr.write(`weft verify: ${e instanceof Error ? e.message : String(e)}\n`);
  process.exit(1);
});
