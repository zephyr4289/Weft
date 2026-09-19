// trace_rec.h — RFC 0014: .weftrec v4 kernel trace events, C reference codec.
//
// WHY EXISTS: issue #20 (Tier 5: Observability) task 1 — a standardized
// trace format so a capture from ANY port replays identically anywhere.
// The capture formats (v1 kernel claims, v2 fanout claims, v3 dzv
// compression) record PAYLOADS; v4 records EVENTS — the decision stream of
// a kernel run: publishes, claims, drops, revocations, ACKs, stalls, tears.
//
// VERSIONING: a new format version (4), not an in-place edit of v1-v3 —
// per the §1.3 rule, older tooling MUST reject v4 (version gate) and v4
// tooling rejects v1/v2/v3.
//
// HEADER (32 bytes) — same layout as v1 (§1.1), these fields differ:
//   offset 4   format_version   4
//   offset 8   flags            bit 2 = TRACE (0x4); rest reserved, 0
//   offset 12  envelope_version 0 (v4 carries no payload frames)
//   offset 16  frame_count      event_count (name kept for layout stability)
//   offset 20  crc32            CRC-32/zlib over bytes 0..20
//
// EVENT RECORD (12 bytes, fixed width — declared by the version):
//   offset 0   u16 kind   (see weft_trace_kind)
//   offset 2   u16 aux    (kind-scoped: publish = payload_len; else 0)
//   offset 4   u32 data   (kind-scoped: seq / epoch / attempts)
//   offset 8   u32 crc32  CRC-32/zlib over bytes 0..8 of the record
// Fixed width because trace events are fixed-shape by definition; forward
// extensibility = a new format version (the same trade v1 made for the
// 16-byte envelope, now made explicitly for events).
//
// PARITY SURFACE: the packed event stream (records WITHOUT the per-record
// CRC) is the cross-port byte-identity surface — every port emits the same
// stream for the same deterministic scenario (fixtures/xlang-trace/). The
// container (header + CRC'd records) is produced by THIS codec; ports
// without a filesystem produce the stream and hand it to the reference
// encoder. No wall-clock anywhere: the stream is a pure function of the
// scenario, which is what makes byte-identity possible (timestamps live in
// the container's sidecar or the replay tool, never in the event stream).

#ifndef WEFT_TRACE_REC_H
#define WEFT_TRACE_REC_H

#include <stddef.h>
#include <stdint.h>

#define WEFT_TRACE_MAGIC      0x43455257u  // "WREC" LE
#define WEFT_TRACE_VERSION    4u
#define WEFT_TRACE_FLAG       0x4u         // flags bit 2
#define WEFT_TRACE_HDR_SIZE   32u
#define WEFT_TRACE_REC_SIZE   12u

/// Event kinds (u16). Kind order in the packed stream is the kernel's
/// own op order — no reordering, no buffering (the trace is the run).
typedef enum {
    WEFT_TRACE_PUBLISH     = 1,  // aux = payload_len, data = seq
    WEFT_TRACE_CLAIM       = 2,  // aux = 0,            data = seq claimed
    WEFT_TRACE_DROP        = 3,  // aux = epoch at ACK, data = seq refused
    WEFT_TRACE_REVOKE      = 4,  // aux = 0,            data = pre-revoke epoch
    WEFT_TRACE_ACK         = 5,  // aux = 0,            data = epoch after ACK
    WEFT_TRACE_STALL       = 6,  // aux = 0,            data = attempts (consumer skip)
    WEFT_TRACE_TEAR        = 7,  // aux = 0,            data = seq of torn frame
    WEFT_TRACE_CANARY_FAIL = 8,  // aux = 0,            data = seq of bad frame
} weft_trace_kind;

/// CRC-32/zlib (reflected 0xEDB88320, init 0xFFFFFFFF, final xor) — the
/// §1.4 contract. Exported so ports and tools share one implementation.
uint32_t weft_trace_crc32(const uint8_t* data, size_t len);

/// One event, packed (8 bytes, no CRC — the parity surface).
typedef struct {
    uint16_t kind;
    uint16_t aux;
    uint32_t data;
} weft_trace_event;

void weft_trace_event_pack(const weft_trace_event* e, uint8_t out[8]);
int  weft_trace_event_unpack(const uint8_t in[8], weft_trace_event* e);  // -1 if kind invalid

/// Streaming writer. `weft_trace_writer_open` writes the 32-byte header
/// (with placeholder count), `weft_trace_writer_event` appends CRC'd
/// records, `weft_trace_writer_close` patches the count and the header
/// CRC into `buf`. `cap` must hold 32 + 12*max_events bytes.
typedef struct {
    uint8_t*  buf;
    size_t    cap;
    size_t    off;
    uint32_t  count;
    int       overflow;
} weft_trace_writer;

size_t weft_trace_writer_needed(size_t max_events);   // 32 + 12*max_events
void   weft_trace_writer_open(weft_trace_writer* w, uint8_t* buf, size_t cap);
int    weft_trace_writer_event(weft_trace_writer* w, const weft_trace_event* e); // 0 / -1 overflow
int    weft_trace_writer_close(weft_trace_writer* w);  // patches header; 0 / -1

/// Validation + decoding. Returns 0 on a well-formed v4 trace; -1 on any
/// structural violation (magic/version/flags/size/CRC mismatch — version
/// gate included: v1/v2/v3 files are REFUSED here by design).
int weft_trace_validate(const uint8_t* buf, size_t len, uint32_t* event_count);

/// Iterate events from a validated buffer. `*cursor` starts at 32.
int weft_trace_next(const uint8_t* buf, size_t len, size_t* cursor,
                    weft_trace_event* e);  // 1 = event, 0 = end, -1 = CRC/structure

/// JSON-lines exporter (the human-readable contract; see
/// schemas/weftrec-trace.schema.json). One object per line, plus a header
/// line. Returns bytes written to `out` (bounded by cap).
size_t weft_trace_to_json(const uint8_t* buf, size_t len,
                          uint8_t* out, size_t cap);

#endif // WEFT_TRACE_REC_H
