// chaos.test.ts — RFC 0011 stepped-chaos parity for @weft/core.
//
// Pins the TS engine (mirrored from core/ts/fanout_chaos.ts) to:
//   1. the PRNG/pattern vectors shared by every port (the chaos contract),
//   2. the committed golden verdict JSON produced by the C reference
//      oracle (tools/chaos-fixtures/stepped-golden-200k.json) — the engine
//      must reproduce it BYTE-IDENTICALLY. Live C-vs-TS diffs run in
//      ci/scripts/run_chaos_parity.sh; this test keeps the package honest
//      without needing a C toolchain.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { runSteppedChaos, selftest as chaosSelftest } from '../src/fanout_chaos';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, '..', '..', '..');

describe('RFC 0011 stepped chaos engine', () => {
  it('pins the contract vectors (PRNG + pattern)', () => {
    expect(chaosSelftest()).toBe(true);
  });

  it('reproduces the C reference golden verdict byte-identically', () => {
    const golden = readFileSync(
      join(repoRoot, 'tools', 'chaos-fixtures', 'stepped-golden-200k.json'),
      'utf8',
    ).trim();
    const v = runSteppedChaos({
      seed: 1337, steps: 200_000, slots: 4, words: 4,
      readers: 2, frames: 200, chaosRate: 200,
    });
    expect(v.pass).toBe(true);
    expect(v.json).toBe(golden);
  });

  it('holds L-C1..L-C6 under a 99.9% fault rate', () => {
    const v = runSteppedChaos({
      seed: 424242, steps: 20_000, slots: 3, words: 8,
      readers: 4, frames: 100, chaosRate: 999,
    });
    expect(v.pass).toBe(true);
    const parsed = JSON.parse(v.json);
    expect(parsed.ledger.tornAccepted).toBe(0);
    expect(parsed.ledger.futureClaims).toBe(0);
    expect(parsed.ledger.bracketViolations).toBe(0);
    expect(parsed.ledger.fresh.length).toBe(4);
  });
});
