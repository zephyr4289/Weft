// verified.test.ts — @weft/core RFC-0005 VerifiedWeft conformance suite
//
// WHY EXISTS: RFC 0005 (Authenticated Frames) had a benchmark-only narrative
// and an empty Staff Decision — no implementation anywhere in the tree. This
// suite pins the first implementation (pure TS, zero dependencies) to the
// shared cross-language fixture (fixtures/xlang-verifiedweft/hmac-vectors.json,
// digests cross-checked against node:crypto at generation time) and to the
// same V-series gates the C and Rust ports run:
//
//   V1  HMAC-SHA256 fixture vectors (RFC 4231 TC1-4,6,7 + Weft boundaries)
//   V2  domain-separated key derivation
//   V3  record roundtrip: encode -> decode+verify, zero-copy views
//   V4  exhaustive single-bit tamper detection (768 flips)
//   V5  wrong key / short record / bad magic / geometry overflow
//   V6  constant-time equality semantics
//   V7  signer reuse: N messages through one signer, every tag correct
//
// Byte-compat contract: tags produced here are bit-identical to the C and
// Rust ports (verified cross-language by fixtures/xlang-verifiedweft/run.sh).

import { readFileSync } from 'node:fs';
import { createHmac, createHash } from 'node:crypto';
import { describe, it, expect } from 'vitest';
import {
  Sha256,
  VerifiedWeftSigner,
  ctEq,
  deriveKey,
  hmacSha256,
  verifiedWeftRecordDecodeVerify,
  verifiedWeftRecordEncode,
  verifiedWeftVerify,
  weftEnvelopeEncodeV1,
  VW_OK,
  VW_ERR_TAG,
  VW_ERR_SHORT,
  VW_ERR_BAD_MAGIC,
  HMAC_TAG_LEN,
  VW_KEY_LEN,
  VW_ENVELOPE_LEN,
} from '../src/index';

interface FixtureCase {
  name: string;
  key_hex: string;
  data_hex: string;
  tag_hex: string;
}

const FIXTURE = JSON.parse(
  readFileSync(
    new URL('../../../fixtures/xlang-verifiedweft/hmac-vectors.json', import.meta.url),
    'utf8',
  ),
) as { algorithm: string; cases: FixtureCase[] };

const hexBytes = (hex: string): Uint8Array =>
  Uint8Array.from(
    (hex.match(/.{2}/g) ?? []).map((b) => parseInt(b, 16)),
  );

const hex = (b: Uint8Array): string =>
  Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

describe('V1: HMAC-SHA256 fixture vectors', () => {
  it('matches RFC 4231 TC1-4,6,7 + Weft boundary digests', () => {
    for (const c of FIXTURE.cases) {
      const got = hmacSha256(hexBytes(c.key_hex), hexBytes(c.data_hex));
      expect(hex(got), c.name).toBe(c.tag_hex);
    }
  });

  it('fixture digests agree with node:crypto (ground truth re-check)', () => {
    for (const c of FIXTURE.cases) {
      const ref = createHmac('sha256', Buffer.from(c.key_hex, 'hex'))
        .update(Buffer.from(c.data_hex, 'hex'))
        .digest('hex');
      expect(ref, c.name).toBe(c.tag_hex);
    }
  });
});

describe('V2: domain-separated key derivation', () => {
  it('is deterministic and domain-bound', () => {
    const k1 = deriveKey(new TextEncoder().encode('secret-abc'));
    const k2 = deriveKey(new TextEncoder().encode('secret-abc'));
    const k3 = deriveKey(new TextEncoder().encode('secret-XYZ'));
    expect(k1).toEqual(k2);
    expect(k1).not.toEqual(k3);
    expect(k1.length).toBe(VW_KEY_LEN);
  });
});

describe('V3: record roundtrip (zero-copy decode)', () => {
  it('encode -> decode+verify across payload sizes 0..300', () => {
    const key = deriveKey(new TextEncoder().encode('stream-key'));
    for (const plen of [0, 1, 16, 64, 300]) {
      const env = new Uint8Array(VW_ENVELOPE_LEN);
      weftEnvelopeEncodeV1(env, plen + 1, plen);
      const payload = new Uint8Array(plen);
      for (let i = 0; i < plen; i++) payload[i] = (i * 7 + 1) & 0xff;
      const tag = new Uint8Array(HMAC_TAG_LEN);

      const s = new VerifiedWeftSigner(key);
      s.update(env, 0, VW_ENVELOPE_LEN);
      s.update(payload);
      s.finalize(tag);

      const rec = new Uint8Array(VW_ENVELOPE_LEN + plen + HMAC_TAG_LEN);
      expect(verifiedWeftRecordEncode(env, payload, tag, rec)).toBe(rec.length);

      const r = verifiedWeftRecordDecodeVerify(key, rec);
      expect(r.code, `plen=${plen}`).toBe(VW_OK);
      expect(r.envelope).toEqual(env);
      expect(r.payload).toEqual(payload);
      expect(r.payload.length).toBe(plen);
    }
  });
});

