// verified.rs — VerifiedWeft: authenticated frames (RFC 0005).
//
// Driver-layer module (RFC 0005). The frozen kernel (super::kernel semantics
// preserved — lib.rs registers this as `pub mod verified`; no kernel file is
// modified). Byte-compat with core/c/verified.c: same wire format, same key
// schedule, same tags.
//
// Wire format:
//   [envelope v1 16B | payload | HMAC-SHA256 tag 32B]
// Key schedule:
//   auth_key = HMAC-SHA256(secret, b"Weft-VerifiedWeft-v1:key")
//   tag      = HMAC-SHA256(auth_key, envelope[0..16] || payload)
//
// Zero dependencies: SHA-256 (FIPS 180-4) implemented below; verified against
// RFC 4231 vectors shared via fixtures/xlang-verifiedweft/hmac-vectors.json.

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
];

const H0: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
    0x5be0cd19,
];

/// SHA-256 digest length in bytes.
pub const SHA256_DIGEST_LEN: usize = 32;
/// SHA-256 block length in bytes.
pub const SHA256_BLOCK_LEN: usize = 64;

#[derive(Clone)]
/// Streaming SHA-256 state (FIPS 180-4). Reusable; no allocation.
pub struct Sha256 {
    h: [u32; 8],
    total_len: u64,
    block: [u8; SHA256_BLOCK_LEN],
    fill: usize,
}

impl Sha256 {
    /// Fresh hashing state.
    pub fn new() -> Self {
        Sha256 { h: H0, total_len: 0, block: [0u8; SHA256_BLOCK_LEN], fill: 0 }
    }

    /// Fresh state pre-fed with an ipad block (used by HmacKey::new).
    pub fn new_with_ipad(ipad: &[u8; SHA256_BLOCK_LEN]) -> Self {
        let mut s = Sha256 { h: H0, total_len: 0, block: [0u8; SHA256_BLOCK_LEN], fill: 0 };
        s.update(ipad);
        s
    }

    /// Feed message bytes.
    pub fn update(&mut self, mut data: &[u8]) {
        self.total_len = self.total_len.wrapping_add(data.len() as u64);
        if self.fill > 0 {
            let take = core::cmp::min(SHA256_BLOCK_LEN - self.fill, data.len());
            self.block[self.fill..self.fill + take].copy_from_slice(&data[..take]);
            self.fill += take;
            data = &data[take..];
            if self.fill == SHA256_BLOCK_LEN {
                let block = self.block;
                Self::compress(&mut self.h, &block);
                self.fill = 0;
            }
        }
        while data.len() >= SHA256_BLOCK_LEN {
            let (blk, rest) = data.split_at(SHA256_BLOCK_LEN);
            let mut b = [0u8; SHA256_BLOCK_LEN];
            b.copy_from_slice(blk);
            Self::compress(&mut self.h, &b);
            data = rest;
        }
        if !data.is_empty() {
            self.block[self.fill..self.fill + data.len()].copy_from_slice(data);
            self.fill += data.len();
        }
    }

    /// Finalize and return the digest; consumes the state.
    pub fn finalize(mut self) -> [u8; SHA256_DIGEST_LEN] {
        let bit_len = self.total_len.wrapping_mul(8);
        let mut pad = [0u8; 72]; // 0x80 + zeros + 8-byte length (worst case)
        pad[0] = 0x80;
        let pad_len = if self.fill < 56 { 56 - self.fill } else { 120 - self.fill };
        self.update(&pad[..pad_len]);
        // Length field appended without counting it into total_len.
        let mut last = [0u8; SHA256_BLOCK_LEN];
        last[..self.fill].copy_from_slice(&self.block[..self.fill]);
        for i in 0..8 {
            last[56 + i] = (bit_len >> (56 - 8 * i)) as u8;
        }
        Self::compress(&mut self.h, &last);

        let mut out = [0u8; SHA256_DIGEST_LEN];
        for i in 0..8 {
            out[i * 4] = (self.h[i] >> 24) as u8;
            out[i * 4 + 1] = (self.h[i] >> 16) as u8;
            out[i * 4 + 2] = (self.h[i] >> 8) as u8;
            out[i * 4 + 3] = self.h[i] as u8;
        }
        out
    }

    /// One 64-byte block compression round.
    fn compress(h: &mut [u32; 8], block: &[u8; SHA256_BLOCK_LEN]) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                block[i * 4],
                block[i * 4 + 1],
                block[i * 4 + 2],
                block[i * 4 + 3],
            ]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d) = (h[0], h[1], h[2], h[3]);
        let (mut e, mut f, mut g, mut hh) = (h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }
}

