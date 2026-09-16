// fanout.h — RFC 0004: Multi-Consumer Fan-Out ring, C driver layer
//
// WHY EXISTS: RFC 0004 (Accepted as driver-layer pattern, round-6 §4). The
// Triad kernel is 1-writer/1-reader by design; applications that need a
// primary canvas, a minimap, a flight recorder, and a network visualizer on
// one stream cannot bind N readers to one Triad. The TS port ships the ring
// over a SharedArrayBuffer (`core/ts/fanout.ts`); the canonical C and Rust
// kernels had no equivalent — native surfaces (Android JNI, Flutter FFI, C++
// engines, Rust audio graphs) could not fan out at all. This module brings
// the ring to C with a BYTE-COMPATIBLE layout, so a ring produced by any
// port is consumable by any other (see fixtures/xlang-fanout/).
//
// RING LAYOUT (byte-identical to core/ts/fanout.ts — the interop contract):
//   byte 0              latestSeq   _Atomic u64   0 = no frame yet; frames from 1
//   byte 8              publishes   _Atomic u64   telemetry (one add per publish)
//   byte 16 + 8k        slotSeq[k]  _Atomic u64   0 = INVALIDATED (fill in progress)
//   byte 16 + 8M        payload     M slots x payload_bytes (slot k at +k*payload_bytes)
// payload_bytes MUST be a multiple of 4 (u32 word granularity — the same
// constraint as the TS port's payloadFloats). ring_bytes = 16 + 8M + M*payload_bytes.
// Cross-language sessions keep ctrl values < 2^53 (the TS port surfaces seq
// as Number) — declared, not assumed, mirroring the TS boundary note.
//
// PROTOCOL (RFC 0004 §Reference-level specification — same as the TS port):
//   Writer (single, by contract — the same contract as the kernel's writer):
//     begin():   wSeq += 1; k = (wSeq-1) mod M;
//                slotSeq[k] <- 0  (invalidate BEFORE the fill — the FI1 bracket)
//                return slot cursor
//     publish(): slotSeq[k] <- wSeq; latestSeq <- wSeq; publishes += 1
//   Reader (N, independent; each owns its pre-allocated copy buffer — Law 2):
//     claim():   L = latestSeq; if L == 0 or L == lastSeq: not fresh
//                else bounded (<= 4 attempts):
//                  k = (L-1) mod M; sB = slotSeq[k]
//                  if sB != L: re-read latestSeq; unchanged -> SKIP this tick
//                    (Law 1: no spin; counted, never silent); changed -> chase
//                  copy slot k -> reader buffer; sA = slotSeq[k]
//                  if sA == L: consistent frame L; dropped = L - lastSeq - 1;
//                    advance lastSeq (FI2: per-slot stamp monotonicity proves
//                    an unchanged stamp means no overwrite began during copy)
//                  else: torn copy; retry on the newest completed frame
//                attempts exhausted: not fresh, counted, never a spin
//
// MEMORY ORDERING — the C port does better than "all seq_cst" (the TS port
// is seqcst-only because JS Atomics give no choice; the kernel constitution
// bans seq_cst in the KERNEL, 06 §2 — this is the driver layer, but the same
// taste applies: pay only for the ordering the proof needs).
//
//   Default regime ("fenced acq/rel"), one fence per side per frame:
//     Writer  begin:    slotSeq[k] <- 0 as SeqCst store, then a SeqCst fence
//                      BEFORE the fill cursor is returned. Property P1: no
//                      payload word of the new fill may become visible to a
//                      reader before the invalidate stamp does (otherwise a
//                      reader could accept a torn frame whose stamp still
//                      validates). Release alone cannot express this — a
//                      release store orders PRIOR accesses, not subsequent
//                      ones; the SeqCst store + fence pair does, on every
//                      implementation we target (x86-TSO: stores retire in
//                      order; AArch64: release + DMB ISH propagation order).
//     Writer  publish: slotSeq[k] and latestSeq as Release stores. The fill
//                      (plain or relaxed-atomic) is sequenced before the
//                      stamp; a release stamp orders prior writes below it —
//                      exactly the kernel's envelope->canary->exchange stance.
//     Reader  claim:   latestSeq/slotSeq as Acquire loads (pairing with the
//                      writer's release publication point gives the happy
//                      path its happens-before: the frame-L payload is
//                      visible to the copy). One SeqCst fence between the
//                      copy and the revalidation load. Property P2: if the
//                      copy observed any word of an overwrite, the
//                      revalidation load must observe the invalidate-or-newer
//                      stamp. Acquire alone cannot express this (it stops
//                      LATER ops hoisting above it, not the copy sinking
//                      below the check); the fence does.
//     Payload words:   Relaxed atomic u32 accesses on BOTH sides (fill and
//                      copy). This keeps the seqlock discipline free of data
//                      races in the strict C11 sense (a plain payload access
//                      concurrent with the opposing side's access is a race
//                      by definition, whatever the brackets) — at zero cost
//                      on x86 (a relaxed load/store is a plain mov) and at
//                      the cost of nothing TSAN can flag. The raw cursor from
//                      begin() is still available for production float writes
//                      under the bracket discipline (the TS port's stance);
//                      all in-tree tests and the TSAN builds use
//                      weft_fanout_fill()/claim(), which stay race-free.
//   A/B regime: compile with -DWEFT_FANOUT_SEQ_CST=1 for the TS-equivalent
//   all-seq_cst stamps (fences retained — subsumed). Torture gates run under
//   BOTH regimes; the cheaper default ships only with that evidence.
//
// LAW 2: begin/fill/publish/claim allocate nothing (init/attach may).
// LAW 1: every path is bounded; a skip or exhausted retry is counted in
//        reader stats, never silent, never a spin.
// LAW 4: honest boundaries — dropped = L - lastSeq - 1 in u64 arithmetic
//        assumes the single-writer monotonic contract (a corrupt or
//        non-monotonic ring is outside the contract and underflows, exactly
//        as the TS port produces a negative Number); multi-canvas renders
//        are approximately synchronized (latest-wins per consumer).