describe('V4: exhaustive single-bit tamper detection', () => {
  it('rejects all 768 single-bit flips of envelope, payload, and tag', () => {
    const key = deriveKey(new TextEncoder().encode('tamper-key'));
    const env = new Uint8Array(VW_ENVELOPE_LEN);
    weftEnvelopeEncodeV1(env, 7, 48);
    const payload = new Uint8Array(48);
    for (let i = 0; i < 48; i++) payload[i] = (i * 13 + 5) & 0xff;
    const tag = new Uint8Array(HMAC_TAG_LEN);
    const s = new VerifiedWeftSigner(key);
    s.update(env, 0, VW_ENVELOPE_LEN);
    s.update(payload);
    s.finalize(tag);

    const flip = (src: Uint8Array, byteI: number, bit: number): Uint8Array => {
      const bad = src.slice();
      bad[byteI] ^= 1 << bit;
      return bad;
    };
    let rejected = 0;
    let total = 0;
    for (let byteI = 0; byteI < VW_ENVELOPE_LEN; byteI++) {
      for (let bit = 0; bit < 8; bit++) {
        if (verifiedWeftVerify(key, flip(env, byteI, bit), payload, tag) === VW_ERR_TAG) rejected++;
        total++;
      }
    }
    for (let byteI = 0; byteI < payload.length; byteI++) {
      for (let bit = 0; bit < 8; bit++) {
        if (verifiedWeftVerify(key, env, flip(payload, byteI, bit), tag) === VW_ERR_TAG) rejected++;
        total++;
      }
    }
    for (let byteI = 0; byteI < tag.length; byteI++) {
      for (let bit = 0; bit < 8; bit++) {
        if (verifiedWeftVerify(key, env, payload, flip(tag, byteI, bit)) === VW_ERR_TAG) rejected++;
        total++;
      }
    }
    expect(rejected).toBe(total);
    expect(total).toBe(768);
  });
});

describe('V5: rejections', () => {
  it('wrong key rejected', () => {
    const key = deriveKey(new TextEncoder().encode('stream-key'));
    const other = deriveKey(new TextEncoder().encode('other-key'));
    const env = new Uint8Array(VW_ENVELOPE_LEN);
    weftEnvelopeEncodeV1(env, 3, 24);
    const payload = new Uint8Array(24).fill(0xab);
    const tag = new Uint8Array(HMAC_TAG_LEN);
    const s = new VerifiedWeftSigner(key);
    s.update(env, 0, VW_ENVELOPE_LEN);
    s.update(payload);
    s.finalize(tag);
    expect(verifiedWeftVerify(other, env, payload, tag)).toBe(VW_ERR_TAG);
  });

  it('short record rejected', () => {
    const key = deriveKey(new TextEncoder().encode('stream-key'));
    const rec = new Uint8Array(40); // < 16 + 32
    expect(verifiedWeftRecordDecodeVerify(key, rec).code).toBe(VW_ERR_SHORT);
  });

  it('bad magic rejected', () => {
    const key = deriveKey(new TextEncoder().encode('stream-key'));
    const env = new Uint8Array(VW_ENVELOPE_LEN);
    weftEnvelopeEncodeV1(env, 3, 24);
    env[0] = 0x58; // 'X'
    const rec = new Uint8Array(16 + 24 + HMAC_TAG_LEN);
    const tag = new Uint8Array(HMAC_TAG_LEN);
    const s = new VerifiedWeftSigner(key);
    s.update(env, 0, VW_ENVELOPE_LEN);
    s.update(new Uint8Array(24).fill(0xab));
    s.finalize(tag);
    verifiedWeftRecordEncode(env, new Uint8Array(24).fill(0xab), tag, rec);
    expect(verifiedWeftRecordDecodeVerify(key, rec).code).toBe(VW_ERR_BAD_MAGIC);
  });

  it('geometry overflow rejected', () => {
    const key = deriveKey(new TextEncoder().encode('stream-key'));
    const env = new Uint8Array(VW_ENVELOPE_LEN);
    weftEnvelopeEncodeV1(env, 3, 24); // claims 24 payload bytes
    const trunc = new Uint8Array(16 + 8); // only 8 present after header
    trunc.set(env, 0);
    expect(verifiedWeftRecordDecodeVerify(key, trunc).code).toBe(VW_ERR_SHORT);
  });
});

describe('V6: constant-time equality semantics', () => {
  it('ctEq matches memcmp semantics', () => {
    const a = new Uint8Array(32);
    const b = new Uint8Array(32);
    expect(ctEq(a, b)).toBe(true);
    b[0] = 1;
    expect(ctEq(a, b)).toBe(false);
    b[0] = 0;
    b[31] = 0x80;
    expect(ctEq(a, b)).toBe(false);
    expect(ctEq(new Uint8Array(0), new Uint8Array(0))).toBe(true);
  });
});

describe('V7: signer reuse', () => {
  it('one signer, many messages — every tag verified', () => {
    const key = deriveKey(new TextEncoder().encode('reuse-key'));
    const s = new VerifiedWeftSigner(key);
    const env = new Uint8Array(VW_ENVELOPE_LEN);
    const tag = new Uint8Array(HMAC_TAG_LEN);
    for (let i = 0; i < 500; i++) {
      weftEnvelopeEncodeV1(env, i + 1, 64);
      const payload = new Uint8Array(64).fill(i & 0xff);
      s.update(env, 0, VW_ENVELOPE_LEN);
      s.update(payload);
      s.finalize(tag);
      // Independent one-shot recomputation must agree.
      const expected = hmacSha256(key, new Uint8Array([...env.slice(0, 16), ...payload]));
      expect(ctEq(tag, expected), `frame ${i}`).toBe(true);
    }
  });
});

describe('Sha256 primitive', () => {
  it('matches node:crypto createHash across block-boundary lengths', () => {
    for (const n of [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 128, 1000]) {
      const data = new Uint8Array(n).fill(0xab);
      const ref = createHash('sha256').update(Buffer.from(data)).digest('hex');
      const h = new Sha256();
      h.update(data);
      const out = new Uint8Array(32);
      h.finalize(out);
      expect(hex(out), `len=${n}`).toBe(ref);
    }
  });
});
