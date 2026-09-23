# weft_spectrum/arena.py — auto-selecting tensor arena (P5, mandate D).
#
# Backend ladder (transparent down-tier, Law 4 — first available wins, a
# failing backend NEVER throws into the host):
#   Tier 1: CUDA (Engineer 2 driver seam via libcuda)
#   Tier 2: Apple MPS (Apple Silicon)
#   Tier 3: Intel OpenVINO
#   Tier 4: vectorized CPU SIMD (always available)
#
# The arena owns ONE pre-aligned slab (64B/128B per cache-line/SIMD rules).
# Buffers handed out are ZERO-COPY views into that slab (memoryview) with an
# optional DLPack capsule path (structure mirrors Pillar 2/3 precedent) so
# torch.from_dlpack(buffer) works verbatim when torch is present — without
# torch ever being a required dependency.

import ctypes
import struct

from .alignment import aligned_slab, required_alignment
from .detector import detect_cuda, detect_apple_mps, detect_openvino
from .wire import E_PROBE_UNAVAILABLE, E_DEVICE_LOST

_BACKEND_LADDER = ("cuda", "mps", "openvino", "cpu_simd")

# DLPack device enums (dlpack.h)
_DLPACK_CPU = 1
_DLPACK_CUDA = 2


class ArenaBuffer:
    """Zero-copy window into an ArenaSlab. Exposes memoryview + DLPack."""

    __slots__ = ("slab", "view", "byteOffset", "nbytes", "shape", "dtype",
                 "_capsule")

    def __init__(self, slab, view, byte_offset, nbytes):
        self.slab = slab            # keeps the base allocation alive
        self.view = view            # memoryview — zero-copy
        self.byteOffset = byte_offset
        self.nbytes = nbytes
        self.shape = (nbytes,)
        self.dtype = "uint8"
        self._capsule = None

    def __dlpack__(self, stream=None):
        return _make_dlpack_capsule(self)

    def __dlpack_device__(self):
        if self.slab.backend == "cuda":
            return (_DLPACK_CUDA, 0)
        return (_DLPACK_CPU, 0)

    def release(self):
        """Return the window to the arena (no deallocation — slab is pinned)."""
        self.slab._release(self)


class ArenaSlab:
    """One cache-line-aligned arena. Zero-copy buffer windows, reuse-first."""

    def __init__(self, nbytes, backend="cpu_simd", cache_line_bytes=64,
                 simd_width_bits=128):
        self.backend = backend
        self.align = required_alignment(cache_line_bytes, simd_width_bits)
        self.base, self.base_view = aligned_slab(nbytes, self.align)
        self.base_addr = ctypes.addressof(ctypes.c_char.from_buffer(self.base))
        # The ALIGNED data region starts at _align_off inside the raw base;
        # every window handed out lives inside that region.
        self._align_off = (-self.base_addr) % self.align
        self.nbytes = nbytes
        self._free = [(0, nbytes)]  # sorted free list [(offset, size)] in region space

    @property
    def alignment(self):
        return self.align

    def allocate(self, nbytes):
        """Hand out a zero-copy window; returns ArenaBuffer or None."""
        nbytes = (nbytes + self.align - 1) & ~(self.align - 1)  # align up
        for i, (off, size) in enumerate(self._free):
            if size >= nbytes:
                self._free[i] = (off + nbytes, size - nbytes)
                if self._free[i][1] == 0:
                    self._free.pop(i)
                start = self._align_off + off
                view = memoryview(self.base)[start:start + nbytes]
                return ArenaBuffer(self, view, start, nbytes)
        return None  # arena full — governor decides (Law 4, never raise)

    def _release(self, buf):
        off = buf.byteOffset - self._align_off
        self._free.append((off, buf.nbytes))
        self._free.sort()

    @property
    def used_bytes(self):
        return self.nbytes - sum(s for _, s in self._free)


class TensorArena:
    """Detects the best backend and sizes the slab from the profile budget."""

    def __init__(self, profile=None, budget_bytes=None, probe_order=None):
        ladder = probe_order or _BACKEND_LADDER
        self.probeErrors = []
        self.backend = "cpu_simd"
        for name in ladder:
            try:
                if name == "cuda" and not detect_cuda():
                    self.probeErrors.append(E_PROBE_UNAVAILABLE)
                    continue
                if name == "mps" and not detect_apple_mps():
                    self.probeErrors.append(E_PROBE_UNAVAILABLE)
                    continue
                if name == "openvino" and not detect_openvino():
                    self.probeErrors.append(E_PROBE_UNAVAILABLE)
                    continue
                self.backend = name
                break
            except Exception:
                self.probeErrors.append(E_DEVICE_LOST)  # probe blew up: down-tier
        self.profile = profile
        if budget_bytes is None:
            budget_bytes = getattr(profile, "memoryBudgetBytes", 0) or 64 * 1024 * 1024
        self.budgetBytes = budget_bytes
        self.slab = ArenaSlab(
            budget_bytes,
            backend=self.backend,
            cache_line_bytes=getattr(profile, "cacheLineBytes", 64) or 64,
            simd_width_bits=getattr(profile, "simdWidthBits", 128) or 128,
        )

    def allocate(self, nbytes):
        return self.slab.allocate(nbytes)

    def down_tier(self):
        """Transparent fallback: force the next ladder rung, re-slab, keep serving."""
        idx = _BACKEND_LADDER.index(self.backend)
        if idx + 1 < len(_BACKEND_LADDER):
            self.backend = _BACKEND_LADDER[idx + 1]
        self.slab = ArenaSlab(
            self.budgetBytes // 2,  # budget halves per Law §4 rule 5
            backend=self.backend,
            cache_line_bytes=getattr(self.profile, "cacheLineBytes", 64) if self.profile else 64,
            simd_width_bits=getattr(self.profile, "simdWidthBits", 128) if self.profile else 128,
        )
        return self.backend


