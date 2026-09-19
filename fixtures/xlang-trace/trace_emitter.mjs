// trace_emitter.mjs — RFC 0014 TS scenario runner (fixture leg).
// Prints the packed event stream as hex — byte-compared against the C
// reference (core/c/trace-dump hex N SEED) by run.sh.
import { traceScenario, toHex } from '../../core/ts/trace.ts';
const n = Number(process.argv[2] ?? 2000);
const seedRaw = process.argv[3] ?? '0x00C0FFEE';
const seed = seedRaw.startsWith('0x') ? parseInt(seedRaw, 16) : Number(seedRaw);
process.stdout.write(toHex(traceScenario(n, seed)) + '\n');