/// One-shot SHA-256.
pub fn sha256(data: &[u8]) -> [u8; SHA256_DIGEST_LEN] {
    let mut h = Sha256::new();
    h.update(data);
    h.finalize()
}

// ---------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104)
// ---------------------------------------------------------------------------

/// HMAC-SHA256 tag length in bytes.
pub const HMAC_TAG_LEN: usize = 32;
/// Derived auth-key length in bytes.
pub const VW_KEY_LEN: usize = 32;
/// Signed envelope prefix length (v1 header) in bytes.
pub const VW_ENVELOPE_LEN: usize = 16;

/// Pre-keyed HMAC state (ipad pre-fed; opad stored for finalize).
#[derive(Clone)]
pub struct HmacKey {
    inner: Sha256,
    /// Pre-keyed template (ipad fed, no message bytes). finalize() restores
    /// from this so the key is reusable across messages.
    inner_template: Sha256,
    opad: [u8; SHA256_BLOCK_LEN],
}

impl HmacKey {
    /// Derive the ipad/opad pads and pre-feed the inner hash.
    pub fn new(key: &[u8]) -> Self {
        let mut key_block = [0u8; SHA256_BLOCK_LEN];
        if key.len() > SHA256_BLOCK_LEN {
            key_block[..SHA256_DIGEST_LEN].copy_from_slice(&sha256(key));
        } else {
            key_block[..key.len()].copy_from_slice(key);
        }
        let mut ipad = [0u8; SHA256_BLOCK_LEN];
        let mut opad = [0u8; SHA256_BLOCK_LEN];
        for i in 0..SHA256_BLOCK_LEN {
            ipad[i] = key_block[i] ^ 0x36;
            opad[i] = key_block[i] ^ 0x5c;
        }
        let inner = Sha256::new_with_ipad(&ipad);
        HmacKey { inner_template: inner.clone(), inner, opad }
    }

    /// Feed message bytes.
    pub fn update(&mut self, data: &[u8]) {
        self.inner.update(data);
    }

    /// Finalize the current message; state resets to the pre-keyed template
    /// so the key is immediately reusable for the next message.
    pub fn finalize(&mut self) -> [u8; HMAC_TAG_LEN] {
        // Take inner state by clone so the template is untouched.
        let inner = self.inner.clone();
        let inner_digest = inner.finalize();
        let mut outer = Sha256::new();
        outer.update(&self.opad);
        outer.update(&inner_digest);
        self.inner = self.inner_template.clone();
        outer.finalize()
    }
}

/// One-shot HMAC-SHA256.
pub fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; HMAC_TAG_LEN] {
    let mut k = HmacKey::new(key);
    k.update(data);
    k.finalize()
}

// ---------------------------------------------------------------------------
// VerifiedWeft (RFC 0005)
// ---------------------------------------------------------------------------

/// Domain-separation label for auth-key derivation.
pub const VW_DOMAIN: &[u8] = b"Weft-VerifiedWeft-v1:key";

/// Verify result codes — numeric parity with weft_vw_result_t in core/c.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VwResult {
    /// Verification succeeded.
    Ok = 0,
    /// Record shorter than envelope + tag, or geometry overflow.
    ErrShort = 1,
    /// Magic mismatch or malformed header geometry.
    ErrBadMagic = 2,
    /// HMAC mismatch — tampered or wrong key.
    ErrTag = 3,
}

/// One-time key derivation with domain separation.
pub fn derive_key(secret: &[u8]) -> [u8; VW_KEY_LEN] {
    hmac_sha256(secret, VW_DOMAIN)
}

/// Reusable per-stream signer (pre-keyed; clone-free hot path).
pub struct VwSigner {
    key: HmacKey,
}

impl VwSigner {
    /// Pre-key the signer from a derived auth key.
    pub fn new(auth_key: &[u8; VW_KEY_LEN]) -> Self {
        VwSigner { key: HmacKey::new(auth_key) }
    }

    /// tag = HMAC(auth_key, envelope[0..16] || payload).
    /// Sign one frame: tag = HMAC(auth_key, envelope[0..16] || payload).
    pub fn sign(&mut self, envelope: &[u8], payload: &[u8], out_tag: &mut [u8; HMAC_TAG_LEN]) {
        self.key.update(&envelope[..VW_ENVELOPE_LEN]);
        self.key.update(payload);
        *out_tag = self.key.finalize();
    }
}