# ---------------------------------------------------------------------------
# DLPack capsule (struct-encoded DLManagedTensor over the arena window)
# ---------------------------------------------------------------------------
#
# Zero-copy: the capsule's data pointer IS (slab base address + window
# offset). dtype uint8 (kDLUInt, 8 bits, 1 lane); device per slab backend.
# Legacy capsule name "dltensor" pairs with the v0.8 struct (both numpy and
# torch consume it). The arena slab stays pinned by the ArenaSlab; the
# capsule's own scratch is freed by PyMem_RawFree via the DLPack deleter.

_PyCapsule_New = ctypes.pythonapi.PyCapsule_New
_PyCapsule_New.restype = ctypes.py_object
_PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

_PyMem_RawMalloc = ctypes.pythonapi.PyMem_RawMalloc
_PyMem_RawMalloc.restype = ctypes.c_void_p
_PyMem_RawMalloc.argtypes = [ctypes.c_size_t]
_PyMem_RawFree = ctypes.pythonapi.PyMem_RawFree
_PyMem_RawFree.restype = None
_PyMem_RawFree.argtypes = [ctypes.c_void_p]
# void PyMem_RawFree(void*) has EXACTLY the DLPack deleter signature
# (void deleter(DLManagedTensor*)) — a real C function, immune to Python
# interpreter shutdown ordering (no ctypes callback trampoline involved).
_PYMEM_RAWFREE_ADDR = ctypes.cast(_PyMem_RawFree, ctypes.c_void_p).value

# DLTensor:   void* data; i32 dev_type; i32 dev_id; i32 ndim;
#             {u8 code,u8 bits,u8 lanes,pad}; i64* shape; i64* strides; i64 off
# Native 64-bit layout: 8 | 4+4 | 4+4 | 8 | 8 | 8 = 48 bytes
_DLTENSOR = struct.Struct("Pii i BBBx P P Q")
# DLManagedTensor = DLTensor + void* manager_ctx + void(*deleter)(DLManagedTensor*) = 64 bytes
_DLMANAGED = struct.Struct("Pii i BBBx P P Q PP")
assert _DLTENSOR.size == 48 and _DLMANAGED.size == 64

# One RawMalloc block per capsule: [managed 64 | shape 8 | strides 8] = 80B.
# The deleter (PyMem_RawFree) frees the whole block in one call — every
# pointer inside the block points INTO the same allocation, so the single
# free is complete and correct (no multi-free, no leaks, no crash paths).
_BLOCK_SIZE = _DLMANAGED.size + 16


def _make_dlpack_capsule(buf):
    slab = buf.slab
    data = slab.base_addr + buf.byteOffset
    dev_type, dev_id = buf.__dlpack_device__()

    block = _PyMem_RawMalloc(_BLOCK_SIZE)
    if not block:
        raise MemoryError("arena dlpack block")  # host OOM surfaced (Law 4)
    shape_ptr = block + _DLMANAGED.size
    strides_ptr = shape_ptr + 8

    managed = _DLMANAGED.pack(
        data, dev_type, dev_id, 1, 1, 8, 1,     # uint8, 1-D
        shape_ptr, strides_ptr, 0,              # shape*, strides*, byte_offset
        0, _PYMEM_RAWFREE_ADDR,                 # manager_ctx, deleter
    )
    shape_bytes = struct.pack("<q", buf.nbytes)
    ctypes.memmove(block, managed, _DLMANAGED.size)
    ctypes.memmove(shape_ptr, shape_bytes, 8)
    ctypes.memmove(strides_ptr, struct.pack("<q", 1), 8)

    buf._capsule = block
    # Legacy capsule name pairs with the v0.8 struct layout above (numpy and
    # torch both consume it; the "dltensor_versioned" name would require the
    # DLPack 1.0 struct with an 8-byte version prefix before data).
    capsule = _PyCapsule_New(block, b"dltensor", None)
    return capsule


def dlpack_available():
    """Structural DLPack lane (CPU) — always true; torch is optional."""
    return True
