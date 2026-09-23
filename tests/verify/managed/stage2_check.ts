// stage2_check.ts — Stage 2 comparator: linter precision & recall versus
// fixtures/expected/poisoned.expect.json.
//
// Usage: node stage2_check.ts <lint-findings.json> <expect.json>
// lint-findings.json is the output of `weft verify --lint-alloc --format json`.
// Requires set equality on (basename, line, rule); clean files zero findings.
import * as fs from 'node:fs';

interface Finding {
  file: string;
  line: number;
  ruleId: string;
}
interface LintOutput {
  files: Array<{ file: string; findings: Finding[]; hotFunctions: number }>;
  totalFindings: number;
}

function fail(msg: string): never {
  console.error(`stage2: FAIL ${msg}`);
  process.exit(1);
}

const [lintPath, expectPath] = process.argv.slice(2);
if (!lintPath || !expectPath) fail('usage: stage2_check.ts <lint.json> <expect.json>');

let lint: LintOutput;
let expect: { poisoned: Array<{ file: string; line: number; rule: string }>; clean: string[] };
try {
  lint = JSON.parse(fs.readFileSync(lintPath, 'utf8')) as LintOutput;
  expect = JSON.parse(fs.readFileSync(expectPath, 'utf8'));
} catch (e) {
  fail(`input error: ${(e as Error).message}`);
}

const base = (p: string): string => p.split('/').pop() ?? p;
const actual = new Set<string>();
for (const f of lint.files) {
  for (const fd of f.findings) {
    actual.add(`${base(f.file)}|${fd.line}|${fd.ruleId}`);
  }
}
const expected = new Set<string>(
  expect.poisoned.map((e) => `${e.file}|${e.line}|${e.rule}`),
);

const missed: string[] = [];
for (const e of expected) if (!actual.has(e)) missed.push(e);
const unexpected: string[] = [];
for (const a of actual) if (!expected.has(a)) unexpected.push(a);

const recall = expected.size === 0 ? 1 : (expected.size - missed.length) / expected.size;
const precision = actual.size === 0 ? 1 : (actual.size - unexpected.length) / actual.size;

// clean fixture guard: they are part of the scan set and must contribute zero
const cleanFindings = lint.files
  .filter((f) => expect.clean.includes(base(f.file)))
  .reduce((acc, f) => acc + f.findings.length, 0);

const ok = missed.length === 0 && unexpected.length === 0 && cleanFindings === 0;
console.log(
  JSON.stringify(
    {
      stage: 2,
      poisonedFixtures: expect.poisoned.length,
      flagged: actual.size,
      missed: missed.length,
      unexpected: unexpected.length,
      cleanFindings,
      precision: Math.round(precision * 10000) / 10000,
      recall: Math.round(recall * 10000) / 10000,
      ok,
      ...(missed.length > 0 ? { missedDetail: missed } : {}),
      ...(unexpected.length > 0 ? { unexpectedDetail: unexpected } : {}),
    },
    null,
    2,
  ),
);
process.exit(ok ? 0 : 1);