/// Constant-time equality (length-only timing dependence).
pub fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for i in 0..a.len() {
        diff |= a[i] ^ b[i];
    }
    diff == 0
}

/// Verify one frame record's fields. Constant-time tag compare.
pub fn verify(
    auth_key: &[u8; VW_KEY_LEN],
    envelope: &[u8],
    payload: &[u8],
    tag: &[u8; HMAC_TAG_LEN],
) -> VwResult {
    let mut k = HmacKey::new(auth_key);
    k.update(&envelope[..VW_ENVELOPE_LEN]);
    k.update(payload);
    let expect = k.finalize();
    if !ct_eq(&expect, tag) {
        return VwResult::ErrTag;
    }
    VwResult::Ok
}

/// Encode a full auth record into `dst`. Returns bytes written, or 0 if
/// `dst` is too small.
pub fn record_encode(
    envelope: &[u8],
    payload: &[u8],
    tag: &[u8; HMAC_TAG_LEN],
    dst: &mut [u8],
) -> usize {
    let total = VW_ENVELOPE_LEN + payload.len() + HMAC_TAG_LEN;
    if dst.len() < total || envelope.len() < VW_ENVELOPE_LEN {
        return 0;
    }
    dst[..VW_ENVELOPE_LEN].copy_from_slice(&envelope[..VW_ENVELOPE_LEN]);
    dst[VW_ENVELOPE_LEN..VW_ENVELOPE_LEN + payload.len()].copy_from_slice(payload);
    dst[VW_ENVELOPE_LEN + payload.len()..total].copy_from_slice(tag);
    total
}

/// Decode + verify a record from wire bytes. On Ok, returns zero-copy views
/// into `src` as (envelope, payload) slices plus the payload length.
pub fn record_decode_verify<'a>(
    auth_key: &[u8; VW_KEY_LEN],
    src: &'a [u8],
) -> Result<(&'a [u8], &'a [u8], usize), VwResult> {
    if src.len() < VW_ENVELOPE_LEN + HMAC_TAG_LEN {
        return Err(VwResult::ErrShort);
    }
    if &src[0..4] != b"WEFT" {
        return Err(VwResult::ErrBadMagic);
    }
    // Envelope v1 (03-ENVELOPE §1, little-endian):
    //   header_size [6..8), seq [8..12), payload_len [12..16).
    let header_size = u16::from_le_bytes([src[6], src[7]]) as usize;
    let plen = u32::from_le_bytes([src[12], src[13], src[14], src[15]]) as usize;

    if header_size < VW_ENVELOPE_LEN {
        return Err(VwResult::ErrBadMagic);
    }
    let body = header_size + plen;
    if body > src.len() - HMAC_TAG_LEN {
        return Err(VwResult::ErrShort);
    }

    let tag_start = body;
    let mut tag = [0u8; HMAC_TAG_LEN];
    tag.copy_from_slice(&src[tag_start..tag_start + HMAC_TAG_LEN]);

    // verify() can only fail with ErrTag here — geometry is already checked —
    // but propagate whatever it returns for exactness.
    let vr = verify(auth_key, &src[..VW_ENVELOPE_LEN], &src[header_size..body], &tag);
    if vr != VwResult::Ok {
        return Err(vr);
    }
    Ok((&src[..VW_ENVELOPE_LEN], &src[header_size..body], plen))
}

