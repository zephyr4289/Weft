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
        if data.len() >= SHA256_BLOCK_LEN {
            // Contiguous run: one dispatch for the whole span (multi-block
            // HW transforms amortize the state shuffle across the run).
            let blocks = data.len() / SHA256_BLOCK_LEN;
            unsafe { transform_dispatch()(&mut self.h, data.as_ptr(), blocks) };
            data = &data[blocks * SHA256_BLOCK_LEN..];
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
        unsafe { transform_dispatch()(&mut self.h, last.as_ptr(), 1) };

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


// ---------------------------------------------------------------------------
// Hardware acceleration (Series 6) — mirrors core/c/sha256_hw.c
// ---------------------------------------------------------------------------
// Runtime-dispatched accelerated compression: x86 SHA extensions and ARMv8
// Crypto Extensions. The scalar path above stays the normative reference;
// digests are bit-identical across regimes (v8_hw_equivalence gate), same
// contract as the C port. Provenance note in core/c/sha256_hw.c applies
// here too (public-domain Intel/Gulley/Walton layout, schedule re-derived
// independently during the C port and mirrored here).

/// Which compression implementation is active (parity with weft_sha256_impl_t).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Sha256Impl {
    /// Portable scalar reference.
    Scalar,
    /// x86 SHA extensions.
    X86ShaNi,
    /// ARMv8 Crypto Extensions.
    ArmCe,
}

/// Multi-block transform contract: compress `blocks` x 64-byte big-endian
/// blocks into state. Same signature for scalar and accelerated paths.
type TransformFn = unsafe fn(&mut [u32; 8], *const u8, usize);

unsafe fn transform_scalar(h: &mut [u32; 8], data: *const u8, blocks: usize) {
    for b in 0..blocks {
        let block: &[u8; SHA256_BLOCK_LEN] =
            unsafe { &*data.add(b * SHA256_BLOCK_LEN).cast() };
        Sha256::compress(h, block);
    }
}

static G_TRANSFORM: std::sync::OnceLock<TransformFn> = std::sync::OnceLock::new();

fn transform_dispatch() -> TransformFn {
    *G_TRANSFORM.get_or_init(|| {
        #[cfg(target_arch = "x86_64")]
        {
            if std::is_x86_feature_detected!("sha") {
                return transform_x86_ni;
            }
        }
        #[cfg(target_arch = "aarch64")]
        {
            if std::is_aarch64_feature_detected!("sha2") {
                return transform_arm_ce;
            }
        }
        transform_scalar
    })
}

/// Report the active implementation (tests + evidence logs name the regime).
pub fn sha256_active_impl() -> Sha256Impl {
    #[cfg(target_arch = "x86_64")]
    {
        if std::is_x86_feature_detected!("sha") {
            return Sha256Impl::X86ShaNi;
        }
    }
    #[cfg(target_arch = "aarch64")]
    {
        if std::is_aarch64_feature_detected!("sha2") {
            return Sha256Impl::ArmCe;
        }
    }
    Sha256Impl::Scalar
}

#[cfg(target_arch = "x86_64")]
mod hw_x86 {
    use core::arch::x86_64::*;

