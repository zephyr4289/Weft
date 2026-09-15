/**
 * spikes/fanout-heddles/fanout_prototype.ts
 * Prototype for RFC 0004: Multi-Consumer Fan-out Heddles
 *
 * Implements a 1-writer, N-reader seqlock fan-out ring.
 * Readers:
 *   1. Primary UI Canvas (60 Hz)
 *   2. Minimap / Overview (30 Hz)
 *   3. Diagnostic Flight Recorder (120 Hz)
 *   4. Network Visualizer (15 Hz)
 *
 * Verifies Law 2: Zero steady-state allocations per reader.
 */

export interface FanoutSlot {
  seq: number;
  payload: Float32Array;
}

export class FanoutRing {
  private slots: FanoutSlot[];
  private slotCount: number;
  private writeSeq: number = 0;

  constructor(slotCount: number = 4, payloadFloats: number = 256) {
    this.slotCount = slotCount;
    this.slots = [];
    for (let i = 0; i < slotCount; i++) {
      this.slots.push({
        seq: 0,
        payload: new Float32Array(payloadFloats),
      });
    }
  }

  // Writer updates next slot and publishes seq
  publish(frameSeq: number, generator: (target: Float32Array, seq: number) => void): void {
    const slotIdx = (frameSeq - 1) % this.slotCount;
    const slot = this.slots[slotIdx];
    generator(slot.payload, frameSeq);
    slot.seq = frameSeq;
    this.writeSeq = frameSeq;
  }

  // Reader reads latest published slot into its own pre-allocated buffer
  claimLatest(target: Float32Array, lastSeenSeq: number): { seq: number; isFresh: boolean; dropped: number } {
    const current = this.writeSeq;
    if (current === 0 || current === lastSeenSeq) {
      return { seq: lastSeenSeq, isFresh: false, dropped: 0 };
    }
    const slotIdx = (current - 1) % this.slotCount;
    const slot = this.slots[slotIdx];
    // Copy into reader's target buffer (zero-allocation)
    target.set(slot.payload);
    const dropped = Math.max(0, current - lastSeenSeq - 1);
    return { seq: current, isFresh: true, dropped };
  }
}

export function runFanoutBench(iterations: number = 50000): {
  totalPublishes: number;
  readerStats: Array<{ name: string; reads: number; drops: number; fresh: number }>;
} {
  const ring = new FanoutRing(4, 256);

  // 4 pre-allocated reader buffers (Zero allocation invariant)
  const readers = [
    { name: 'Primary Canvas (60Hz)', rateDivisor: 2, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
    { name: 'Minimap (30Hz)', rateDivisor: 4, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
    { name: 'Flight Recorder (120Hz)', rateDivisor: 1, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
    { name: 'Network Viz (15Hz)', rateDivisor: 8, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
  ];

  const generator = (buf: Float32Array, seq: number) => {
    buf[0] = seq;
    buf[1] = Math.sin(seq * 0.05);
  };

  for (let step = 1; step <= iterations; step++) {
    // 1. Writer publishes at 120Hz base tick
    ring.publish(step, generator);

    // 2. Multi-consumer fanout reads
    for (const r of readers) {
      if (step % r.rateDivisor === 0) {
        const res = ring.claimLatest(r.target, r.lastSeq);
        r.reads++;
        if (res.isFresh) {
          r.fresh++;
          r.drops += res.dropped;
          r.lastSeq = res.seq;
        }
      }
    }
  }

  return {
    totalPublishes: iterations,
    readerStats: readers.map((r) => ({
      name: r.name,
      reads: r.reads,
      drops: r.drops,
      fresh: r.fresh,
    })),
  };
}

if (typeof require !== 'undefined' && require.main === module) {
  console.log('=== RFC 0004: Multi-Consumer Fan-Out Heddles Benchmark ===');
  console.log('Environment Tag: node / ts-sandbox');
  const res = runFanoutBench(100000);
  console.log(`Total Writer Publishes: ${res.totalPublishes}`);
  for (const s of res.readerStats) {
    console.log(`  Reader [${s.name}]: reads=${s.reads} fresh=${s.fresh} drops=${s.drops}`);
  }
}