#ifndef WEFT_FANOUT_H
#define WEFT_FANOUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/// Maximum ring depth. RFC 0004 recommends 4-8; the bound keeps the cached
/// per-slot view arrays inside the reader struct (one line each, no heap).
#define WEFT_FANOUT_MAX_SLOTS 64u

/// Bounded claim attempts (same constant and rationale as the TS port: a
/// retry only happens when a NEWER frame completed during the claim, and the
/// newer frame's own slot is self-consistent, so convergence is immediate).
#define WEFT_FANOUT_MAX_CLAIM_ATTEMPTS 4

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Claim result record — reader-owned and identity-stable, mutated in place
/// per claim so the hot path allocates nothing (Law 2). Read the fields
/// synchronously after weft_fanout_claim(); do not retain the record across
/// claims expecting a snapshot.
typedef struct {
    bool fresh;         ///< A new consistent frame was claimed this tick
    uint64_t seq;       ///< Frame seq now held (last consistent if !fresh)
    uint64_t dropped;   ///< Frames completed without this reader ever observing them
} weft_fanout_claim_t;

/// Advisory reader statistics (AXIOM T: advisory, never a correctness
/// reference). Field names mirror the TS port's FanoutReaderStats.
typedef struct {
    uint64_t reads;                 ///< Total claim() calls
    uint64_t fresh;                 ///< Claims that returned a fresh frame
    uint64_t drops;                 ///< Sum of `dropped` across fresh claims
    uint64_t skipped_mid_overwrite; ///< Ticks skipped: target slot mid-overwrite, no newer frame
    uint64_t torn_exhausted;        ///< Claims that exhausted the bounded retry
} weft_fanout_stats_t;

/// Advisory broadcaster state (cold path; AXIOM T applies).
typedef struct {
    uint64_t latest_seq;                        ///< Atomic load of latestSeq
    uint64_t publishes;                         ///< Atomic load of publishes
    unsigned slot_count;                        ///< Ring depth M
    size_t payload_bytes;                       ///< Per-slot payload capacity
    uint64_t slot_stamps[WEFT_FANOUT_MAX_SLOTS];///< Per-slot stamps (0 = invalidated)
} weft_fanout_debug_t;

/// The fan-out ring: one writer + N readers, M pre-allocated slots, one
/// publication point (latestSeq). Field discipline mirrors the kernel:
///   - ring/ctrl: shared; synchronized ONLY by the stamp protocol above.
///   - w_seq/w_slot: WRITER-PRIVATE (single writer by contract).
///   - owns_ring: set by init, cleared by attach; only destroy consults it.
typedef struct weft_fanout {
    uint8_t* ring;             ///< Single allocation (or foreign memory if attached)
    _Atomic uint64_t* ctrl;    ///< ring + 0: [latestSeq, publishes, slotSeq[0..M)]
    size_t payload_bytes;      ///< Immutable after init; multiple of 4
    unsigned slot_count;       ///< Immutable after init; [2, WEFT_FANOUT_MAX_SLOTS]
    uint64_t w_seq;            ///< Writer-private frame counter (0 = no begin yet)
    unsigned w_slot;           ///< Writer-private slot index of the current begin()
    uint8_t* w_cursor;         ///< Cached payload cursor of the current begin()
    int owns_ring;             ///< 1 = init allocated it (destroy frees); 0 = attached
} weft_fanout_t;

/// A reader bound to a ring (any ring — same process, or foreign memory
/// produced by another port). Each reader owns its pre-allocated copy buffer
/// and its claim record; nothing is shared between readers.
typedef struct weft_fanout_reader {
    uint8_t* ring;             ///< The ring base (borrowed; caller keeps it alive)
    _Atomic uint64_t* ctrl;    ///< ring + 0
    size_t payload_bytes;      ///< Immutable after init; multiple of 4
    unsigned slot_count;       ///< Immutable after init
    uint32_t* target;          ///< Pre-allocated copy buffer (payload_bytes/4 words)
    const _Atomic uint32_t* slot_words[WEFT_FANOUT_MAX_SLOTS]; ///< Cached slot views
    uint64_t last_seq;         ///< Last frame seq held consistent (0 = none yet)
    weft_fanout_claim_t rec;   ///< Identity-stable claim record (mutated per claim)
    uint64_t n_reads, n_fresh, n_drops, n_skip, n_exhausted; ///< Reader-private stats
} weft_fanout_reader_t;

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

