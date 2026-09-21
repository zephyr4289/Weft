// v4l2_mock_device.h — synthetic V4L2/DMA-BUF mock camera (tests/adapters).
//
// WHY EXISTS: mandate C of the directive — CI has no camera silicon, so
// the vision battery runs against a device model that reproduces the parts
// of V4L2 that matter for zero-copy ingestion semantics: per-buffer DMA
// memory (memfd arenas, one per buffer — the fd-passable stand-in for
// dma-buf), an asynchronous sensor writer thread on an absolute-pace
// clock (4K @ 120 FPS default), DONE/QUEUED buffer state transitions, and
// deterministic per-frame stamps (header + swath + trailer with CRC) so
// the battery can PROVE integrity and drop accounting, not just liveness.
//
// Honesty boundary (documented in D-62 §B.4): the mock models a memory
// -mapped DMA camera (V4L2_MEMORY_MMAP semantics with per-buffer
// exportable memory). Real VIDIOC ioctl negotiation is exercised by the
// v4l2 backend's own path; here we exercise the ENGINE, the buffer
// lifecycle, and the zero-copy handoff under controlled timing.

#ifndef WEFT_TESTS__V4L2_MOCK_DEVICE_H_
#define WEFT_TESTS__V4L2_MOCK_DEVICE_H_

#include "weft_vision_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The mock backend ops table (links into test/bench binaries only).
const weft_vision_backend_ops_t *weft_vision_mock_backend(void);

/// Frame stamp layout (first 64 bytes of every mock frame).
#define WEFT_MOCK_STAMP_MAGIC 0x4d565746u /* "WFVM" little-endian */

typedef struct weft_mock_stamp {
    uint32_t magic;
    uint32_t _pad;
    uint64_t frame_no;
    uint64_t ts_unix_ns;
    uint32_t swath_off;
    uint32_t swath_len;
    uint64_t swath_hash; /* rolling 64-bit hash over the swath */
} weft_mock_stamp_t;

_Static_assert(sizeof(weft_mock_stamp_t) == 40,
               "mock stamp: 40-byte header, 24 bytes slack to 64");

/// Per-4KiB-block stamp written into the swath (integrity granularity).
typedef struct weft_mock_block_stamp {
    uint32_t magic;
    uint32_t block_idx;
    uint64_t frame_no;
} weft_mock_block_stamp_t;

/// O(1) liveness check: stamp magic, frame_no, trailer magic/frame_no and
/// the stamp-vs-trailer CRC agreement (no swath scan). Returns 0 = sound.
int weft_mock_frame_check_header(const uint8_t *buf, size_t len,
                                 uint64_t frame_no);

/// Verify a mock frame end-to-end: stamp magic, frame_no, trailer magic,
/// CRC over the stamped swath. Returns 0 = intact, negative = corrupted.
int weft_mock_frame_verify(const uint8_t *buf, size_t len, uint64_t frame_no);

#ifdef __cplusplus
}
#endif

#endif  // WEFT_TESTS__V4L2_MOCK_DEVICE_H_
