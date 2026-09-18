// blend.test.ts — the Q12 raster blend, TS reference conformance (Series 8).
//
// The cross-language gate (fixtures/xlang-blend/) byte-compares all ports
// against the C kernel's golden digests; this battery pins the properties
// the reference must hold regardless of port: boundary identities, the
// no-rounding truncation, in-place aliasing, and hardcoded digests from
// the committed CSV (a canary against accidental spec drift).

import { describe, it, expect } from 'vitest';
import {
  blendQ12Packed,
  blendQ12Words,
  fnv1a64Words,
  BLEND_ONE_Q12,
} from '../src/blend';

function xorshift32(x: number): number {
  x >>>= 0;
  x ^= (x << 13) >>> 0;
  x ^= x >>> 17;
  x ^= (x << 5) >>> 0;
  return x >>> 0;
}

describe('Q12 raster blend (blend.ts)', () => {
  it('boundary identities: alpha 0 reproduces prev, 4096 reproduces newest', () => {
    let s = 0xabcdef;
    for (let i = 0; i < 1000; i++) {
      s = xorshift32(s);
      const a = s;
      s = xorshift32(s);
      const b = s;
      // JS bitwise yields signed int32; normalize both sides.
      expect(blendQ12Packed(a, b, 0)).toBe(a | 0);
      expect(blendQ12Packed(a, b, BLEND_ONE_Q12)).toBe(b | 0);
    }
  });

  it('channels blend independently (per-channel truncation, no rounding term)', () => {
    // Equal channels are a fixed point of the weighted average.
    expect(blendQ12Packed(0x0000ff00, 0x0000ff00, 2048)).toBe(0x0000ff00);
    // Asymmetric truncation: r goes 0 -> 255 at alpha 2048
    // ((255*2048) >> 12 = 127 — floor, never round-to-128).
    expect(blendQ12Packed(0x00000000, 0x000000ff, 2048) & 0xff).toBe(127);
    // alpha 4095: (255*4095) >> 12 = 254 (254.9 truncates — the rounding
    // term would produce 255 and diverge from every SIMD lane).
    expect(blendQ12Packed(0x00000000, 0x000000ff, 4095) & 0xff).toBe(254);
  });

  it('out may alias prev (in-place blend) and matches the fresh-buffer result', () => {
    const n = 1000;
    const prev = new Uint32Array(n);
    const newest = new Uint32Array(n);
    let s = 0x5eedbeef;
    for (let i = 0; i < n; i++) {
      s = xorshift32(s); prev[i] = s;
      s = xorshift32(s); newest[i] = s;
    }
    const fresh = new Uint32Array(n);
    blendQ12Words(prev, newest, 1234, fresh);
    const inPlace = prev.slice();
    blendQ12Words(inPlace, newest, 1234, inPlace);
    expect(Array.from(inPlace)).toEqual(Array.from(fresh));
  });

  it('digest canaries: hardcoded rows from fixtures/xlang-blend/golden-vectors.csv', () => {
    // These digests were emitted by the C kernel (blend_test --digests);
    // the TS reference reproduces them and the other ports must too.
    const replay = (words: number, alpha: number): string => {
      const prev = new Uint32Array(words + 8);
      const newest = new Uint32Array(words + 8);
      let s = 0x5eedbeef;
      for (let i = 0; i < words + 8; i++) {
        s = xorshift32(s); prev[i] = s;
        s = xorshift32(s); newest[i] = s;
      }
      const out = new Uint32Array(words + 8);
      blendQ12Words(prev, newest, alpha, out);
      return fnv1a64Words(out.subarray(0, words));
    };
    expect(replay(0, 0)).toBe('cbf29ce484222325');
    expect(replay(4097, 4095)).toBe('a1a489a28510d7b2');
    expect(replay(4097, 4096)).toBe('c44a2746c6ab6b7d');
    expect(replay(257, 2048)).toBe('9eb557435b9dab22');
    expect(replay(1000, 512)).toBe('1b6c7d3288099530');
  });
});
