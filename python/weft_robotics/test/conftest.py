# conftest.py — path bootstrap + shared RNG1 builder for weft_robotics.

import struct
import sys
from pathlib import Path

PKG_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PKG_ROOT / 'src'))

import pytest  # noqa: E402

RNG1_HEADER_SIZE = 128
SLOT_HDR = 64


def build_ring(records, slot_size=4096, slot_count=8, topics=()):
    """Build an RNG1 ring image: records = list of
    (topic_id, fmt, payload bytes, torn=False)."""
    total = RNG1_HEADER_SIZE + slot_size * slot_count
    buf = bytearray(total)
    struct.pack_into('<4sHH', buf, 0, b'RNG1', 1, RNG1_HEADER_SIZE)
    struct.pack_into('<II', buf, 8, slot_size, slot_count)
    for i, t in enumerate(topics[:8]):
        struct.pack_into('<I', buf, 64 + i * 4, t)
    seq = 0
    for (topic_id, fmt, payload, torn) in records:
        seq += 1
        slot = (seq - 1) % slot_count
        base = RNG1_HEADER_SIZE + slot * slot_size
        body = SLOT_HDR + len(payload)
        if body > slot_size:
            raise ValueError('payload exceeds slot')
        write_seq = seq | 1 if torn else seq
        # slot header: seq@0 u64, len@8 u32, topic@12 u32, ts@16 u64,
        #              fmt@24 u32, flags@28 u32 (MANAGED-SEAMS-V1 §5)
        struct.pack_into('<QI', buf, base, write_seq, len(payload))
        struct.pack_into('<I', buf, base + 12, topic_id)
        struct.pack_into('<Q', buf, base + 16, 1000 + seq)
        struct.pack_into('<II', buf, base + 24, fmt, 0)
        buf[base + SLOT_HDR:base + body] = payload
    struct.pack_into('<Q', buf, 16, seq)  # write_seq
    struct.pack_into('<Q', buf, 24, seq)  # committed
    return bytes(buf)


def imu_payload(ts=111, qw=0.7071, qx=0.0, qy=0.7071, qz=0.0,
                gx=0.01, gy=0.02, gz=0.03):
    # IMU6DOF: 8 x f64 LE (ts_ns encoded as double) — MANAGED-SEAMS-V1 §5.1
    return struct.pack('<8d', float(ts), qw, qx, qy, qz, gx, gy, gz)


def points_payload(n=6):
    vals = []
    for i in range(n):
        vals += [float(i), 0.5, -0.25]
    return struct.pack(f'<{n * 3}f', *vals)


def boxes_payload(m=2):
    out = b''
    for j in range(m):
        out += struct.pack('<6f', float(j), 1.0, 2.0, 40.0, 80.0, 0.9)
    return out


def frm1_payload(width=64, height=48, stride=192, fmt=1, handle=(7, 0)):
    # FRM1: magic | width | height | stride | format | handle_ns |
    #       handle_lo | flags  ==  4 + 7*4 = 32 bytes (§6)
    return struct.pack('<4s7I', b'FRM1', width, height, stride, fmt,
                       handle[0], handle[1], 0)


@pytest.fixture
def ring_factory():
    return build_ring
