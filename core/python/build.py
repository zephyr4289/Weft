"""cffi build for the Weft C core (issue #18-6: Python bindings).

API-mode binding: the C sources are compiled into a native extension at
install time (gcc/clang required — the same core/c sources every other
native binding uses, byte-for-byte). The cdef declares FUNCTIONS ONLY —
no struct layouts cross the boundary; every object crosses as an opaque
handle allocated/freed by one C call each (the FFI-finalizer discipline,
see weft_pyshim.c). cffi releases the GIL around these calls, so the
torture tests run real concurrent Python threads.
"""
from cffi import FFI
import os

HERE = os.path.abspath(os.path.dirname(__file__))
CORE = os.path.join(HERE, "..", "c")

ffibuilder = FFI()

ffibuilder.cdef(
    r"""
    // ---- kernel (weft.h) ----
    typedef struct weft weft_t;
    weft_t* weft_py_new(size_t payload_max);
    void    weft_py_free(weft_t* w);
    int     weft_w_write_payload(weft_t* w, const uint8_t* src, size_t len);
    int     weft_publish(weft_t* w, uint32_t seq, uint32_t payload_len);
    uint32_t weft_r_claim(weft_t* w);
    uint32_t weft_r_seq(weft_t* w);
    uint32_t weft_r_magic(weft_t* w);
    uint16_t weft_r_header_size(weft_t* w);
    uint32_t weft_r_payload_len(weft_t* w);
    size_t   weft_r_read_slice(weft_t* w, uint8_t* dst, size_t offset, size_t dst_len);
    const uint8_t* weft_r_live_ptr(weft_t* w, size_t offset);
    void     weft_revoke(weft_t* w);
    int      weft_reclaim(weft_t* w, uint32_t pre_revoke_epoch, uint32_t timeout_ms);
    uint32_t weft_epoch(weft_t* w);
    int      weft_revoked(weft_t* w);
    uint64_t weft_t_publish(weft_t* w);
    uint64_t weft_t_claim(weft_t* w);
    uint64_t weft_t_drop(weft_t* w);
    uint64_t weft_t_wsteps(weft_t* w);
    uint64_t weft_t_rsteps(weft_t* w);
    uint32_t weft_mix32(uint32_t x);

    // ---- fan-out ----
    typedef struct weft_fanout weft_fanout_t;
    typedef struct weft_fanout_reader weft_fanout_reader_t;
    weft_fanout_t* weft_fanout_new(size_t payload_bytes, unsigned slot_count);
    void weft_fanout_free(weft_fanout_t* f);
    int  weft_fanout_attach_writer(weft_fanout_t* f, void* ring, size_t ring_bytes,
                                   size_t payload_bytes, unsigned slot_count);
    uint8_t* weft_fanout_begin(weft_fanout_t* f);
    int  weft_fanout_fill(weft_fanout_t* f, const void* src, size_t len);
    uint64_t weft_fanout_publish(weft_fanout_t* f);
    void weft_fanout_destroy(weft_fanout_t* f);
    size_t weft_fanout_ring_bytes(size_t payload_bytes, unsigned slot_count);
    const void* weft_fanout_ring(const void* f);
    weft_fanout_reader_t* weft_fanout_reader_new(const void* ring, size_t ring_bytes,
                                                 size_t payload_bytes, unsigned slot_count);
    void weft_fanout_reader_free(weft_fanout_reader_t* r);
    void weft_fanout_reader_destroy(weft_fanout_reader_t* r);
    typedef struct { const void* src; size_t len; } weft_batch_frame_t;
    uint64_t weft_publish_batch(weft_fanout_t* f, const weft_batch_frame_t* frames, size_t n);
    const char* weft_fanout_copy_active_impl(void);
    unsigned weft_fanout_slot_line_align(void);

    // ---- claim record (simple struct — no atomics, cffi-safe) ----
    typedef struct { bool fresh; uint64_t seq; uint64_t dropped; } weft_fanout_claim_t;
    const weft_fanout_claim_t* weft_fanout_claim(weft_fanout_reader_t* r);
    const void* weft_fanout_view(const weft_fanout_reader_t* r);
"""
)

FRAME_HELPER = r"""
// issue #18-6 helper: build a batch frame array from flat pointer/len lists
// (avoids exposing struct layouts across the cdef boundary).
typedef struct { const void* src; size_t len; } weft_py_batch_frame_t;

uint64_t weft_py_publish_batch(weft_fanout_t* f,
                               const void* const* srcs,
                               const size_t* lens,
                               size_t n) {
    if (n == 0 || n > 4096) return 0;
    weft_py_batch_frame_t frames[4096];
    for (size_t i = 0; i < n; i++) {
        frames[i].src = srcs[i];
        frames[i].len = lens[i];
    }
    // weft_batch_frame_t is layout-identical (const void* src; size_t len)
    return weft_publish_batch(f, (const weft_batch_frame_t*)frames, n);
}

// claim/view go through cffi DIRECTLY (weft_fanout_claim_t is a simple
// bool/u64/u64 struct — see the cdef); no shim needed for them.
"""

ffibuilder.cdef(
    """
    uint64_t weft_py_publish_batch(weft_fanout_t* f,
                                   const void* const* srcs,
                                   const size_t* lens,
                                   size_t n);
    """
)

ffibuilder.set_source(
    "weft._weft_c",
    '#include "weft.h"\n#include "fanout.h"\n#include "fanout_simd.h"\n'
    '#include "fanout_batch.h"\n'
    "// weft_pyshim.c prototypes (the shim itself compiles via sources=):\n"
    "weft_t* weft_py_new(size_t payload_max);\n"
    "void    weft_py_free(weft_t* w);\n"
    + FRAME_HELPER,
    sources=[
        os.path.join("..", "c", "weft.c"),
        os.path.join("..", "c", "fanout.c"),
        os.path.join("..", "c", "frame_cursor.c"),
        os.path.join("..", "c", "fanout_simd.c"),
        os.path.join("..", "c", "fanout_batch.c"),
        "weft_pyshim.c",
    ],
    include_dirs=[os.path.join("..", "c")],
    extra_compile_args=["-O2", "-std=c11", "-D_GNU_SOURCE"],
)

if __name__ == "__main__":
    ffibuilder.compile(verbose=True)
