"""dlpack_capi.py — DLPack via ctypes: DLManagedTensor capsules with proper
lifetime management (Law 2/4: zero-copy sharing with PyTorch / NumPy).

Implements the PRODUCER side of the DLPack protocol:

    obj.__dlpack__(stream=None, max_version=None, dl_device=None, copy=None)
        -> PyCapsule named "dltensor" wrapping a DLManagedTensor*
    obj.__dlpack_device__() -> (kDLCPU /*1*/, device_id)

The capsule deleter (called by the CONSUMER when the tensor is released)
frees the shape/strides scratch buffers and the DLManagedTensor struct, and
drops the registry's reference on the ring owner — so a torch tensor keeps
the ring's memory alive for exactly as long as the tensor exists.

kDLCPU = 1 (dlpack.h DLDeviceType). Capsule name "dltensor" = the widely
supported unversioned protocol (consumers rename it to "used_dltensor" on
adoption; versioned consumers accept legacy capsules per the DLPack spec).
"""
from __future__ import annotations

import ctypes
import ctypes.util
import threading

# --- dlpack.h structs (little-endian hosts; all supported targets are LE) ---

DLCPU = 1  # kDLCPU


class DLDevice(ctypes.Structure):
    _fields_ = [("device_type", ctypes.c_int), ("device_id", ctypes.c_int)]


class DLDataType(ctypes.Structure):
    _fields_ = [
        ("code", ctypes.c_uint8),
        ("bits", ctypes.c_uint8),
        ("lanes", ctypes.c_uint16),
    ]


class DLTensor(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.c_void_p),
        ("device", DLDevice),
        ("ndim", ctypes.c_int),
        ("dtype", DLDataType),
        ("shape", ctypes.POINTER(ctypes.c_int64)),
        ("strides", ctypes.POINTER(ctypes.c_int64)),
        ("byte_offset", ctypes.c_uint64),
    ]


DELETER_FUNCTYPE = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class DLManagedTensor(ctypes.Structure):
    _fields_ = [
        ("dl_tensor", DLTensor),
        ("manager_ctx", ctypes.c_void_p),
        ("deleter", DELETER_FUNCTYPE),
    ]


_libc = ctypes.CDLL(ctypes.util.find_library("c") or None)
_libc.malloc.argtypes = [ctypes.c_size_t]
_libc.malloc.restype = ctypes.c_void_p
_libc.free.argtypes = [ctypes.c_void_p]
_libc.free.restype = None
_pythonapi = ctypes.pythonapi
_pythonapi.PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
_pythonapi.PyCapsule_New.restype = ctypes.py_object

# Registry keeping the ring OWNER alive (plain Python reference — no manual
# refcounting) plus the deleter callback and scratch buffers, until the
# CONSUMER's deleter fires. Keyed by the DLManagedTensor struct address.
_REGISTRY: dict = {}
_LOCK = threading.Lock()


def _make_deleter():
    def _deleter(struct_ptr_void):
        key = struct_ptr_void  # the DLManagedTensor* the consumer hands back
        with _LOCK:
            entry = _REGISTRY.pop(key, None)
        if entry is None:
            return  # double-free guard (defensive; consumers call once)
        _owner, cb, shape_buf, strides_buf, struct_ptr = entry
        if shape_buf:
            _libc.free(shape_buf)
        if strides_buf:
            _libc.free(strides_buf)
        _libc.free(struct_ptr)
        # `_owner` reference dies with this frame -> ring may be collected
        # once the CONSUMER is done with the tensor. Exactly DLPack semantics.
    return DELETER_FUNCTYPE(_deleter)


def build_capsule(data_address: int, ndim: int, dtype_code: int, dtype_bits: int,
                  shape, strides_elems, elem_size: int, owner) -> object:
    """Build a "dltensor" PyCapsule over ALREADY-EXISTING memory.

    ALL scratch memory (struct + shape + strides) is malloc'd here and freed
    ONLY by the consumer-triggered deleter — never by ctypes (which would be
    a double free). `strides_elems` None => C-contiguous (strides = NULL).
    """
    ndim = int(ndim)
    n = max(ndim, 1)

    shape_void = _libc.malloc(8 * n)
    shape_buf = ctypes.cast(shape_void, ctypes.POINTER(ctypes.c_int64))
    for d in range(ndim):
        shape_buf[d] = int(shape[d])

    strides_void = None
    if strides_elems is not None:
        strides_void = _libc.malloc(8 * n)
        strides_buf = ctypes.cast(strides_void, ctypes.POINTER(ctypes.c_int64))
        for d in range(ndim):
            strides_buf[d] = int(strides_elems[d])  # in ELEMENTS (DLPack convention)

    struct_void = _libc.malloc(ctypes.sizeof(DLManagedTensor))
    struct_ptr = ctypes.cast(struct_void, ctypes.POINTER(DLManagedTensor))
    tensor = struct_ptr.contents
    tensor.dl_tensor.data = ctypes.c_void_p(data_address)
    tensor.dl_tensor.device = DLDevice(DLCPU, 0)
    tensor.dl_tensor.ndim = ndim
    tensor.dl_tensor.dtype = DLDataType(dtype_code, dtype_bits, 1)
    tensor.dl_tensor.shape = shape_buf
    tensor.dl_tensor.strides = (ctypes.cast(strides_void, ctypes.POINTER(ctypes.c_int64))
                                if strides_void else None)
    tensor.dl_tensor.byte_offset = 0

    cb = _make_deleter()
    tensor.manager_ctx = None
    tensor.deleter = cb

    key = struct_void
    with _LOCK:
        # Plain reference to `owner` in the registry keeps the ring alive.
        _REGISTRY[key] = (owner, cb, shape_void, strides_void, struct_void)

    capsule = _pythonapi.PyCapsule_New(key, b"dltensor", None)
    return capsule
