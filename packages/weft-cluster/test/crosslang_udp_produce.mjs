// crosslang_udp_produce.mjs — TS producer: bind a FIXED port, wait for the
// remote SUB control frame, then burst N frames on 'xlang'.
// Usage: node crosslang_udp_produce.mjs <port> <count>
import { ClusterClient, UdpTransport } from '../src/index.js';

const port = parseInt(process.argv[2], 10);
const count = parseInt(process.argv[3] ?? '10', 10);
const enc = new TextEncoder();
const t = new UdpTransport({ port });
const client = new ClusterClient({ nodeId: 101, transport: t, payloadMax: 512 });
await t.start();
client.registerTopic('xlang');
// Wait for the Python consumer's SUB (deadline 15s).
const deadline = Date.now() + 15000;
while (t._subs.size === 0 && Date.now() < deadline) {
  await new Promise((r) => setTimeout(r, 20));
}
for (let i = 0; i < count; i++) {
  client.publish('xlang', enc.encode(`ts-frame-${i}`));
  await new Promise((r) => setTimeout(r, 5));
}
await new Promise((r) => setTimeout(r, 150));
console.log(JSON.stringify({ port, count, subs: t._subs.size }));
await client.stop();
process.exit(0);
