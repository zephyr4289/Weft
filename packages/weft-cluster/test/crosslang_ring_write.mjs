// crosslang_ring_write.mjs — write N frames into a file-backed ShmRing image
// for the Python mmap attach test. Usage: node crosslang_ring_write.mjs <path> <count>
import fs from 'node:fs';
import { ClusterClient, ShmFabric, LoopbackShmTransport, ShmRing } from '../src/index.js';

const path = process.argv[2];
const count = parseInt(process.argv[3] ?? '32', 10);
const enc = new TextEncoder();

// A real ClusterClient publishes into per-subscriber rings; take one ring
// straight out of the fabric and persist it (single-writer image).
const fabric = new ShmFabric();
const client = new ClusterClient({ nodeId: 303, transport: new LoopbackShmTransport(fabric) });
const ring = new ShmRing(64, 512);
fabric.addSubscriber('xcache', ring);
client.registerTopic('xcache');
for (let i = 0; i < count; i++) {
  client.publish('xcache', enc.encode(`ring-frame-${i}`));
}
fs.writeFileSync(path, Buffer.from(ring.bytes));
console.log(JSON.stringify({ wrote: count, bytes: ring.bytes.byteLength,
  cursor: ring.cursor, stride: ring.stride, slotCount: ring.slotCount,
  payloadMax: ring.payloadMax }));
