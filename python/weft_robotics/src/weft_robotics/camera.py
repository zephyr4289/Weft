# camera.py — zero-copy camera frame source over a mapped arena (managed).
#
# Engineer 2's V4L2/DMA-BUF grabber owns the mapping; this module is the
# managed consumer contract (docs/adapters/MANAGED-SEAMS-V1.md §6):
#   - MappedArena: one self-contained aligned slab (ctypes buffer), slots
#     handed out as CameraFrame windows at slot addresses
#   - CameraFrame.get_tensor(): DLPack v0.8 capsule whose data pointer IS
#     the arena slot address — POINTER IDENTITY is the acceptance proof
#     (Stage 6). np.from_dlpack(frame) / torch.from_dlpack(frame) work
#     verbatim with ZERO copies.
#   - Frm1View: flyweight over the FRM1 descriptor (§6) — geometry only,
#     the heavy pixels never enter Python objects.
#
# The capsule's DLManagedTensor scratch is ONE PyMem_RawMalloc block
# freed by PyMem_RawFree itself acting as the DLPack deleter (exact
# C-signature match, no ctypes callback trampoline, shutdown-safe —
# Pillar 5 arena precedent).

import ctypes
import struct

FRM1_MAGIC = b'FRM1'
FRM1_SIZE = 32

FMT_RGB8 = 1
FMT_BGR8 = 2
FMT_NV12 = 3
FMT_GRAY8 = 4

_DLPACK_CPU = 1
_DLPACK_CUDA = 2