    /// x86 SHA-NI multi-block transform — port of the C implementation in
    /// core/c/sha256_hw.c (validated there against the scalar reference on
    /// 3000 randomized buffers + NIST vectors before mirroring).
    #[target_feature(enable = "sha,sse4.1")]
    pub unsafe fn transform_x86_ni(state: &mut [u32; 8], data: *const u8, blocks: usize) {
      unsafe {
        let mask = _mm_set_epi64x(0x0c0d0e0f08090a0bu64 as i64, 0x0405060700010203u64 as i64);

        // state -> rnds2 domain: state0 = [F,E,B,A], state1 = [H,G,D,C]
        let l0 = _mm_loadu_si128(state.as_ptr().cast());
        let l1 = _mm_loadu_si128(state.as_ptr().add(4).cast());
        let mut tmp = _mm_shuffle_epi32(l0, 0xB1);
        let mut state1 = _mm_shuffle_epi32(l1, 0x1B);
        let mut state0 = _mm_alignr_epi8(tmp, state1, 8);
        state1 = _mm_blend_epi16(state1, tmp, 0xF0);

        for blk in 0..blocks {
            let d = unsafe { data.add(blk * 64) };
            let save0 = state0;
            let save1 = state1;

            let mut t0 = _mm_shuffle_epi8(_mm_loadu_si128(d.cast()), mask);
            let mut t1 = _mm_shuffle_epi8(_mm_loadu_si128(d.add(16).cast()), mask);
            let mut t2 = _mm_shuffle_epi8(_mm_loadu_si128(d.add(32).cast()), mask);
            let mut t3 = _mm_shuffle_epi8(_mm_loadu_si128(d.add(48).cast()), mask);

        // group 0 (rounds 0-3): consume block 0 (t0)
        let mut msg = _mm_add_epi32(t0, _mm_set_epi64x(0xE9B5DBA5B5C0FBCFu64 as i64, 0x71374491428A2F98u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        let _ = msg;

        // group 1 (rounds 4-7): consume block 1 (t1)
        let mut msg = _mm_add_epi32(t1, _mm_set_epi64x(0xAB1C5ED5923F82A4u64 as i64, 0x59F111F13956C25Bu64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t0 = _mm_sha256msg1_epu32(t0, t1); // prep block 4
        let _ = msg;

        // group 2 (rounds 8-11): consume block 2 (t2)
        let mut msg = _mm_add_epi32(t2, _mm_set_epi64x(0x550C7DC3243185BEu64 as i64, 0x12835B01D807AA98u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t1 = _mm_sha256msg1_epu32(t1, t2); // prep block 5
        let _ = msg;

        // group 3 (rounds 12-15): consume block 3 (t3)
        let mut msg = _mm_add_epi32(t3, _mm_set_epi64x(0xC19BF1749BDC06A7u64 as i64, 0x80DEB1FE72BE5D74u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t3, t2, 4); // W[5..8]
        t0 = _mm_add_epi32(t0, tmp);
        t0 = _mm_sha256msg2_epu32(t0, t3);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t2 = _mm_sha256msg1_epu32(t2, t3); // prep block 6
        let _ = msg;

        // group 4 (rounds 16-19): consume block 4 (t0)
        let mut msg = _mm_add_epi32(t0, _mm_set_epi64x(0x240CA1CC0FC19DC6u64 as i64, 0xEFBE4786E49B69C1u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t0, t3, 4); // W[9..12]
        t1 = _mm_add_epi32(t1, tmp);
        t1 = _mm_sha256msg2_epu32(t1, t0);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t3 = _mm_sha256msg1_epu32(t3, t0); // prep block 7
        let _ = msg;

        // group 5 (rounds 20-23): consume block 5 (t1)
        let mut msg = _mm_add_epi32(t1, _mm_set_epi64x(0x76F988DA5CB0A9DCu64 as i64, 0x4A7484AA2DE92C6Fu64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t1, t0, 4); // W[13..16]
        t2 = _mm_add_epi32(t2, tmp);
        t2 = _mm_sha256msg2_epu32(t2, t1);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t0 = _mm_sha256msg1_epu32(t0, t1); // prep block 8
        let _ = msg;

        // group 6 (rounds 24-27): consume block 6 (t2)
        let mut msg = _mm_add_epi32(t2, _mm_set_epi64x(0xBF597FC7B00327C8u64 as i64, 0xA831C66D983E5152u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t2, t1, 4); // W[17..20]
        t3 = _mm_add_epi32(t3, tmp);
        t3 = _mm_sha256msg2_epu32(t3, t2);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t1 = _mm_sha256msg1_epu32(t1, t2); // prep block 9
        let _ = msg;

        // group 7 (rounds 28-31): consume block 7 (t3)
        let mut msg = _mm_add_epi32(t3, _mm_set_epi64x(0x1429296706CA6351u64 as i64, 0xD5A79147C6E00BF3u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t3, t2, 4); // W[21..24]
        t0 = _mm_add_epi32(t0, tmp);
        t0 = _mm_sha256msg2_epu32(t0, t3);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t2 = _mm_sha256msg1_epu32(t2, t3); // prep block 10
        let _ = msg;

        // group 8 (rounds 32-35): consume block 8 (t0)
        let mut msg = _mm_add_epi32(t0, _mm_set_epi64x(0x53380D134D2C6DFCu64 as i64, 0x2E1B213827B70A85u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t0, t3, 4); // W[25..28]
        t1 = _mm_add_epi32(t1, tmp);
        t1 = _mm_sha256msg2_epu32(t1, t0);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t3 = _mm_sha256msg1_epu32(t3, t0); // prep block 11
        let _ = msg;

        // group 9 (rounds 36-39): consume block 9 (t1)
        let mut msg = _mm_add_epi32(t1, _mm_set_epi64x(0x92722C8581C2C92Eu64 as i64, 0x766A0ABB650A7354u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t1, t0, 4); // W[29..32]
        t2 = _mm_add_epi32(t2, tmp);
        t2 = _mm_sha256msg2_epu32(t2, t1);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t0 = _mm_sha256msg1_epu32(t0, t1); // prep block 12
        let _ = msg;

        // group 10 (rounds 40-43): consume block 10 (t2)
        let mut msg = _mm_add_epi32(t2, _mm_set_epi64x(0xC76C51A3C24B8B70u64 as i64, 0xA81A664BA2BFE8A1u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t2, t1, 4); // W[33..36]
        t3 = _mm_add_epi32(t3, tmp);
        t3 = _mm_sha256msg2_epu32(t3, t2);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t1 = _mm_sha256msg1_epu32(t1, t2); // prep block 13
        let _ = msg;

        // group 11 (rounds 44-47): consume block 11 (t3)
        let mut msg = _mm_add_epi32(t3, _mm_set_epi64x(0x106AA070F40E3585u64 as i64, 0xD6990624D192E819u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t3, t2, 4); // W[37..40]
        t0 = _mm_add_epi32(t0, tmp);
        t0 = _mm_sha256msg2_epu32(t0, t3);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t2 = _mm_sha256msg1_epu32(t2, t3); // prep block 14
        let _ = msg;

        // group 12 (rounds 48-51): consume block 12 (t0)
        let mut msg = _mm_add_epi32(t0, _mm_set_epi64x(0x34B0BCB52748774Cu64 as i64, 0x1E376C0819A4C116u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t0, t3, 4); // W[41..44]
        t1 = _mm_add_epi32(t1, tmp);
        t1 = _mm_sha256msg2_epu32(t1, t0);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        t3 = _mm_sha256msg1_epu32(t3, t0); // prep block 15
        let _ = msg;

        // group 13 (rounds 52-55): consume block 13 (t1)
        let mut msg = _mm_add_epi32(t1, _mm_set_epi64x(0x682E6FF35B9CCA4Fu64 as i64, 0x4ED8AA4A391C0CB3u64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t1, t0, 4); // W[45..48]
        t2 = _mm_add_epi32(t2, tmp);
        t2 = _mm_sha256msg2_epu32(t2, t1);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        let _ = msg;

        // group 14 (rounds 56-59): consume block 14 (t2)
        let mut msg = _mm_add_epi32(t2, _mm_set_epi64x(0x8CC7020884C87814u64 as i64, 0x78A5636F748F82EEu64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        let tmp = _mm_alignr_epi8(t2, t1, 4); // W[49..52]
        t3 = _mm_add_epi32(t3, tmp);
        t3 = _mm_sha256msg2_epu32(t3, t2);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        let _ = msg;

        // group 15 (rounds 60-63): consume block 15 (t3)
        let mut msg = _mm_add_epi32(t3, _mm_set_epi64x(0xC67178F2BEF9A3F7u64 as i64, 0xA4506CEB90BEFFFAu64 as i64));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        let _ = msg;


            state0 = _mm_add_epi32(state0, save0);
            state1 = _mm_add_epi32(state1, save1);
        }

        // rnds2 domain -> state layout
        tmp = _mm_shuffle_epi32(state0, 0x1B);
        state1 = _mm_shuffle_epi32(state1, 0xB1);
        state0 = _mm_blend_epi16(tmp, state1, 0xF0);
        state1 = _mm_alignr_epi8(state1, tmp, 8);
        _mm_storeu_si128(state.as_mut_ptr().cast(), state0);
        _mm_storeu_si128(state.as_mut_ptr().add(4).cast(), state1);
      }
    }
}

#[cfg(target_arch = "x86_64")]
use hw_x86::transform_x86_ni;

// aarch64 CE: mirrors the C compile-guarded path — NOT executable-tested on
// the x86_64 sandbox; ARM CI (apple/linux-arm64 runners) covers it. The
// v8 equivalence test is cfg-gated per architecture.
#[cfg(target_arch = "aarch64")]
mod hw_arm {
    use core::arch::aarch64::*;

    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];

    #[target_feature(enable = "sha2")]
    pub unsafe fn transform_arm_ce(state: &mut [u32; 8], data: *const u8, blocks: usize) {
        let mut state0 = unsafe { vld1q_u32(state.as_ptr()) };   // A B C D
        let mut state1 = unsafe { vld1q_u32(state.as_ptr().add(4)) }; // E F G H

        for blk in 0..blocks {
            let d = unsafe { data.add(blk * 64) };
            let save0 = state0;
            let save1 = state1;

            let mut w0 = unsafe { vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(d))) };
            let mut w1 = unsafe { vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(d.add(16)))) };
            let mut w2 = unsafe { vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(d.add(32)))) };
            let mut w3 = unsafe { vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(d.add(48)))) };

            for i in 0..16usize {
                let wk;
                if i < 4 {
                    let w = match i { 0 => w0, 1 => w1, 2 => w2, _ => w3 };
                    wk = unsafe { vaddq_u32(w, vld1q_u32(K.as_ptr().add(4 * i))) };
                } else {
                    // su0/su1 extend the rotating schedule vector in place
                    // (same index math as the C port, see sha256_hw.c).
                    let m = i % 4;
                    let cur = match m { 0 => &mut w0, 1 => &mut w1, 2 => &mut w2, _ => &mut w3 };
                    let wnext;
                    let wback2;
                    let wback1;
                    match m {
                        0 => { wnext = &mut w1; wback2 = &mut w2; wback1 = &mut w3; }
                        1 => { wnext = &mut w2; wback2 = &mut w3; wback1 = &mut w0; }
                        2 => { wnext = &mut w3; wback2 = &mut w0; wback1 = &mut w1; }
                        _ => { wnext = &mut w0; wback2 = &mut w1; wback1 = &mut w2; }
                    }
                    *cur = unsafe { vsha256su0q_u32(*cur, *wnext) };
                    *cur = unsafe { vsha256su1q_u32(*cur, *wback2, *wback1) };
                    wk = unsafe { vaddq_u32(*cur, vld1q_u32(K.as_ptr().add(4 * i))) };
                }
                let tmp = state0;
                state0 = unsafe { vsha256hq_u32(state0, state1, wk) };
                state1 = unsafe { vsha256h2q_u32(state1, tmp, wk) };
            }

            state0 = unsafe { vaddq_u32(state0, save0) };
            state1 = unsafe { vaddq_u32(state1, save1) };
        }

        unsafe { vst1q_u32(state.as_mut_ptr(), state0) };
        unsafe { vst1q_u32(state.as_mut_ptr().add(4), state1) };
    }
}

#[cfg(target_arch = "aarch64")]
use hw_arm::transform_arm_ce;

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


/// Reusable pre-keyed verifier (Series 6) — the verify mirror of VwSigner.
/// Identical accept/reject semantics to verify(); only the HMAC key schedule
/// is amortized across the stream. No allocation.
pub struct VwVerifier {
    key: HmacKey,
}

impl VwVerifier {
    /// Pre-key from a derived auth key.
    pub fn new(auth_key: &[u8; VW_KEY_LEN]) -> Self {
        VwVerifier { key: HmacKey::new(auth_key) }
    }

    /// Verify one frame with the pre-keyed state (constant-time compare).
    pub fn verify(
        &mut self,
        envelope: &[u8],
        payload: &[u8],
        tag: &[u8; HMAC_TAG_LEN],
    ) -> VwResult {
        self.key.update(&envelope[..VW_ENVELOPE_LEN]);
        self.key.update(payload);
        let expect = self.key.finalize(); // reseeds — reusable
        if !ct_eq(&expect, tag) {
            return VwResult::ErrTag;
        }
        VwResult::Ok
    }
}

/// Zero-copy view of one verified record inside a batch buffer.
#[derive(Debug)]
pub struct VwRecordView<'a> {
    /// 16-byte envelope prefix (slice into the batch buffer).
    pub envelope: &'a [u8],
    /// Payload bytes (slice into the batch buffer).
    pub payload: &'a [u8],
    /// Envelope seq field, decoded little-endian.
    pub seq: u32,
}

/// Walk a buffer of concatenated auth records, verifying each in order
/// (Series 6 stream-consumer path; Law 4 drop-and-count preserved).
///
/// Returns `(result, n_verified, bytes_consumed)`:
///   - `(Ok, n, len)` when every record verifies — `n` records, full walk
///   - a bad record stops the walk: its code, the good-prefix count, and the
///     offset where it starts (resync point for stream consumers)
///   - trailing bytes shorter than a record are ignored (truncation policy
///     belongs to the caller)
/// `views`, when given, is cleared and then filled with zero-copy views of
/// the VERIFIED records only — its capacity is the caller's memory budget.
pub fn batch_decode_verify<'a>(
    auth_key: &[u8; VW_KEY_LEN],
    src: &'a [u8],
    mut views: Option<&mut Vec<VwRecordView<'a>>>,
) -> (VwResult, usize, usize) {
    let mut v = VwVerifier::new(auth_key);
    if let Some(views) = views.as_mut() {
        views.clear();
    }
    let mut verified = 0usize;
    let mut off = 0usize;
    while off + VW_ENVELOPE_LEN + HMAC_TAG_LEN <= src.len() {
        let rec = &src[off..];
        if &rec[0..4] != b"WEFT" {
            return (VwResult::ErrBadMagic, verified, off);
        }
        let header_size = u16::from_le_bytes([rec[6], rec[7]]) as usize;
        let plen = u32::from_le_bytes([rec[12], rec[13], rec[14], rec[15]]) as usize;
        if header_size < VW_ENVELOPE_LEN {
            return (VwResult::ErrBadMagic, verified, off);
        }
        let body = header_size + plen;
        if body > src.len() - off - HMAC_TAG_LEN {
            return (VwResult::ErrShort, verified, off);
        }
        let mut tag = [0u8; HMAC_TAG_LEN];
        tag.copy_from_slice(&rec[body..body + HMAC_TAG_LEN]);
        let vr = v.verify(&rec[..VW_ENVELOPE_LEN], &rec[header_size..body], &tag);
        if vr != VwResult::Ok {
            return (vr, verified, off);
        }
        if let Some(views) = views.as_mut() {
            views.push(VwRecordView {
                envelope: &rec[..VW_ENVELOPE_LEN],
                payload: &rec[header_size..body],
                seq: u32::from_le_bytes([rec[8], rec[9], rec[10], rec[11]]),
            });
        }
        verified += 1;
        off += body + HMAC_TAG_LEN;
    }
    (VwResult::Ok, verified, off)
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

    /// V8: HW dispatch equivalence — accelerated digests identical to scalar
    /// (randomized sweep, mirrors core/c V8; the fixture vectors in V1 pin
    /// the same property against ground truth on every run).
    #[test]
    fn v8_hw_equivalence() {
        let active = sha256_active_impl();
        println!("V8: active impl = {:?}", active);

        let mut rng: u32 = 0x9E37_79B9;
        let mut mismatches = 0usize;
        for _ in 0..256 {
            let n = (rng as usize) % 512;
            let mut buf = vec![0u8; n];
            for b in buf.iter_mut() {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                *b = rng as u8;
            }
            // Scalar reference.
            let mut h_scalar = Sha256::new();
            h_scalar.update(&buf);
            let d_scalar = h_scalar.finalize();
            // Dispatched path (HW when available) — sha256() goes through
            // update(), which dispatches.
            let d_auto = sha256(&buf);
            if d_scalar != d_auto {
                mismatches += 1;
            }
        }
        assert_eq!(mismatches, 0, "256 random buffers digest-identical");

        // Direct transform A/B when an accelerator exists on this CPU.
        #[cfg(target_arch = "x86_64")]
        {
            if std::is_x86_feature_detected!("sha") {
                let mut blocks = [0u8; 64 * 4];
                let mut seed = 0x1234_5678u32;
                for b in blocks.iter_mut() {
                    seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
                    *b = seed as u8;
                }
                let mut h_scalar = [0x6a09e667u32, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
                let mut h_ni = h_scalar;
                unsafe {
                    transform_scalar(&mut h_scalar, blocks.as_ptr(), 4);
                    transform_x86_ni(&mut h_ni, blocks.as_ptr(), 4);
                }
                assert_eq!(h_scalar, h_ni, "scalar vs SHA-NI transform state identical");
            }
        }
    }

    /// V9: pre-keyed verifier == one-shot verify across a stream.
    #[test]
    fn v9_prekeyed_verifier() {
        let key = derive_key(b"v9-stream");
        let mut v = VwVerifier::new(&key);
        let mut s = VwSigner::new(&key);
        let mut envelope = [0u8; 16];
        envelope[..4].copy_from_slice(b"WEFT");
        envelope[6] = 16;
        envelope[12] = 64;
        let mut payload = vec![0u8; 64];
        let mut tag = [0u8; HMAC_TAG_LEN];

        for i in 0..1000u32 {
            envelope[8] = i as u8;
            for (j, p) in payload.iter_mut().enumerate() {
                *p = (i as usize + j) as u8;
            }
            s.sign(&envelope, &payload, &mut tag);
            assert_eq!(v.verify(&envelope, &payload, &tag), VwResult::Ok);
            assert_eq!(verify(&key, &envelope, &payload, &tag), VwResult::Ok);

            tag[0] ^= 0x01;
            assert_eq!(v.verify(&envelope, &payload, &tag), VwResult::ErrTag);
            tag[0] ^= 0x01;
        }
        // State still usable after the stream.
        envelope[8] = 0;
        for (j, p) in payload.iter_mut().enumerate() {
            *p = j as u8;
        }
        s.sign(&envelope, &payload, &mut tag);
        assert_eq!(v.verify(&envelope, &payload, &tag), VwResult::Ok);
    }

    /// V10: batch decode-verify — all-OK, first-bad stop, zero-copy views,
    /// bytes_consumed, truncated-tail policy.
    #[test]
    fn v10_batch() {
        let key = derive_key(b"v10-batch");
        let mut s = VwSigner::new(&key);
        const N: usize = 500;
        const PLEN: usize = 48;
        let rec_len = VW_ENVELOPE_LEN + PLEN + HMAC_TAG_LEN;
        let mut stream = vec![0u8; N * rec_len];
        let mut envelope = [0u8; 16];
        envelope[..4].copy_from_slice(b"WEFT");
        envelope[6] = 16;
        envelope[12] = PLEN as u8;
        let payload = [0x33u8; PLEN];
        let mut tag = [0u8; HMAC_TAG_LEN];

        for i in 0..N {
            let seq = (i * 3 + 1) as u32;
            envelope[8..12].copy_from_slice(&seq.to_le_bytes());
            s.sign(&envelope, &payload, &mut tag);
            let n = record_encode(&envelope, &payload, &tag, &mut stream[i * rec_len..]);
            assert_eq!(n, rec_len);
        }

        let mut views = Vec::new();
        let (r, n, consumed) = batch_decode_verify(&key, &stream, Some(&mut views));
        assert_eq!((r, n, consumed), (VwResult::Ok, N, N * rec_len));
        assert_eq!(views.len(), N);
        for (i, view) in views.iter().enumerate() {
            assert!(unsafe { std::ptr::eq(view.envelope.as_ptr(), stream.as_ptr().add(i * rec_len)) });
            assert_eq!(view.seq, (i * 3 + 1) as u32);
            assert_eq!(view.payload.len(), PLEN);
        }

        // Tamper record 137: batch stops there, prefix good.
        let k = 137;
        stream[k * rec_len + 20] ^= 0x40;
        let (r, n, consumed) = batch_decode_verify(&key, &stream, None);
        assert_eq!(r, VwResult::ErrTag);
        assert_eq!(n, k);
        assert_eq!(consumed, k * rec_len);

        // Truncated tail (< envelope+tag): ignored.
        let (r, n, consumed) = batch_decode_verify(&key, &stream[..k * rec_len + 17], None);
        assert_eq!((r, n, consumed), (VwResult::Ok, k, k * rec_len));
    }
}
