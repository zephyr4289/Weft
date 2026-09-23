// law1.test.ts — the Law-1 discipline: the static scanner over the REAL
// HAL sources (the tripwire), and the heap gate harness self-check (the
// load-bearing proof runs in bench/frame_budget.ts under --expose-gc; here
// we verify the gate's own semantics on a known-allocating vs known-clean
// driver when forced GC is available).

import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { scanFrameMethods } from '../src/law1/static_scan.ts';
import { heapGate, heapGateAvailable } from '../src/law1/audit.ts';

const here = dirname(fileURLToPath(import.meta.url));
const pkgRoot = join(here, '..');

const FRAME_METHODS = [
  'beginFrame',
  'uploadWaveform',
  'uploadRows',
  'drawOscillo',
  'drawDepthLadder',
  'drawCandles',
  'drawPointcloud',
  'endFrame',
] as const;

// Documented false positives (the deny-list is token-level):
//  - the `new ImageData`/`new Float32Array` allocations live in
//    initialize(), a frame method name never appears there — but the
//    scanner extracts by method NAME anywhere in the class source, so
//    allow-lists pin the exact legal lines.
const ALLOW = [
  '// ', // comments may mention anything
  // Error-path escapes: `new HeddleError(...)` throws fire AT MOST ONCE —
  // the loop dies and the ladder degrades. They are not steady-state
  // allocations; the heap gate proves the steady path. Documented here so
  // the allowlist is an explicit contract, not a rubber stamp.
  'new HeddleError(',
  // …and the multi-line error MESSAGE templates inside those throws
  // (template literals allocate, but only on the once-per-lifetime path):
  'on non-waveform lane',
  'on waveform lane',
];

describe('Law-1 static scan over the real HAL sources', () => {
  it('NullHAL frame methods are allocation-free (textually)', () => {
    const src = readFileSync(join(pkgRoot, 'src/hal/null_device.ts'), 'utf8');
    const v = scanFrameMethods(src, FRAME_METHODS, ALLOW);
    expect(v).toEqual([]);
  });

  it('Canvas2DHAL frame methods are allocation-free (textually)', () => {
    const src = readFileSync(join(pkgRoot, 'src/hal/canvas2d/canvas2d_blitter.ts'), 'utf8');
    const v = scanFrameMethods(src, FRAME_METHODS, ALLOW);
    expect(v).toEqual([]);
  });

  it('WebGL2HAL frame methods are allocation-free (textually)', () => {
    const src = readFileSync(join(pkgRoot, 'src/hal/webgl2/webgl2_binder.ts'), 'utf8');
    const v = scanFrameMethods(src, FRAME_METHODS, ALLOW);
    expect(v).toEqual([]);
  });

  it('WebGPUHAL frame methods: only the SPEC-MANDATED residual survives', () => {
    const src = readFileSync(join(pkgRoot, 'src/hal/webgpu/webgpu_binder.ts'), 'utf8');
    const v = scanFrameMethods(src, FRAME_METHODS, ALLOW);
    // The only permitted hits are createCommandEncoder()/encoder.finish()
    // inside submit([encoder.finish()]) — wait, the deny-list flags `new`,
    // not method calls. The WebGPU spec-mandated per-frame objects are
    // created via createCommandEncoder() (a method call, not `new`) and
    // the submit array literal [encoder.finish()] — array literals are
    // NOT on the deny-list (small fixed arrays often stay in V8's young
    // generation; the heap gate measures the truth). So: zero violations
    // expected textually; the residual is reported by the heap gate and
    // labeled in D-42 §5.
    expect(v).toEqual([]);
  });

  it('the scanner actually bites: a planted allocator is caught', () => {
    const dirty = `
class X {
  beginFrame(): void {
    const scratch = new Float32Array(4);
    void scratch;
  }
}
`;
    const v = scanFrameMethods(dirty, ['beginFrame'], ALLOW);
    expect(v.length).toBe(1);
    expect(v[0].token.startsWith('new')).toBe(true);
  });
});

describe('Law-1 heap gate semantics (when --expose-gc is present)', () => {
  it.skipIf(!heapGateAvailable())(
    'a clean driver passes and an allocating driver FAILS',
    () => {
      const clean = heapGate(50, 500, () => {
        /* deliberately nothing */
      });
      expect(clean.passed).toBe(true);

      const sink: unknown[] = [];
      const allocating = heapGate(50, 500, () => {
        sink.push({ frame: 1 }); // one object per frame — 500 objects
      });
      expect(allocating.passed).toBe(false);
      expect(allocating.deltaBytes).toBeGreaterThan(0);
    },
  );
});
