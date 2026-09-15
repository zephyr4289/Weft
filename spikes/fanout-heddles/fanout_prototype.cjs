// spikes/fanout-heddles/fanout_prototype.js

class FanoutRing {
  constructor(slotCount = 4, payloadFloats = 256) {
    this.slotCount = slotCount;
    this.slots = [];
    for (let i = 0; i < slotCount; i++) {
      this.slots.push({
        seq: 0,
        payload: new Float32Array(payloadFloats),
      });
    }
    this.writeSeq = 0;
  }

  publish(frameSeq, generator) {
    const slotIdx = (frameSeq - 1) % this.slotCount;
    const slot = this.slots[slotIdx];
    generator(slot.payload, frameSeq);
    slot.seq = frameSeq;
    this.writeSeq = frameSeq;
  }

  claimLatest(target, lastSeenSeq) {
    const current = this.writeSeq;
    if (current === 0 || current === lastSeenSeq) {
      return { seq: lastSeenSeq, isFresh: false, dropped: 0 };
    }
    const slotIdx = (current - 1) % this.slotCount;
    const slot = this.slots[slotIdx];
    target.set(slot.payload);
    const dropped = Math.max(0, current - lastSeenSeq - 1);
    return { seq: current, isFresh: true, dropped };
  }
}

function runFanoutBench(iterations = 50000) {
  const ring = new FanoutRing(4, 256);

  const readers = [
    { name: 'Primary Canvas (60Hz)', rateDivisor: 2, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
    { name: 'Minimap (30Hz)', rateDivisor: 4, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
    { name: 'Flight Recorder (120Hz)', rateDivisor: 1, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
    { name: 'Network Viz (15Hz)', rateDivisor: 8, lastSeq: 0, target: new Float32Array(256), reads: 0, drops: 0, fresh: 0 },
  ];

  const generator = (buf, seq) => {
    buf[0] = seq;
    buf[1] = Math.sin(seq * 0.05);
  };

  for (let step = 1; step <= iterations; step++) {
    ring.publish(step, generator);

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

module.exports = { FanoutRing, runFanoutBench };
