// crosslang_udp_consume.mjs — TS consumer over UDP: connect to the Python
// producer's data port, subscribe 'xlang', collect frames with a HARD
// deadline (independent of frame arrival), print a JSON line.
// Usage: node crosslang_udp_consume.mjs <producerPort> <count>
import { ClusterClient, UdpTransport } from '../src/index.js';

const producerPort = parseInt(process.argv[2], 10);
const want = parseInt(process.argv[3] ?? '10', 10);
const dec = new TextDecoder();

const t = new UdpTransport();
const client = new ClusterClient({ nodeId: 202, transport: t, payloadMax: 512 });
t.onFrame = (h, base, bytes, i32) => client._onTransportFrame(h, base, bytes, i32);
await t.start();
t.addPeer('127.0.0.1', producerPort);
const sub = client.subscribe('xlang');

const got = [];
const collector = (async () => {
  for await (const f of sub) {
    got.push({
      seq: f.seqLo,
      text: dec.decode(new Uint8Array(f.dv.buffer, f.dv.byteOffset + f.payloadOffset, f.payloadLen)),
    });
    if (got.length >= want) return;
  }
})();
const deadline = new Promise((r) => setTimeout(() => r('deadline'), 12000));
await Promise.race([collector, deadline]);
console.log(JSON.stringify({ got: got.length, seqs: got.map((g) => g.seq),
  first: got[0]?.text ?? null, last: got[got.length - 1]?.text ?? null,
  stale: sub.staleCount, decodeErrors: t.decodeErrors }));
sub.running = false;
await client.stop();
process.exit(0);
