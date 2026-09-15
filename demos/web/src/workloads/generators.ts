// generators.ts — TypeScript port of W1–W5 canonical workload generators
//
// WHY EXISTS: Provides deterministic frame payload generators identical to the
// W-suite Python benchmark catalog per DIRECTIVE-15 T15.1 and FAIRNESS PIN.

const SEED = 0x00c0ffee;

function createRng(seed: number) {
  let s = seed;
  return () => {
    s = (s * 1664525 + 1013904223) >>> 0;
    return s / 4294967296;
  };
}

export interface WorkloadMetadata {
  id: 'W1' | 'W2' | 'W3' | 'W4' | 'W5';
  title: string;
  description: string;
  floatCount: number;
}

export const WORKLOAD_INFO: Record<string, WorkloadMetadata> = {
  W1: {
    id: 'W1',
    title: 'W1: Audio Spectrum (1024 floats @ 60/120Hz)',
    description: '1024-bin FFT magnitude spectrum stream',
    floatCount: 1024,
  },
  W2: {
    id: 'W2',
    title: 'W2: 10k Particle Simulation (3000 floats @ 120Hz)',
    description: '500 particles × 6-DOF (x,y,z,vx,vy,vz) RK4 integration',
    floatCount: 3000,
  },
  W3: {
    id: 'W3',
    title: 'W3: Synthetic Gyroscope / Spectrogram (256×64)',
    description: '16,384 float 2D heat matrix stream (synthetic sensor source)',
    floatCount: 256 * 64,
  },
  W4: {
    id: 'W4',
    title: 'W4: EEG Telemetry / Data Grid (50 delta floats)',
    description: '10,000 rows × 20 cols fixed-cell subset delta stream',
    floatCount: 50,
  },
  W5: {
    id: 'W5',
    title: 'W5: L2 Order Book Ladder (10,000 floats @ 60Hz)',
    description: '1000 price levels × 10 fields bid/ask ladder',
    floatCount: 10000,
  },
};

export function generateW1Audio(frameIdx: number, out: Float32Array): void {
  const rng = createRng((SEED + frameIdx) >>> 0);
  const baseFreq = 1.0 + 0.5 * Math.sin(frameIdx * 0.1);
  for (let i = 0; i < out.length; i++) {
    const mag =
      0.5 * Math.sin((2 * Math.PI * baseFreq * i) / out.length) +
      0.3 * Math.sin((2 * Math.PI * 3 * baseFreq * i) / out.length) +
      0.2 * rng();
    out[i] = Math.max(0.0, Math.min(1.0, mag));
  }
}

export function generateW2Particles(frameIdx: number, out: Float32Array): void {
  const rng = createRng((SEED + frameIdx) >>> 0);
  const particleCount = out.length / 6;
  for (let p = 0; p < particleCount; p++) {
    const idx = p * 6;
    const x = (rng() - 0.5) * 2.0;
    const y = (rng() - 0.5) * 2.0;
    const z = (rng() - 0.5) * 2.0;
    const vx = -y * 0.01;
    const vy = x * 0.01;
    const vz = 0.0;
    out[idx + 0] = x;
    out[idx + 1] = y;
    out[idx + 2] = z;
    out[idx + 3] = vx;
    out[idx + 4] = vy;
    out[idx + 5] = vz;
  }
}

export function generateW3Spectrogram(frameIdx: number, out: Float32Array): void {
  const rng = createRng((SEED + frameIdx) >>> 0);
  const rows = 64;
  const cols = 256;
  let idx = 0;
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const val = Math.sin(frameIdx * 0.05 + r * 0.1 + c * 0.05) * 0.5 + 0.5 + (rng() - 0.5) * 0.1;
      out[idx++] = Math.max(0.0, Math.min(1.0, val));
    }
  }
}

export function generateW4EEG(frameIdx: number, out: Float32Array): void {
  const rng = createRng((SEED + frameIdx) >>> 0);
  for (let i = 0; i < out.length; i++) {
    out[i] = rng() * 1000.0;
  }
}

export function generateW5OrderBook(frameIdx: number, out: Float32Array): void {
  const rng = createRng((SEED + frameIdx) >>> 0);
  const levels = 1000;
  const basePrice = 100.0 + Math.sin(frameIdx * 0.05) * 5.0;
  let idx = 0;
  for (let lvl = 0; lvl < levels; lvl++) {
    out[idx + 0] = basePrice - lvl * 0.01 + (rng() - 0.5) * 0.01; // bid price
    out[idx + 1] = rng() * 10.0; // bid size
    out[idx + 2] = Math.floor(rng() * 50); // bid count
    out[idx + 3] = basePrice + lvl * 0.01 + (rng() - 0.5) * 0.01; // ask price
    out[idx + 4] = rng() * 10.0; // ask size
    out[idx + 5] = Math.floor(rng() * 50); // ask count
    out[idx + 6] = 0;
    out[idx + 7] = 0;
    out[idx + 8] = 0;
    out[idx + 9] = 0;
    idx += 10;
  }
}

export function generateWorkloadFrame(wid: string, frameIdx: number, out: Float32Array): void {
  switch (wid) {
    case 'W1':
      generateW1Audio(frameIdx, out);
      break;
    case 'W2':
      generateW2Particles(frameIdx, out);
      break;
    case 'W3':
      generateW3Spectrogram(frameIdx, out);
      break;
    case 'W4':
      generateW4EEG(frameIdx, out);
      break;
    case 'W5':
      generateW5OrderBook(frameIdx, out);
      break;
  }
}