// ---------------------------------------------------------------------------
// V-series conformance (mirrors core/c/verified_test.c; same shared fixture)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    struct Vector {
        name: &'static str,
        key: &'static str,
        data: &'static str,
        tag: &'static str,
    }

    // Byte-identical to fixtures/xlang-verifiedweft/hmac-vectors.json
    // (RFC 4231 TC1-4,6,7 + Weft boundary cases; node:crypto cross-checked).
    const VECTORS: [Vector; 8] = [
        Vector {
            name: "rfc4231-tc1",
            key: concat!(
                "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"
            ),
            data: concat!(
                "4869205468657265"
            ),
            tag: "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        },
        Vector {
            name: "rfc4231-tc2",
            key: concat!(
                "4a656665"
            ),
            data: concat!(
                "7768617420646f2079612077616e7420666f72206e6f7468696e673f"
            ),
            tag: "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        },
        Vector {
            name: "rfc4231-tc3",
            key: concat!(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            ),
            data: concat!(
                "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
                "dddddddddddddddddddddddddddddddddddd"
            ),
            tag: "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
        },
        Vector {
            name: "rfc4231-tc4",
            key: concat!(
                "0102030405060708090a0b0c0d0e0f10111213141516171819"
            ),
            data: concat!(
                "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
                "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
            ),
            tag: "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
        },
        Vector {
            name: "rfc4231-tc6",
            key: concat!(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaa"
            ),
            data: concat!(
                "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a",
                "65204b6579202d2048617368204b6579204669727374"
            ),
            tag: "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
        },
        Vector {
            name: "rfc4231-tc7",
            key: concat!(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "aaaaaa"
            ),
            data: concat!(
                "5468697320697320612074657374207573696e672061206c6172676572207468",
                "616e20626c6f636b2d73697a65206b657920616e642061206c61726765722074",
                "68616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565",
                "647320746f20626520686173686564206265666f7265206265696e6720757365",
                "642062792074686520484d414320616c676f726974686d2e"
            ),
            tag: "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
        },
        Vector {
            name: "weft-empty-payload",
            key: concat!(
                "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"
            ),
            data: concat!(
                ""
            ),
            tag: "87a26610b4e32f22d6d403b2397f534fb64c83b15aa53deaec60b1afa31dbb74",
        },
        Vector {
            name: "weft-one-byte",
            key: concat!(
                "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"
            ),
            data: concat!(
                "ff"
            ),
            tag: "869b6896716dbdbce95aa32d75657fae807c82b8d52c25c83b7afa617271b7c8",
        },
    ];

    fn hex_bytes(hex: &str) -> Vec<u8> {
        (0..hex.len() / 2)
            .map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{:02x}", b)).collect()
    }

    /// V1: fixture vectors.
    #[test]
    fn v1_hmac_vectors() {
        for v in VECTORS.iter() {
            let got = hmac_sha256(&hex_bytes(v.key), &hex_bytes(v.data));
            assert_eq!(hex(&got), v.tag, "vector {}", v.name);
        }
    }

    /// V2: domain-separated key derivation.
    #[test]
    fn v2_derive_key() {
        let k1 = derive_key(b"secret-abc");
        let k2 = derive_key(b"secret-abc");
        let k3 = derive_key(b"secret-XYZ");
        assert_eq!(k1, k2, "deterministic");
        assert_ne!(k1, k3, "different secret -> different key");
        assert_eq!(k1, hmac_sha256(b"secret-abc", VW_DOMAIN), "key = HMAC(secret, domain)");
    }

    /// V3: record roundtrip with zero-copy decode across payload sizes.
    #[test]
    fn v3_record_roundtrip() {
        let key = derive_key(b"stream-key");
        for plen in [0usize, 64, 128, 300] {
            let mut envelope = [0u8; 16];
            envelope[..4].copy_from_slice(b"WEFT");
            envelope[6] = 16; // header_size LE
            envelope[8..12].copy_from_slice(&((plen as u32) + 1).to_le_bytes()); // seq
            envelope[12..16].copy_from_slice(&(plen as u32).to_le_bytes()); // payload_len
            let payload: Vec<u8> = (0..plen).map(|i| (i * 7 + 1) as u8).collect();

            let mut s = VwSigner::new(&key);
            let mut tag = [0u8; HMAC_TAG_LEN];
            s.sign(&envelope, &payload, &mut tag);

            let mut record = vec![0u8; 16 + plen + 32];
            let n = record_encode(&envelope, &payload, &tag, &mut record);
            assert_eq!(n, 16 + plen + 32);

            let (env2, pay2, plen2) = record_decode_verify(&key, &record).unwrap();
            assert_eq!(plen2, plen);
            assert_eq!(env2, &envelope[..]);
            assert_eq!(pay2, &payload[..]);
        }
    }

    /// V4: exhaustive single-bit tamper detection (768 flips).
    #[test]
    fn v4_tamper() {
        let key = derive_key(b"tamper-key");
        let mut envelope = [0u8; 16];
        envelope[..4].copy_from_slice(b"WEFT");
        envelope[6] = 16;
        envelope[8] = 7;
        envelope[12] = 48;
        let payload: Vec<u8> = (0..48usize).map(|i| (i * 13 + 5) as u8).collect();
        let mut s = VwSigner::new(&key);
        let mut tag = [0u8; HMAC_TAG_LEN];
        s.sign(&envelope, &payload, &mut tag);

        let mut rejected = 0u32;
        let mut total = 0u32;
        for byte_i in 0..16usize {
            for bit in 0..8u8 {
                let mut bad = envelope;
                bad[byte_i] ^= 1 << bit;
                if verify(&key, &bad, &payload, &tag) == VwResult::ErrTag {
                    rejected += 1;
                }
                total += 1;
            }
        }
        for byte_i in 0..48usize {
            for bit in 0..8u8 {
                let mut bad = payload.clone();
                bad[byte_i] ^= 1 << bit;
                if verify(&key, &envelope, &bad, &tag) == VwResult::ErrTag {
                    rejected += 1;
                }
                total += 1;
            }
        }
        for byte_i in 0..HMAC_TAG_LEN {
            for bit in 0..8u8 {
                let mut bad = tag;
                bad[byte_i] ^= 1 << bit;
                if verify(&key, &envelope, &payload, &bad) == VwResult::ErrTag {
                    rejected += 1;
                }
                total += 1;
            }
        }
        assert_eq!(rejected, total, "all single-bit flips rejected");
    }

    /// V5: wrong key / short record / bad magic / geometry overflow.
    #[test]
    fn v5_rejections() {
        let key = derive_key(b"stream-key");
        let other = derive_key(b"other-key");
        let mut envelope = [0u8; 16];
        envelope[..4].copy_from_slice(b"WEFT");
        envelope[6] = 16;
        envelope[8] = 3;
        envelope[12] = 24;
        let payload = vec![0xABu8; 24];
        let mut s = VwSigner::new(&key);
        let mut tag = [0u8; HMAC_TAG_LEN];
        s.sign(&envelope, &payload, &mut tag);

        assert_eq!(verify(&other, &envelope, &payload, &tag), VwResult::ErrTag);

        let short_rec = [0u8; 40];
        assert_eq!(record_decode_verify(&key, &short_rec), Err(VwResult::ErrShort));

        let mut bad_magic = envelope;
        bad_magic[0] = b'X';
        let mut rec = vec![0u8; 16 + 24 + 32];
        let t2 = tag;
        assert_eq!(record_encode(&bad_magic, &payload, &t2, &mut rec), 16 + 24 + 32);
        assert_eq!(record_decode_verify(&key, &rec), Err(VwResult::ErrBadMagic));

        let mut trunc = [0u8; 24];
        trunc[..16].copy_from_slice(&envelope);
        assert_eq!(record_decode_verify(&key, &trunc), Err(VwResult::ErrShort));
    }

    /// V6: constant-time equality semantics.
    #[test]
    fn v6_ct_eq() {
        let a = [0u8; 32];
        let mut b = [0u8; 32];
        assert!(ct_eq(&a, &b));
        b[0] = 1;
        assert!(!ct_eq(&a, &b));
        b[0] = 0;
        b[31] = 0x80;
        assert!(!ct_eq(&a, &b));
        assert!(ct_eq(&[], &[]));
    }

    /// V7: per-frame cost vs RFC 0005 claims (soft gate < 10 us rtt;
    /// RFC claims 2.10 enc / 2.09 dec / 4.19 rtt at 64B payload).
    #[test]
    fn v7_perf_vs_claims() {
        let key = derive_key(b"bench-key");
        let frames = 100_000usize;
        let mut envelope = [0u8; 16];
        envelope[..4].copy_from_slice(b"WEFT");
        envelope[6] = 16;
        envelope[12] = 64;
        let payload = vec![0x5Au8; 64];
        let mut tags = vec![[0u8; HMAC_TAG_LEN]; frames];

        let t0 = std::time::Instant::now();
        let mut s = VwSigner::new(&key);
        for i in 0..frames {
            envelope[8] = i as u8;
            s.sign(&envelope, &payload, &mut tags[i]);
        }
        let enc_us = t0.elapsed().as_secs_f64() * 1e6 / frames as f64;

        let t0 = std::time::Instant::now();
        for i in 0..frames {
            envelope[8] = i as u8;
            assert_eq!(verify(&key, &envelope, &payload, &tags[i]), VwResult::Ok);
        }
        let dec_us = t0.elapsed().as_secs_f64() * 1e6 / frames as f64;

        println!(
            "V7 measured: enc {:.2} us | dec {:.2} us | rtt {:.2} us",
            enc_us,
            dec_us,
            enc_us + dec_us
        );
        assert!(enc_us + dec_us < 10.0, "rtt {:.2} us >= 10 us", enc_us + dec_us);
    }
}
