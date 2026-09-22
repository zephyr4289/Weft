// poison.ts — poisoned TypeScript fixture for the zero-alloc linter tests.
// Every commented rule id MUST fire; anything else is an engine regression.
// Module-scope allocations below exist to prove hot-region scoping.

const RAW = '{"a":1}';
const ROWS = [1, 2, 3];

/** @hot */
export function onMarketTick(tick: number, out: Float64Array): void {
  const scratch = new Float64Array(64); // expect: WV-TS-001
  const closure = (x: number) => x + tick; // expect: WV-TS-005
  const snapshot = { bid: tick, ask: tick }; // expect: WV-TS-002
  const row = [tick, tick, tick]; // expect: WV-TS-003
  const merged = { ...snapshot }; // expect: WV-TS-002 + WV-TS-004
  const parsed = JSON.parse(RAW); // expect: WV-TS-007
  const copy = ROWS.map(normalize); // expect: WV-TS-009
  out[0] = scratch[0] + closure(1) + snapshot.bid + row[0] + merged.ask + parsed.a + copy[0];
}

function normalize(x: number): number {
  return x * 2;
}

export function coldPath(tick: number): number {
  const fresh = new Float64Array(8); // NOT hot: no finding
  const cb = (x: number) => x + tick; // NOT hot: no finding
  return fresh[0] + cb(1);
}
