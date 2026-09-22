// types.ts — shared formal/scorecard result shapes (erasable-only TS).

export interface InvariantResult {
  id: string;
  name: string;
  verdict: 'PROVED' | 'FAILED';
  evidence: string;
}

export interface ProgressResult {
  name: string;
  bound: number;
  proved: boolean;
  maxSteps: number;
}

export interface SmallModelResult {
  model: string;
  states: number;
  transitions: number;
  maxDepth: number;
  deadlocks: number;
  tornRetriesObserved: number;
  invariants: InvariantResult[];
  progress: ProgressResult[];
  verdict: 'PROVED' | 'FAILED';
}

export interface WideResult {
  target: number;
  explored: number;
  transitions: number;
  truncated: boolean;
  deadlineHit: boolean;
  exhaustive: boolean;
  seconds: number;
  capacity: number;
  loadFactor: number;
}

export interface Theorem {
  id: string;
  name: string;
  verdict: 'PROVED' | 'FAILED';
  basis: string;
}

export interface FormalResult {
  schema: 'weft-verify-formal/1';
  tool: string;
  command: 'formal';
  small: SmallModelResult[];
  wide: WideResult;
  theorems: Theorem[];
  statesTotal: number;
  verdict: 'PASS' | 'FAIL';
}
