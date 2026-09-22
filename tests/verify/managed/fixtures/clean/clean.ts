// clean.ts — clean TypeScript fixture: ZERO findings expected.
// Proves hot scoping (cold-path allocations below are legal).

/** @hot */
export function onTick(tick: number, out: Float64Array, scratch: Float64Array): void {
  let acc = 0;
  for (let i = 0; i < 8; i++) {
    scratch[i] = tick * i;
    const magnitude = scratch[i] > 0 ? scratch[i] : -scratch[i];
    acc += magnitude;
  }
  out[0] = acc;
  if (acc > 1e9) {
    out[1] = -1;
    return;
  }
}

export function coldPath(tick: number): number {
  const fresh = new Float64Array(8); // NOT hot: legal
  const cb = (x: number) => x + tick; // NOT hot: legal
  return fresh[0] + cb(1);
}