class Frm1Error(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


E_SHORT = 1
E_MAGIC = 2


class Frm1View:
    """Flyweight over one 32-byte FRM1 frame descriptor (fail-closed)."""

    __slots__ = ('_mv',)

    def __init__(self, mv):
        if len(mv) < FRM1_SIZE:
            raise Frm1Error(E_SHORT)
        if bytes(mv[0:4]) != FRM1_MAGIC:
            raise Frm1Error(E_MAGIC)
        self._mv = mv

    @property
    def width(self):
        return struct.unpack_from('<I', self._mv, 4)[0]

    @property
    def height(self):
        return struct.unpack_from('<I', self._mv, 8)[0]

    @property
    def stride(self):
        return struct.unpack_from('<I', self._mv, 12)[0]

    @property
    def format(self):
        return struct.unpack_from('<I', self._mv, 16)[0]

    @property
    def handle(self):
        return (struct.unpack_from('<I', self._mv, 20)[0],
                struct.unpack_from('<I', self._mv, 24)[0])

    @property
    def flags(self):
        return struct.unpack_from('<I', self._mv, 28)[0]


class CameraFrame:
    """One arena slot window. Consumers get DLPack/NumPy views over the
    SAME bytes — never a copy. Retained by the arena until release()."""

    __slots__ = ('_arena', 'byteOffset', 'nbytes', 'width', 'height',
                 'stride', 'format', 'seq')

    def __init__(self, arena, byteOffset, nbytes, width, height, stride,
                 fmt, seq):
        self._arena = arena
        self.byteOffset = byteOffset
        self.nbytes = nbytes
        self.width = width
        self.height = height
        self.stride = stride
        self.format = fmt
        self.seq = seq

    def __dlpack_device__(self):
        return (_DLPACK_CPU, 0)

    def __dlpack__(self, stream=None):
        return _make_dlpack_capsule(self)

    def address(self):
        """Absolute virtual address of the slot window (Stage 6 oracle).
        Includes the arena's alignment offset — MUST match the address
        numpy/torch see through window()/DLPack."""
        return self._arena.base_addr + self._arena._align_off + self.byteOffset

    def get_numpy(self):
        """Zero-copy numpy view (np.frombuffer over the slot window)."""
        np = self._arena.numpy()
        return np.frombuffer(
            self._arena.window(self.byteOffset, self.nbytes),
            dtype=np.uint8).reshape(self.height, self.stride)

    def release(self):
        self._arena.release(self)


# ---------------------------------------------------------------------------
# MappedArena — one self-contained slab, 64B-aligned slots
# ---------------------------------------------------------------------------

class MappedArena:
    """ctypes-backed frame arena. In production this maps Eng-2's DMA-BUF
    handles; in the managed sandbox it IS the mapped memory (the same
    pointer-identity contract applies)."""

    def __init__(self, nbytes, slot_bytes=1 << 20):
        self.slot_bytes = slot_bytes
        self.base = ctypes.create_string_buffer(nbytes)
        self.base_addr = ctypes.addressof(self.base)
        # align the usable region to 64B (cache line)
        self._align_off = (-self.base_addr) % 64
        self.capacity = nbytes - self._align_off
        self._free = list(range(0, self.capacity // slot_bytes))
        self._live = {}

    @staticmethod
    def numpy():
        try:
            import numpy
            return numpy
        except ImportError as e:
            raise RuntimeError(
                'numpy is required for camera lane '
                '(pip install numpy)') from e

    def window(self, byteOffset, nbytes):
        """Buffer-exportable window over [byteOffset, +nbytes)."""
        return memoryview(self.base).cast(
            'B')[self._align_off + byteOffset:
                 self._align_off + byteOffset + nbytes]

    def acquire_slot(self, nbytes, width, height, stride, fmt, seq):
        if nbytes > self.slot_bytes:
            raise ValueError('frame exceeds slot_bytes')
        if not self._free:
            # drop-oldest (drop-not-block, Law 4)
            idx = next(iter(self._live))
            del self._live[idx]
            self._free.append(idx)
        idx = self._free.pop(0)
        frame = CameraFrame(self, idx * self.slot_bytes, nbytes,
                            width, height, stride, fmt, seq)
        self._live[idx] = frame
        return frame

    def release(self, frame):
        for idx, live in list(self._live.items()):
            if live is frame:
                del self._live[idx]
                self._free.append(idx)
                return

    def close(self):
        self._free.clear()
        self._live.clear()


# ---------------------------------------------------------------------------
# DLPack capsule machinery (Pillar 5 arena precedent, verbatim technique)
# ---------------------------------------------------------------------------

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
# (void deleter(DLManagedTensor*)) — real C function, shutdown-safe.
_PYMEM_RAWFREE_ADDR = ctypes.cast(_PyMem_RawFree, ctypes.c_void_p).value

# DLTensor: void* data; i32 dev_type; i32 dev_id; i32 ndim;
#           {u8 code,u8 bits,u8 lanes,pad}; i64* shape; i64* strides; i64 off
_DLTENSOR = struct.Struct('Pii i BBBx P P Q')
# DLManagedTensor = DLTensor + manager_ctx + deleter = 64 bytes
_DLMANAGED = struct.Struct('Pii i BBBx P P Q PP')
assert _DLTENSOR.size == 48 and _DLMANAGED.size == 64

_BLOCK_SIZE = _DLMANAGED.size + 32  # shape 2×i64 + strides 2×i64


def _make_dlpack_capsule(frame):
    data = frame.address()
    dev_type, dev_id = frame.__dlpack_device__()

    block = _PyMem_RawMalloc(_BLOCK_SIZE)
    if not block:
        raise MemoryError('camera dlpack block')  # host OOM surfaced (Law 4)
    shape_ptr = block + _DLMANAGED.size
    strides_ptr = shape_ptr + 16

    managed = _DLMANAGED.pack(
        data, dev_type, dev_id, 2, 1, 8, 1,     # uint8, 2-D (h x stride)
        shape_ptr, strides_ptr, 0,
        0, _PYMEM_RAWFREE_ADDR,                 # manager_ctx, deleter
    )
    ctypes.memmove(block, managed, _DLMANAGED.size)
    # shape (elements): [height, stride]; strides (bytes): [stride, 1]
    ctypes.memmove(shape_ptr, struct.pack('<qq', frame.height, frame.stride), 16)
    ctypes.memmove(strides_ptr, struct.pack('<qq', frame.stride, 1), 16)

    # Legacy capsule name "dltensor" pairs with the v0.8 struct (numpy and
    # torch both consume it).
    return _PyCapsule_New(block, b'dltensor', None)


class CameraSource:
    """Managed camera seam: grabs FRM1 descriptors, publishes zero-copy
    frames from the arena. The grabber callback writes pixels DIRECTLY
    into the slot (no Python-object path); get_tensor() hands the slot
    to consumers with pointer identity."""

    def __init__(self, width=3840, height=2160, stride=None, fmt=FMT_RGB8,
                 slots=3):
        self.width = width
        self.height = height
        self.stride = stride or (width * 3 if fmt in (FMT_RGB8, FMT_BGR8)
                                 else width)
        self.fmt = fmt
        nbytes = self.height * self.stride
        slot_bytes = ((nbytes + 63) // 64) * 64  # 64B-aligned slots
        self.arena = MappedArena(slots * slot_bytes + 64,
                                 slot_bytes=slot_bytes)
        self._seq = 0

    def grab_into_slot(self):
        """Simulates the grabber: allocates a slot for the next frame.
        Returns (frame, writable window) — the grabber fills the window
        directly (zero-copy: pixels land IN the arena)."""
        self._seq += 1
        nbytes = self.height * self.stride
        frame = self.arena.acquire_slot(nbytes, self.width, self.height,
                                        self.stride, self.fmt, self._seq)
        return frame, self.arena.window(frame.byteOffset, nbytes)

    def get_frame(self):
        """Frame-only grab (descriptor path)."""
        return self.grab_into_slot()[0]

    def get_tensor(self):
        """Zero-copy DLPack tensor over the newest frame's slot.
        torch.from_dlpack(camera.get_tensor()) — 0 ms copy, pointer
        identity guaranteed (Stage 6 acceptance proof)."""
        frame, _ = self.grab_into_slot()
        return frame

    def frm1_pack(self, out, frame):
        """Pack an FRM1 descriptor for `frame` into `out` (32 bytes)."""
        struct.pack_into('<4s4I2I2I', out, 0, FRM1_MAGIC, frame.width,
                         frame.height, frame.stride, frame.format, 0, 0, 0)
        return out

    def close(self):
        self.arena.close()
