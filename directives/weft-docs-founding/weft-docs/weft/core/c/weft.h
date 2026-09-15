// weft.h — Corrected Triad Protocol kernel (C reference)
//
// Normative: per 02-KERNEL.md and 03-ENVELOPE.md. The single shared atomic
// `latest` is exchanged by writer and reader; ownership is exclusive at every
// instant by construction. No second atomic, no exclusion set, no CAS retry.
//
// Forbidden patterns (02 §2.2 — any one of these in review = reject the PR):
//   - A second atomic participating in buffer-ownership decisions
//   - Candidate-set computation, exclusion sets, "pick a free slot" logic
//   - CAS retry loops in publish/claim (wait-freedom = zero loops)
//   - Returning failure from claim() (a claim always yields a buffer)
//   - Copy-on-read inside claim() (readers must observe the LIVE buffer; A3)
//   - Allocation of any kind in publish/claim
//
// No memory_order_seq_cst anywhere in the kernel (06 §2). The ordering matrix
// in litmus/catalog.yaml is the audit; this file cites it, not restates it.

#ifndef WEFT_H
#define WEFT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Weft publish result codes.
typedef enum {
    WEFT_PUB_OK              = 0,  // publish succeeded
    WEFT_PUB_DROPPED_REVOKED = 1, // writer has been revoked; publish was a no-op
} weft_pub_result_t;

/// Decode result codes (03-ENVELOPE §2).
typedef enum {
    WEFT_DECODE_OK             = 0,
    WEFT_DECODE_SHORT          = 1,  // avail < 16, or payload_len > avail - header_size
    WEFT_DECODE_BAD_MAGIC      = 2,  // magic != "WEFT"
    WEFT_DECODE_BAD_HEADER     = 3,  // header_size < 16 or > avail
} weft_decode_result_t;

/// Weft instance — one writer + one reader, three off-heap buffers, one atomic.
///
/// Field access discipline (02 §1):
///   - `buf[3]`: ownership rules below; never read another party's held buffer.
///   - `latest`: SHARED atomic; exchanged by writer (publish) and reader (claim).
///   - `w_work`: WRITER-PRIVATE; only the writer thread reads or writes.
///   - `r_work`: READER-PRIVATE; only the reader thread reads or writes.
///   - `revoked`, `epoch`: shared; see I6 handshake in 02 §6.
///   - `t_*` counters: shared atomic u64; Relaxed fetch_add (telemetry only).
///
/// Buffer layout per buffer (03-ENVELOPE §1 + 02 §1):
///   [0..16)                  envelope (magic, version, header_size, seq, payload_len)
///   [16..16+payload_max)     payload
///   [buf_size-8..buf_size)   canary (u64 LE; value = seq)
typedef struct weft {
    uint8_t* buf[3];                 // 3 buffers, 64-byte aligned
    size_t buf_size;                  // = align64(16 + payload_max + 8, 64)
    size_t payload_max;               // immutable after init

    /// The single shared atomic. Exchanged by writer (publish) and reader (claim).
    /// Init: 0. Memory order: AcqRel on both exchanges (02 §5).
    _Atomic uint32_t latest;

    /// Writer-private working index. Init: 1. Not synchronized (thread-private).
    uint32_t w_work;

    /// Reader-private held index. Init: 2. Not synchronized (thread-private).
    uint32_t r_work;

    /// I6: writer revocation flag. Init: false.
    /// Writer: Relaxed load (advisory). Releaser: Release store. (02 §5)
    _Atomic bool revoked;

    /// I6: writer epoch. Init: 0. Increments on each ACK (post-revocation publish).
    /// Writer: fetch_add AcqRel on ACK. Releaser: Acquire poll. (02 §5, 02 §6)
    _Atomic uint32_t epoch;

    // Telemetry (Relaxed; never synchronization)
    _Atomic uint64_t t_publish;       // incremented after each successful publish
    _Atomic uint64_t t_claim;         // incremented after each claim
    _Atomic uint64_t t_drop;          // incremented on each DROPPED_REVOKED
    _Atomic uint64_t t_wsteps;       // incremented once per protocol RMW in w_publish (L2)
    _Atomic uint64_t t_rsteps;       // incremented once per protocol RMW in r_claim (L3)
} weft_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// Allocate a Weft with the given payload_max. Returns 0 on success, -1 on
/// alloc failure. Allocates 3 buffers via posix_memalign(64). May allocate
/// (only `init` may; Law 2 — zero is a contract in publish/claim).
int weft_init(weft_t* w, size_t payload_max);

