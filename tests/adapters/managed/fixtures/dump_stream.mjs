// dump_stream.mjs — write a deterministic ITCH stream to disk (utility).
//
// Used by the Python tracemalloc gate and demos to obtain the exact same
// message sequence the TS engine proves against. Default 1,000,000 messages.
//
//   node dump_stream.mjs --count 1000000 --out /tmp/stream.bin [--chunk 50000]

import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { buildItchChunked } from './generate.mjs';

const args = process.argv.slice(2);
const argOf = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
};
const count = Number(argOf('--count', '1000000'));
const chunk = Number(argOf('--chunk', '50000'));
const out = argOf('--out', '/tmp/weft-adapters-stream.bin');

mkdirSync(dirname(out), { recursive: true });
const feed = buildItchChunked(count, chunk);
const buf = Buffer.concat(feed.chunks);
writeFileSync(out, buf);
console.log(JSON.stringify({ out, count: feed.total, bytes: buf.length, counts: feed.counts }));
