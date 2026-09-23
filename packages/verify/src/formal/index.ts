// index.ts — formal verification orchestrator.

import { runSmallModels } from './small.ts';
import { runWideTally } from './wide.ts';
import type { FormalResult, Theorem } from './types.ts';

export const DEFAULT_FORMAL_TARGET = 10_000_000;
export const DEFAULT_DEADLINE_MS = 120_000;

export interface FormalOptions {
  states?: number;
  deadlineMs?: number;
}

export function runFormal(opts: FormalOptions = {}): FormalResult {
  const target = opts.states ?? DEFAULT_FORMAL_TARGET;
  const deadlineMs = opts.deadlineMs ?? DEFAULT_DEADLINE_MS;

  const { results: small, allProved } = runSmallModels();
  const wide = runWideTally(target, deadlineMs);

  const a = small.find((m) => m.model === 'triad-handoff');
  const b = small.find((m) => m.model === 'seqlock-parity');
  const aStates = a !== undefined ? a.states : 0;
  const bStates = b !== undefined ? b.states : 0;

  const theorems: Theorem[] = [];
  const push = (id: string, name: string, ok: boolean, basis: string): void => {
    theorems.push({ id, name, verdict: ok ? 'PROVED' : 'FAILED', basis });
  };

  const aInv = (name: string): boolean =>
    a !== undefined && a.invariants.find((i) => i.name === name)?.verdict === 'PROVED';
  const bInv = (name: string): boolean =>
    b !== undefined && b.invariants.find((i) => i.name === name)?.verdict === 'PROVED';
  const aProg = (name: string): boolean =>
    a !== undefined && a.progress.find((p) => p.name === name)?.proved === true;
  const bProg = (name: string): boolean =>
    b !== undefined && b.progress.find((p) => p.name === name)?.proved === true;

  push(
    'TH-01',
    'single-writer-exclusivity',
    aInv('single-writer-exclusivity'),
    `exhaustive triad-handoff BFS, ${aStates} states`,
  );
  push(
    'TH-02',
    'writer-reader-slot-exclusion',
    aInv('writer-holds-WRITING-slot') && aInv('writer-reader-slot-exclusion'),
    `exhaustive triad-handoff BFS, ${aStates} states`,
  );
  push(
    'TH-03',
    'no-deadlock',
    (a?.deadlocks ?? 1) === 0 && (b?.deadlocks ?? 1) === 0,
    `0 deadlock states across ${aStates}+${bStates} exhaustive states`,
  );
  push(
    'TH-04',
    'bounded-commit-progress<=10',
    aProg('commit-progress'),
    a !== undefined
      ? `existential bounded liveness, max ${a.progress.find((p) => p.name === 'commit-progress')?.maxSteps} steps`
      : 'model missing',
  );
  push(
    'TH-05',
    'bounded-read-progress<=10',
    aProg('read-progress'),
    a !== undefined
      ? `existential bounded liveness, max ${a.progress.find((p) => p.name === 'read-progress')?.maxSteps} steps`
      : 'model missing',
  );
  push(
    'TH-06',
    'parity-gated-publication',
    bInv('parity-gated-publication'),
    `exhaustive seqlock-parity BFS, ${bStates} states`,
  );
  push(
    'TH-07',
    'bounded-clean-read<=8-fair',
    bProg('clean-read-progress'),
    b !== undefined
      ? `existential bounded liveness (worst case: torn latch + writer lap + relatch = 7), max ${b.progress.find((p) => p.name === 'clean-read-progress')?.maxSteps} steps`
      : 'model missing',
  );
  push(
    'TH-08',
    'bounded-writer-progress<=3',
    bProg('writer-progress'),
    b !== undefined
      ? `existential bounded liveness, max ${b.progress.find((p) => p.name === 'writer-progress')?.maxSteps} steps`
      : 'model missing',
  );

  const tallyOk = wide.explored >= target && !wide.deadlineHit;
  theorems.push({
    id: 'TH-09',
    name: 'state-space-tally>=1e7',
    verdict: tallyOk ? 'PROVED' : 'FAILED',
    basis: wide.exhaustive
      ? `model exhausted at ${wide.explored} distinct states (proof by closure)`
      : `${wide.explored} distinct states explored${wide.truncated ? ' (stop-at-target)' : ''}`,
  });

  const allTheoremsProved = theorems.every((t) => t.verdict === 'PROVED');
  const statesTotal = aStates + bStates + wide.explored;

  return {
    schema: 'weft-verify-formal/1',
    tool: 'weft-verify',
    command: 'formal',
    small,
    wide,
    theorems,
    statesTotal,
    verdict: allTheoremsProved && allProved && !wide.deadlineHit ? 'PASS' : 'FAIL',
  };
}

export { runSmallModels, runWideTally };
