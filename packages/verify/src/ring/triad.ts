// triad.ts — the managed twin of the C triad buffer (Engineer 1 seam).

export const SLOT_FREE = 0;
export const SLOT_WRITING = 1;
export const SLOT_COMMITTED = 2;
export const SLOT_READ = 3;
export const SLOT_DROPPED = 4;

export const PAYLOAD_BYTES = 64;

/** Zero-allocation triad ring: 3 slots x 64B payload, seqlock versions. */
export class TriadRing {
  readonly payload: Uint8Array; // 3 * 64
  readonly state: Uint8Array; // per slot
  readonly version: Uint32Array; // per slot
  writerSlot: number;
  readerSlot: number;
  commits: number;
  reads: number;

  constructor() {
    this.payload = new Uint8Array(3 * PAYLOAD_BYTES);
    this.state = new Uint8Array([SLOT_FREE, SLOT_FREE, SLOT_FREE]);
    this.version = new Uint32Array(3);
    this.writerSlot = -1;
    this.readerSlot = -1;
    this.commits = 0;
    this.reads = 0;
  }

  /** Writer: claim a FREE/DROPPED slot. Returns slot index or -1. */
  acquireWrite(): number {
    const st = this.state;
    for (let i = 0; i < 3; i++) {
      if (st[i] === SLOT_FREE || st[i] === SLOT_DROPPED) {
        st[i] = SLOT_WRITING;
        this.writerSlot = i;
        return i;
      }
    }
    return -1;
  }

  /** Writer: publish the held slot (WRITING -> COMMITTED, version++). */
  commitWrite(): void {
    const s = this.writerSlot;
    if (s < 0 || this.state[s] !== SLOT_WRITING) return;
    this.state[s] = SLOT_COMMITTED;
    this.version[s] = (this.version[s] + 1) >>> 0;
    this.writerSlot = -1;
    this.commits++;
  }

  /** Reader: take the newest COMMITTED slot (COMMITTED -> READ). */
  acquireRead(): number {
    const st = this.state;
    for (let i = 0; i < 3; i++) {
      if (st[i] === SLOT_COMMITTED) {
        st[i] = SLOT_READ;
        this.readerSlot = i;
        return i;
      }
    }
    return -1;
  }

  /** Reader: release the held slot (READ -> FREE). */
  releaseRead(): void {
    const s = this.readerSlot;
    if (s < 0 || this.state[s] !== SLOT_READ) return;
    this.state[s] = SLOT_FREE;
    this.readerSlot = -1;
    this.reads++;
  }

  /** Byte offset of a slot payload. */
  offset(slot: number): number {
    return slot * PAYLOAD_BYTES;
  }
}
