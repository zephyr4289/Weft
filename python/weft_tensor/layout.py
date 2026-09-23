"""layout.py — WTR1 (Weft Tensor Ring v1) normative layout primitives.

Spec: docs/weft-tensor/LAYOUT-V1.md (NORMATIVE).
Law 2: every multi-byte field is parsed with explicit little-endian '<'
struct formats — bit-exact with the frozen C kernel envelope, the TypeScript
ring (packages/weft-tensor) and the Dart FFI layer.
Law 4: fail-closed validation with machine-readable error codes identical to
the TypeScript implementation.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from typing import Tuple

RING_MAGIC = b"WEFT"           # 0x57 0x45 0x46 0x54
SLOT_MAGIC = b"WFRM"           # 0x57 0x46 0x52 0x4D

LAYOUT_VERSION = 1
RING_HEADER_SIZE = 128
SLOT_HEADER_SIZE = 64

# Ring header offsets
OFF_MAGIC = 0
OFF_LAYOUT_VERSION = 4
OFF_HEADER_SIZE = 6
OFF_SLOT_COUNT = 8
OFF_SLOT_STRIDE = 12
OFF_DTYPE_CODE = 16
OFF_DTYPE_BITS = 17
OFF_LANES = 18
OFF_ELEM_SIZE = 20
OFF_SHAPE = 24        # u32 x8
OFF_STRIDES = 56      # u32 x8, in ELEMENTS (DLPack convention)
OFF_SCHEMA_ID = 88    # u64
OFF_PRODUCER_SEQ = 96  # u64 — the publish word (lo word stored LAST)
OFF_TICK_HZ = 104
OFF_FLAGS = 108
OFF_HEADER_CRC = 112  # CRC-32/IEEE over bytes [0,96) ++ [104,112)

RING_FLAG_LITTLE_ENDIAN = 1
RING_FLAG_SHARED_MEMORY = 2

# Slot header offsets (relative to slot base)
SOFF_MAGIC = 0
SOFF_PAYLOAD_LEN = 4
SOFF_SEQ = 8
SOFF_TIMESTAMP_NS = 16
SOFF_DURATION_US = 24
SOFF_SLOT_FLAGS = 28
SOFF_FOURCC = 32
SOFF_RANK = 36
SOFF_PLANES = 37
SOFF_PLANE_OFFSET = 40
SOFF_PLANE_SIZE = 52

SLOT_FLAG_COMMITTED = 1
MAX_RANK = 8

# DLPack DLDataTypeCode — VERBATIM on the wire (zero-mapping parity).
DLPACK_INT, DLPACK_UINT, DLPACK_FLOAT, DLPACK_BFLOAT, DLPACK_COMPLEX, DLPACK_BOOL = 0, 1, 2, 3, 4, 5

_VALID_DTYPES = {
    (DLPACK_INT, 8), (DLPACK_INT, 16), (DLPACK_INT, 32), (DLPACK_INT, 64),
    (DLPACK_UINT, 8), (DLPACK_UINT, 16), (DLPACK_UINT, 32), (DLPACK_UINT, 64),
    (DLPACK_FLOAT, 16), (DLPACK_FLOAT, 32), (DLPACK_FLOAT, 64),
    (DLPACK_BOOL, 8),
}


class LayoutError(Exception):
    """Fail-closed WTR1 validation error (code mirrors TypeScript LayoutError)."""

    def __init__(self, code: str, message: str):
        super().__init__(f"[{code}] {message}")
        self.code = code


def ring_header_crc(mv) -> int:
    """CRC-32/IEEE (zlib) over the static config: bytes [0,96) ++ [104,112).

    Chained zlib.crc32(data, crc) over the two regions == CRC of the
    concatenation (verified against node:zlib in both test suites).
    """
    crc = zlib.crc32(bytes(mv[0:96]))
    crc = zlib.crc32(bytes(mv[104:112]), crc)
    return crc & 0xFFFFFFFF


def is_valid_dtype(code: int, bits: int) -> bool:
    return (code, bits) in _VALID_DTYPES


def fourcc_from_str(s: str) -> int:
    b = s.encode("ascii")[:4].ljust(4, b" ")
    return struct.unpack("<I", b)[0]


def fourcc_to_str(u32: int) -> str:
    return struct.pack("<I", u32 & 0xFFFFFFFF).decode("ascii", "replace")


@dataclass(frozen=True)
class RingLayout:
    """Parsed + validated ring header. Allocated ONCE at attach."""
    version: int
    header_size: int
    slot_count: int
    slot_stride: int
    dtype_code: int
    dtype_bits: int
    lanes: int
    elem_size: int
    shape: Tuple[int, ...]
    strides: Tuple[int, ...]   # in ELEMENTS (DLPack convention)
    rank: int
    schema_id: int
    tick_hz: int
    flags: int
    payload_cap: int
    byte_length: int

    @property
    def dtype_name(self) -> str:
        letter = {DLPACK_INT: "i", DLPACK_UINT: "u", DLPACK_FLOAT: "f",
                  DLPACK_BOOL: "b"}.get(self.dtype_code, "?")
        return f"{letter}{self.dtype_bits}"


def read_slot_header(mv, slot_base: int, out: dict) -> bool:
    """Read + validate one slot header INTO the caller's dict (zero-alloc hot path).

    Returns False for torn/in-progress writes (seqlock contract).
    """
    if bytes(mv[slot_base + SOFF_MAGIC: slot_base + SOFF_MAGIC + 4]) != SLOT_MAGIC:
        return False
    flags = struct.unpack_from("<I", mv, slot_base + SOFF_SLOT_FLAGS)[0]
    if (flags & SLOT_FLAG_COMMITTED) == 0:
        return False
    seq = struct.unpack_from("<Q", mv, slot_base + SOFF_SEQ)[0]
    if seq >> 53:
        return False  # managed Number-path bound (parity with TS)
    out["seq"] = seq
    out["seq_lo"] = seq & 0xFFFFFFFF
    out["seq_hi"] = seq >> 32
    out["payload_len"] = struct.unpack_from("<I", mv, slot_base + SOFF_PAYLOAD_LEN)[0]
    out["timestamp_ns"] = struct.unpack_from("<Q", mv, slot_base + SOFF_TIMESTAMP_NS)[0]
    out["duration_us"] = struct.unpack_from("<I", mv, slot_base + SOFF_DURATION_US)[0]
    out["flags"] = flags
    out["fourcc"] = struct.unpack_from("<I", mv, slot_base + SOFF_FOURCC)[0]
    out["rank"] = mv[slot_base + SOFF_RANK]
    out["planes"] = mv[slot_base + SOFF_PLANES]
    return True


def validate_ring_header(buf) -> RingLayout:
    """Law 4 boundary: fail-closed validation of a WTR1 ring header.

    `buf` is any buffer supporting len() and slicing (bytes, bytearray,
    memoryview, mmap). Throws LayoutError with a machine-readable code.
    """
    mv = memoryview(buf)
    byte_length = len(mv)
    if byte_length < RING_HEADER_SIZE:
        raise LayoutError("WTR1_SHORT", f"buffer is {byte_length}B, ring header needs {RING_HEADER_SIZE}B")
    if bytes(mv[OFF_MAGIC:OFF_MAGIC + 4]) != RING_MAGIC:
        raise LayoutError("WTR1_BAD_MAGIC", 'ring magic is not "WEFT"')
    version = struct.unpack_from("<H", mv, OFF_LAYOUT_VERSION)[0]
    if version != LAYOUT_VERSION:
        raise LayoutError("WTR1_BAD_VERSION", f"layout_version {version} != {LAYOUT_VERSION}")
    header_size = struct.unpack_from("<H", mv, OFF_HEADER_SIZE)[0]
    if header_size != RING_HEADER_SIZE:
        raise LayoutError("WTR1_BAD_HEADER_SIZE", f"header_size {header_size} != {RING_HEADER_SIZE}")
    slot_count = struct.unpack_from("<I", mv, OFF_SLOT_COUNT)[0]
    if slot_count < 2:
        raise LayoutError("WTR1_BAD_SLOT_COUNT", f"slot_count {slot_count} < 2")
    slot_stride = struct.unpack_from("<I", mv, OFF_SLOT_STRIDE)[0]
    if slot_stride % 64 != 0 or slot_stride < SLOT_HEADER_SIZE:
        raise LayoutError("WTR1_BAD_SLOT_STRIDE", f"slot_stride {slot_stride} is not 64-aligned / < 64")
    code = mv[OFF_DTYPE_CODE]
    bits = mv[OFF_DTYPE_BITS]
    lanes = struct.unpack_from("<H", mv, OFF_LANES)[0]
    if not is_valid_dtype(code, bits) or lanes != 1:
        raise LayoutError("WTR1_BAD_DTYPE", f"dtype {code}/{bits}bits x{lanes}lanes not supported in V1")
    elem_size = struct.unpack_from("<I", mv, OFF_ELEM_SIZE)[0]
    if elem_size != (bits // 8) * lanes:
        raise LayoutError("WTR1_BAD_ELEM_SIZE", f"elem_size {elem_size} != {(bits // 8) * lanes}")
    shape = struct.unpack_from("<8I", mv, OFF_SHAPE)
    strides = struct.unpack_from("<8I", mv, OFF_STRIDES)
    rank = 0
    for d in range(MAX_RANK - 1, -1, -1):
        if shape[d] != 0:
            rank = d + 1
            break
    if rank == 0:
        raise LayoutError("WTR1_BAD_RANK", "shape is all-zero (rank-0 tensors not supported in V1)")
    for i in range(rank - 1):
        if strides[i] < strides[i + 1] * shape[i + 1]:
            raise LayoutError("WTR1_BAD_STRIDES", "strides are not a sane row-major layout")
    if strides[rank - 1] < 1:
        raise LayoutError("WTR1_BAD_STRIDES", "unit stride must be >= 1")
    seq = struct.unpack_from("<Q", mv, OFF_PRODUCER_SEQ)[0]
    if seq >> 53:
        raise LayoutError("WTR1_SEQ_OVERFLOW", "producer_seq exceeds 2^53 — unsupported by the managed Number path")
    flags = struct.unpack_from("<I", mv, OFF_FLAGS)[0]
    if (flags & RING_FLAG_LITTLE_ENDIAN) == 0:
        raise LayoutError("WTR1_NOT_LITTLE_ENDIAN", "flags bit0 (little-endian) not set — refusing to guess byte order")
    want_crc = struct.unpack_from("<I", mv, OFF_HEADER_CRC)[0]
    got_crc = ring_header_crc(mv)
    if want_crc != got_crc:
        raise LayoutError("WTR1_BAD_CRC", f"header crc {want_crc:#x} != computed {got_crc:#x}")
    need = header_size + slot_count * slot_stride
    if byte_length < need:
        raise LayoutError("WTR1_SHORT_RING", f"buffer {byte_length}B < header + {slot_count} slots ({need}B)")

    schema_id = struct.unpack_from("<Q", mv, OFF_SCHEMA_ID)[0]
    return RingLayout(
        version=version, header_size=header_size,
        slot_count=slot_count, slot_stride=slot_stride,
        dtype_code=code, dtype_bits=bits, lanes=lanes, elem_size=elem_size,
        shape=shape, strides=strides, rank=rank,
        schema_id=schema_id,
        tick_hz=struct.unpack_from("<I", mv, OFF_TICK_HZ)[0],
        flags=flags,
        payload_cap=slot_stride - SLOT_HEADER_SIZE,
        byte_length=byte_length,
    )