/// Destroy a Weft. Idempotent. Frees nothing that reclaim already released (§6).
/// If `revoke` was never called, plain free is correct (no writer exists).
/// `destroy` without a prior `reclaim` while a writer may still run is a CALLER
/// ERROR — document, don't defend.
void weft_destroy(weft_t* w);

// ---------------------------------------------------------------------------
// Writer (called from the writer thread only)
// ---------------------------------------------------------------------------

/// Get a write cursor for the writer's working buffer. The cursor is a raw
/// pointer to buf[w_work][16..16+payload_max). The writer fills it, then calls
/// `weft_publish`. NOT a snapshot — the live buffer; writes here are visible
/// after the next publish.
uint8_t* weft_w_begin(weft_t* w);

/// Convenience: write `len` bytes of payload from `src` into the writer's
/// working buffer at offset 16. Returns 0 on success, -1 if len > payload_max.
int weft_w_write_payload(weft_t* w, const uint8_t* src, size_t len);

/// Publish the writer's working buffer with the given seq and payload_len.
/// Per 02 §2 + §6:
///   1. if revoked.load(Relaxed): epoch.fetch_add(1, AcqRel); t_drop++;
///      return DROPPED_REVOKED  (checked FIRST, before any byte write)
///   2. write envelope (v1, seq, payload_len) into buf[w_work]
///   3. write canary = seq at buf[w_work].tail
///   4. old = latest.exchange(w_work, AcqRel)   // THE atomic
///   5. w_work = old
///   6. t_publish++; t_wsteps++; return PUB_OK
weft_pub_result_t weft_publish(weft_t* w, uint32_t seq, uint32_t payload_len);

// ---------------------------------------------------------------------------
// Reader (called from the reader thread only)
// ---------------------------------------------------------------------------

/// Claim the freshest published buffer. Per 02 §2:
///   mine = latest.exchange(r_work, AcqRel)   // THE atomic
///   r_work = mine
///   t_claim++; t_rsteps++
///   return mine
/// NEVER fails. Before any publish, returns the null frame (seq=0).
uint32_t weft_r_claim(weft_t* w);

/// Read envelope fields of the reader's held buffer (live, not a snapshot).
/// Each function reads the live buffer at call time. Per 02 §4 + 03-ENVELOPE.
uint32_t weft_r_seq(weft_t* w);
uint16_t weft_r_header_size(weft_t* w);
uint32_t weft_r_magic(weft_t* w);
uint32_t weft_r_payload_len(weft_t* w);

/// Copy LIVE held-buffer bytes at call time. Per A3: the reader must observe
/// the live buffer, never a snapshot taken at claim time. Copies `dst_len`
/// bytes starting at `offset` from buf[r_work] into `dst`. Returns bytes copied
/// (capped at buf_size - offset).
size_t weft_r_read_slice(weft_t* w, uint8_t* dst, size_t offset, size_t dst_len);

/// Direct pointer to the reader's held buffer (live). For verify-in-place (L1).
/// Returns buf[r_work] + offset, or NULL if offset >= buf_size.
const uint8_t* weft_r_live_ptr(weft_t* w, size_t offset);

// ---------------------------------------------------------------------------
// I6 — writer revocation handshake (02 §6, A1)
// ---------------------------------------------------------------------------

/// Step 1: revoke the writer. Sets revoked.store(true, Release). The writer
/// will observe it on its next publish and ACK via epoch.fetch_add(AcqRel).
void weft_revoke(weft_t* w);

/// Steps 2-3: poll epoch (Acquire) until it advances past the pre-revoke value,
/// bounded by timeout_ms. Returns 0 on ACK received, -1 on timeout.
///
/// After ACK is observed, the caller may poison (memset 0xDE) or free.
/// Poison-before-ACK is the bug this handshake prevents (A1).
int weft_reclaim(weft_t* w, uint32_t pre_revoke_epoch, uint32_t timeout_ms);

// ---------------------------------------------------------------------------
// Telemetry (Relaxed loads; statistics only, never synchronization)
// ---------------------------------------------------------------------------

uint64_t weft_t_publish(weft_t* w);
uint64_t weft_t_claim(weft_t* w);
uint64_t weft_t_drop(weft_t* w);
uint64_t weft_t_wsteps(weft_t* w);
uint64_t weft_t_rsteps(weft_t* w);
uint32_t weft_epoch(weft_t* w);
bool weft_revoked(weft_t* w);

// ---------------------------------------------------------------------------
// Envelope pure functions (03-ENVELOPE §1, §2) — no threads, no kernel state
// ---------------------------------------------------------------------------