/// Total ring size in bytes for the given geometry (the interop contract —
/// identical to core/ts/fanout.ts: 16 + 8*slot_count + slot_count*payload_bytes).
size_t weft_fanout_ring_bytes(size_t payload_bytes, unsigned slot_count);

// ---------------------------------------------------------------------------
// Broadcaster (the writer side) — called from the writer thread only
// ---------------------------------------------------------------------------

/// Allocate a fan-out ring (posix_memalign(64); may allocate — Law 2 applies
/// to begin/fill/publish/claim, not init). Zero-initialized ctrl: latestSeq=0
/// (no frame yet), publishes=0, every slotSeq=0 (all invalidated) — the same
/// invariants a fresh SAB gives the TS port. Returns 0 on success, -1 on bad
/// geometry (payload_bytes==0 or %4!=0; slot_count outside [2,64]) or alloc
/// failure.
int weft_fanout_init(weft_fanout_t* f, size_t payload_bytes, unsigned slot_count);

/// Attach a broadcaster to FOREIGN ring memory (e.g. produced by another
/// port, or shared via FFI). Geometry is validated against ring_bytes.
/// w_seq continues from the ring's latestSeq so per-slot stamp monotonicity
/// survives producer handoff. The caller owns the memory; destroy frees
/// nothing (but see the RFC 0004 open question on multi-reader lifecycle —
/// the I6 handshake governs only the kernel path, not this ring).
/// SINGLE WRITER BY CONTRACT: attaching a second broadcaster to a live ring
/// is a caller error — document, don't defend.
int weft_fanout_attach_writer(weft_fanout_t* f, void* ring, size_t ring_bytes,
                              size_t payload_bytes, unsigned slot_count);

/// Begin the next frame: bumps the frame counter, INVALIDATES the target
/// slot's stamp (SeqCst store + SeqCst fence — property P1 above) and
/// returns the slot's payload cursor. The cursor stays valid until the next
/// begin(). Writes through it are visible to readers only after publish().
/// Zero allocation. Not a snapshot — the live slot.
uint8_t* weft_fanout_begin(weft_fanout_t* f);

/// Fill the begun slot from src via relaxed atomic u32 word stores (len must
/// be a multiple of 4 and <= payload_bytes). This is the race-free fill path
/// (strict C11 + TSAN-clean); the raw cursor from begin() is the plain-store
/// production path under the bracket discipline. Returns words written, or
/// -1 on bad len / no begin(). Zero allocation.
int weft_fanout_fill(weft_fanout_t* f, const void* src, size_t len);

/// Publish the begun frame: stamp the slot (Release), flip latestSeq (Release
/// — the publication point), bump publishes (Relaxed telemetry). Returns the
/// published frame seq, or 0 if no begin() ever ran (a detectable no-op, not
/// an error — same as the TS port). Zero allocation.
uint64_t weft_fanout_publish(weft_fanout_t* f);

/// Release the ring. Frees only what init allocated. Idempotent.
void weft_fanout_destroy(weft_fanout_t* f);

/// Advisory state snapshot (cold path; AXIOM T).
void weft_fanout_debug_stats(const weft_fanout_t* f, weft_fanout_debug_t* out);

// ---------------------------------------------------------------------------
// Reader (the consumer side) — N per ring, each fully independent
// ---------------------------------------------------------------------------

/// Attach a reader to a ring (any thread, any port's memory). Geometry is
/// validated against ring_bytes — a mismatched pair fails fast instead of
/// tearing. Allocates the reader's copy buffer and caches slot views (init
/// may allocate; claim never does). Returns 0 on success, -1 on bad geometry.
int weft_fanout_reader_init(weft_fanout_reader_t* r, const void* ring, size_t ring_bytes,
                            size_t payload_bytes, unsigned slot_count);

/// Claim the freshest completed frame into this reader's buffer. Never
/// blocks, never spins unboundedly, never fails: a tick with no consistent
/// newer frame returns fresh=false and the reader keeps its last consistent
/// frame. Returns the reader-owned claim record (identity-stable, mutated in
/// place). `dropped` telescopes exactly: sum(dropped) == lastSeq - freshClaims.
const weft_fanout_claim_t* weft_fanout_claim(weft_fanout_reader_t* r);

/// The reader's pre-allocated copy buffer (payload_bytes bytes, stable
/// identity). Meaningful after a fresh claim; read it live before the next
/// claim — the same discipline as the kernel's r_read_slice (A3).
const void* weft_fanout_view(const weft_fanout_reader_t* r);

/// Advisory statistics snapshot (cold path; AXIOM T).
void weft_fanout_reader_stats(const weft_fanout_reader_t* r, weft_fanout_stats_t* out);

/// Release the reader (frees only the copy buffer). Idempotent.
void weft_fanout_reader_destroy(weft_fanout_reader_t* r);

#endif // WEFT_FANOUT_H
