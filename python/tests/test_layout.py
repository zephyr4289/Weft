"""test_layout.py — WTR1 layout: CRC (zlib is the authority), fail-closed
corruption matrix, dtype table. Law 2/4."""
import struct
import zlib

import pytest

from weft_tensor import (
    LayoutError, RING_HEADER_SIZE, OFF_PRODUCER_SEQ, OFF_HEADER_CRC, OFF_FLAGS,
    RING_FLAG_LITTLE_ENDIAN, validate_ring_header, ring_header_crc,
    is_valid_dtype, DLPACK_FLOAT, DLPACK_UINT, fourcc_from_str, fourcc_to_str,
    WeftRing,
)


def make_ring():
    return WeftRing.create(slot_count=4, payload_cap=24,
                           dtype_code=DLPACK_FLOAT, dtype_bits=32,
                           shape=[2, 3], schema_id=0xA11CE00000000001)


def test_crc_matches_zlib_over_static_region():
    ring = make_ring()
    mv = ring._mv
    static = bytes(mv[0:96]) + bytes(mv[104:112])
    expected = zlib.crc32(static) & 0xFFFFFFFF
    assert ring_header_crc(mv) == expected
    assert struct.unpack_from("<I", mv, OFF_HEADER_CRC)[0] == expected


def test_create_attaches_and_describes():
    ring = make_ring()
    L = ring.layout
    assert L.version == 1 and L.slot_count == 4 and L.slot_stride == 128
    assert L.payload_cap == 64 and L.tick_hz == 0
    assert L.shape[:3] == (2, 3, 0)
    assert L.strides[:2] == (3, 1)
    assert "shape=[2x3]" in ring.describe()
    again = WeftRing.attach(bytes(ring._mv))
    assert again.layout.schema_id == 0xA11CE00000000001
    assert again.producer_seq == 0


def test_fail_closed_corruption_matrix():
    ring = make_ring()
    cases = [
        ("WTR1_BAD_MAGIC", lambda m: m.__setitem__(0, 0x58)),
        ("WTR1_BAD_VERSION", lambda m: struct.pack_into("<H", m, 4, 2)),
        ("WTR1_BAD_HEADER_SIZE", lambda m: struct.pack_into("<H", m, 6, 256)),
        ("WTR1_BAD_SLOT_COUNT", lambda m: struct.pack_into("<I", m, 8, 1)),
        ("WTR1_BAD_SLOT_STRIDE", lambda m: struct.pack_into("<I", m, 12, 100)),
        ("WTR1_BAD_DTYPE", lambda m: m.__setitem__(16, 9)),
        ("WTR1_BAD_DTYPE", lambda m: struct.pack_into("<H", m, 18, 2)),
        ("WTR1_BAD_ELEM_SIZE", lambda m: struct.pack_into("<I", m, 20, 3)),
        ("WTR1_BAD_STRIDES", lambda m: struct.pack_into("<I", m, 60, 0)),
        ("WTR1_SEQ_OVERFLOW", lambda m: struct.pack_into("<I", m, OFF_PRODUCER_SEQ + 4, 0x200000)),
        ("WTR1_NOT_LITTLE_ENDIAN", lambda m: struct.pack_into("<I", m, OFF_FLAGS, 2)),
        ("WTR1_BAD_CRC", lambda m: struct.pack_into("<I", m, OFF_HEADER_CRC, 0xDEADBEEF)),
    ]
    for code, mutate in cases:
        buf = bytearray(bytes(ring._mv))
        mutate(memoryview(buf))
        with pytest.raises(LayoutError) as ei:
            validate_ring_header(bytes(buf))
        assert ei.value.code == code, f"expected {code}, got {ei.value.code}"
    with pytest.raises(LayoutError) as e:
        validate_ring_header(b"\x00" * 64)
    assert e.value.code == "WTR1_SHORT"
    with pytest.raises(LayoutError) as e:
        validate_ring_header(bytes(ring._mv)[:600])
    assert e.value.code == "WTR1_SHORT_RING"


def test_dtype_table_and_fourcc():
    assert is_valid_dtype(DLPACK_FLOAT, 16)
    assert is_valid_dtype(DLPACK_UINT, 64)
    assert not is_valid_dtype(3, 16)  # bfloat reserved for V2
    assert fourcc_to_str(fourcc_from_str("RGBA")) == "RGBA"
    assert fourcc_from_str("RGBA") == 0x41424752  # LE bytes 'R','G','B','A'
