// probe_run.ts — Stage 3 runner: zero-allocation runtime execution probe.
// Usage: node [--expose-gc] probe_run.ts [iterations] [rounds]
// Prints JSON; exit 0 iff the heap-growth gate (<= 64 KiB) passes.
import { runZeroAllocProbe } from '../../../packages/verify/src/runtime/probe.ts';

const iterations = Number(process.argv[2] ?? 1_000_000);
const rounds = Number(process.argv[3] ?? 3);
const bite = process.argv[4] === 'bite'; // negative control: allocate per iteration

if (!Number.isFinite(iterations) || iterations < 1) {
  console.error('probe_run: invalid iterations');
  process.exit(2);
}

const hasGc = typeof (globalThis as { gc?: unknown }).gc === 'function';
if (!hasGc) {
  console.error('probe_run: Node must run with --expose-gc for the zero-alloc probe');
  process.exit(2);
}

const result = runZeroAllocProbe(iterations, rounds, bite);
console.log(JSON.stringify(result, null, 2));
process.exit(result.pass ? 0 : 1);