/// Encode a triad-1 envelope into dst (must be >= 16 bytes).
/// Writes: magic="WEFT", version=1, header_size=16, seq, payload_len.
void weft_envelope_encode_v1(uint8_t* dst, uint32_t seq, uint32_t payload_len);

/// Encode a custom-version envelope (for L8b: header_size can be > 16).
/// Writes: magic="WEFT", version, header_size, seq, payload_len, then `extras`
/// bytes (filled with 0xAA by convention; the decoder ignores them).
void weft_envelope_encode(uint8_t* dst, uint16_t version, uint16_t header_size,
                          uint32_t seq, uint32_t payload_len);

/// Decode an envelope. Per 03-ENVELOPE §2:
///   1. avail >= 16 else DECODE_SHORT
///   2. magic == "WEFT" else DECODE_BAD_MAGIC
///   3. read version, header_size
///   4. header_size >= 16 and <= avail else DECODE_BAD_HEADER
///   5. payload begins at header_size (NEVER 16)
///   6. payload_len <= avail - header_size else DECODE_SHORT
///   7. unknown trailing fields (between 16 and header_size) are skipped
weft_decode_result_t weft_envelope_decode(const uint8_t* buf, size_t avail,
                                          uint16_t* version, uint16_t* header_size,
                                          uint32_t* seq, uint32_t* payload_len);

/// Bind-time version negotiation (03-ENVELOPE §3).
///   chosen = max({ v ∈ S : v <= W })
///   empty set → 0 (BIND_INCOMPATIBLE)
uint16_t weft_negotiate(uint16_t writer_version, const uint16_t* reader_versions,
                        size_t reader_count);

// ---------------------------------------------------------------------------
// Shared payload pattern (04-LITMUS §0.1)
// ---------------------------------------------------------------------------

/// mix32(x) — per 04-LITMUS §0.1. All ops mod 2^32.
uint32_t weft_mix32(uint32_t x);

/// pat(seq, i) — per 04-LITMUS §0.1. Deterministic payload byte.
uint8_t weft_pat(uint32_t seq, uint32_t i);

/// xorshift32 step — per 04-LITMUS §0.2. Marsaglia 13/17/5.
uint32_t weft_xorshift32(uint32_t* state);

// ---------------------------------------------------------------------------
// Debug view (WO-P2-TOOLS T1) — read-only, wait-free, allocation-free
// inspection accessor. The ONE permitted kernel API addition for Phase 2.
// Semver: minor bump.
// ---------------------------------------------------------------------------

/// Per-buffer debug info for the two LIVE buffers.
typedef struct {
    uint32_t slot_idx;        ///< Buffer index (0..2)
    uint32_t seq;             ///< Envelope seq (LE)
    uint16_t version;         ///< Envelope version (LE)
    uint16_t header_size;     ///< Envelope header_size (LE)
    uint32_t payload_len;      ///< Envelope payload_len (LE)
    /// Owner: 0=free, 1=writer, 2=reader, 3=in-exchange(latest)
    uint8_t owner;
} weft_debug_buf_t;

/// Debug view — a set of individually-consistent samples, NOT a consistent
/// snapshot. Per AXIOM T (05-CONTRACTS v1.3), telemetry values are advisory.
/// w_work/r_work are thread-private; reading them from another thread is
/// advisory-only sampling. The doc comment must say so (WO-P2-TOOLS T1).
typedef struct {
    uint32_t latest;           ///< Atomic load (Relaxed — advisory per AXIOM T)
    uint32_t w_work;           ///< Writer-private (advisory; thread-private by contract)
    uint32_t r_work;           ///< Reader-private (advisory; thread-private by contract)
    uint8_t  revoked;           ///< Revoked flag
    uint32_t epoch;            ///< Epoch (Acquire load)
    uint64_t t_publish;        ///< Telemetry (advisory)
    uint64_t t_claim;           ///< Telemetry (advisory)
    uint64_t t_drop;            ///< Telemetry (advisory)
    /// Two live buffer samples. Buffers with no live owner are reported by
    /// index/state only — NO dereference (I6 rule). The third buffer (if
    /// freed/poisoned) is reported as slot_idx=3, owner=0, all fields zero.
    weft_debug_buf_t bufs[2];
    /// Flag: a header sampled mid-publish may be internally inconsistent.
    /// The view reports mid_publish_sample=true with the raw bytes.
    bool mid_publish_sample;
} weft_debug_view_t;

/// Read-only, wait-free, allocation-free inspection. Fills `out` with the
/// current kernel state. Never dereferences freed/poisoned buffers (I6).
/// Per AXIOM T: telemetry values in the view are advisory.
void weft_debug_view(const weft_t* w, weft_debug_view_t* out);

#endif // WEFT_H
