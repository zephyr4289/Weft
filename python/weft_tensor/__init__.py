"""weft-tensor — Python bridge into the Weft managed tensor plane.

Zero-copy tensor rings with DLPack / PyTorch / NumPy interop.

    import torch, weft_tensor as wt
    ring = wt.WeftRing.attach("camera_ring.mm")        # mmap'd WTR1 memory
    view = ring.acquire_latest()                       # wait-free, zero-alloc
    tensor = torch.from_dlpack(view)                   # zero-copy, zero-mapping

Laws (Pillar 2): (1) zero allocation in steady state, (2) strict little-endian
+ IEEE 754 parity, (3) universal buffers (bytes/mmap/memoryview/ndarray),
(4) fail-closed boundary validation with machine-readable codes.
"""
from .layout import (
    LayoutError,
    RingLayout,
    validate_ring_header,
    read_slot_header,
    ring_header_crc,
    is_valid_dtype,
    fourcc_from_str,
    fourcc_to_str,
    RING_MAGIC,
    SLOT_MAGIC,
    LAYOUT_VERSION,
    RING_HEADER_SIZE,
    SLOT_HEADER_SIZE,
    OFF_MAGIC, OFF_LAYOUT_VERSION, OFF_HEADER_SIZE, OFF_SLOT_COUNT, OFF_SLOT_STRIDE,
    OFF_DTYPE_CODE, OFF_DTYPE_BITS, OFF_LANES, OFF_ELEM_SIZE, OFF_SHAPE, OFF_STRIDES,
    OFF_SCHEMA_ID, OFF_PRODUCER_SEQ, OFF_TICK_HZ, OFF_FLAGS, OFF_HEADER_CRC,
    SOFF_MAGIC, SOFF_PAYLOAD_LEN, SOFF_SEQ, SOFF_TIMESTAMP_NS, SOFF_DURATION_US,
    SOFF_SLOT_FLAGS, SOFF_FOURCC, SOFF_RANK, SOFF_PLANES, SOFF_PLANE_OFFSET, SOFF_PLANE_SIZE,
    DLPACK_INT,
    DLPACK_UINT,
    DLPACK_FLOAT,
    DLPACK_BOOL,
    RING_FLAG_LITTLE_ENDIAN,
    RING_FLAG_SHARED_MEMORY,
    SLOT_FLAG_COMMITTED,
    MAX_RANK,
)
from .ring import WeftRing, numpy_dtype_str
from .view import WeftTensorView
from .ingest import AudioPcmFeeder

__version__ = "0.1.0"

__all__ = [
    "WeftRing", "WeftTensorView", "AudioPcmFeeder",
    "LayoutError", "RingLayout",
    "validate_ring_header", "read_slot_header", "ring_header_crc",
    "is_valid_dtype", "numpy_dtype_str",
    "fourcc_from_str", "fourcc_to_str",
    "RING_MAGIC", "SLOT_MAGIC", "LAYOUT_VERSION", "RING_HEADER_SIZE",
    "SLOT_HEADER_SIZE",
    "OFF_MAGIC", "OFF_LAYOUT_VERSION", "OFF_HEADER_SIZE", "OFF_SLOT_COUNT",
    "OFF_SLOT_STRIDE", "OFF_DTYPE_CODE", "OFF_DTYPE_BITS", "OFF_LANES",
    "OFF_ELEM_SIZE", "OFF_SHAPE", "OFF_STRIDES", "OFF_SCHEMA_ID",
    "OFF_PRODUCER_SEQ", "OFF_TICK_HZ", "OFF_FLAGS", "OFF_HEADER_CRC",
    "SOFF_MAGIC", "SOFF_PAYLOAD_LEN", "SOFF_SEQ", "SOFF_TIMESTAMP_NS",
    "SOFF_DURATION_US", "SOFF_SLOT_FLAGS", "SOFF_FOURCC", "SOFF_RANK",
    "SOFF_PLANES", "SOFF_PLANE_OFFSET", "SOFF_PLANE_SIZE",
    "DLPACK_INT", "DLPACK_UINT", "DLPACK_FLOAT", "DLPACK_BOOL",
    "RING_FLAG_LITTLE_ENDIAN", "RING_FLAG_SHARED_MEMORY", "SLOT_FLAG_COMMITTED",
    "MAX_RANK", "__version__",
]
