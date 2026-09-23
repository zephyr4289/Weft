#!/usr/bin/env python3
"""make_ring_fixture.py — deterministic WTR1 (Weft Tensor Ring v1) fixture generator.

Produces the committed golden fixtures described in docs/weft-tensor/LAYOUT-V1.md §7.
Both the TypeScript and Python suites read these exact bytes; the generator is
pure-deterministic (dyadic floats only, no RNG, no wall clock) so re-running it
must be byte-identical (checked by CI's codegen determinism discipline).

Usage: python3 scripts/make_ring_fixture.py [--out-dir fixtures/weft-tensor] [--verify]
"""
import argparse
import math
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

RING_MAGIC = b"WEFT"
SLOT_MAGIC = b"WFRM"
LAYOUT_VERSION = 1
HEADER_SIZE = 128
SLOT_HEADER_SIZE = 64

DLPACK_INT, DLPACK_UINT, DLPACK_FLOAT = 0, 1, 2

NUMPY_DTYPE = {0: "i", 1: "u", 2: "f"}


def align64(n: int) -> int:
    return (n + 63) & ~63


def crc16_static(header: bytearray) -> int:
    """CRC-32/IEEE over the static config: bytes [0,96) ++ [104,112)."""
    return zlib.crc32(bytes(header[0:96]) + bytes(header[104:112])) & 0xFFFFFFFF


def build_ring(dtype_code: int, dtype_bits: int, shape: list, slot_count: int,
               frames: int, payload_fn) -> bytes:
    assert len(shape) <= 8
    elem_size = dtype_bits // 8
    payload_cap = math.prod(shape) * elem_size
    slot_stride = align64(SLOT_HEADER_SIZE + payload_cap)

    # Element strides, row-major, in elements (DLPack convention).
    strides = [0] * 8
    acc = 1
    for d in range(len(shape) - 1, -1, -1):
        strides[d] = acc
        acc *= shape[d]

    h = bytearray(HEADER_SIZE)
    h[0:4] = RING_MAGIC
    struct.pack_into("<H", h, 4, LAYOUT_VERSION)
    struct.pack_into("<H", h, 6, HEADER_SIZE)
    struct.pack_into("<I", h, 8, slot_count)
    struct.pack_into("<I", h, 12, slot_stride)
    struct.pack_into("<B", h, 16, dtype_code)
    struct.pack_into("<B", h, 17, dtype_bits)
    struct.pack_into("<H", h, 18, 1)  # lanes
    struct.pack_into("<I", h, 20, elem_size)
    for d, s in enumerate(shape):
        struct.pack_into("<I", h, 24 + 4 * d, s)
    for d, s in enumerate(strides):
        struct.pack_into("<I", h, 56 + 4 * d, s)
    struct.pack_into("<Q", h, 88, 0xA11CE00000000001)  # schema_id (fixture)
    struct.pack_into("<I", h, 104, 120)                # tick_hz
    struct.pack_into("<I", h, 108, 0x3)                # flags: LE | shared
    struct.pack_into("<I", h, 112, crc16_static(h))

    buf = bytearray(HEADER_SIZE + slot_count * slot_stride)
    buf[0:HEADER_SIZE] = h

    fmt = {8: "<B", 16: "<H", 32: "<f", 64: "<d"} if dtype_code == DLPACK_FLOAT else \
          {8: "<B", 16: "<H", 32: "<i", 64: "<q"} if dtype_code == DLPACK_INT else \
          {8: "<B", 16: "<H", 32: "<I", 64: "<Q"}

    n_elem = math.prod(shape)
    for seq in range(1, frames + 1):
        slot = (seq - 1) % slot_count
        base = HEADER_SIZE + slot * slot_stride
        vals = payload_fn(seq, n_elem)
        payload = bytearray(payload_cap)
        for i, v in enumerate(vals):
            struct.pack_into(fmt[dtype_bits], payload, i * elem_size, v)

        sh = bytearray(SLOT_HEADER_SIZE)
        sh[0:4] = SLOT_MAGIC
        struct.pack_into("<I", sh, 4, payload_cap)
        struct.pack_into("<Q", sh, 8, seq)
        struct.pack_into("<Q", sh, 16, seq * 1_000_000)     # timestamp_ns
        struct.pack_into("<I", sh, 24, 8333)                 # duration_us (120fps)
        struct.pack_into("<I", sh, 32, 0)                    # flags: committed bit set last
        fourcc = b"F32 " if dtype_code == DLPACK_FLOAT else b"RAW "
        sh[32:36] = fourcc
        struct.pack_into("<B", sh, 36, len(shape))
        struct.pack_into("<B", sh, 37, 1)
        struct.pack_into("<I", sh, 40, 0)                    # plane_offset[0]
        struct.pack_into("<I", sh, 52, payload_cap)          # plane_size[0]

        buf[base:base + SLOT_HEADER_SIZE] = sh
        buf[base + SLOT_HEADER_SIZE: base + SLOT_HEADER_SIZE + payload_cap] = payload
        # Commit marker: flags bit0 = 1
        struct.pack_into("<I", buf, base + 28, 1)
        # Publish: producer_seq = seq (LE u64 at ring offset 96)
        struct.pack_into("<Q", buf, 96, seq)

    return bytes(buf)


def f32_fixture(seq: int, n: int):
    # Dyadic only: k*0.25 is exactly representable.
    return [(seq * 10 + i) * 0.25 for i in range(n)]


def u8_fixture(seq: int, n: int):
    return [(seq * 37 + i * 11) & 0xFF for i in range(n)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=str(REPO / "fixtures/weft-tensor"))
    ap.add_argument("--verify", action="store_true",
                    help="verify existing files are byte-identical instead of writing")
    args = ap.parse_args()

    f32 = build_ring(DLPACK_FLOAT, 32, [2, 3], 4, 10, f32_fixture)
    u8 = build_ring(DLPACK_UINT, 8, [2, 2, 4], 4, 6, u8_fixture)

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    targets = {
        out / "ring-v1-f32.bin": f32,
        out / "ring-v1-u8.bin": u8,
    }
    if args.verify:
        ok = True
        for path, want in targets.items():
            got = path.read_bytes()
            if got != want:
                print(f"DRIFT: {path} differs from deterministic generator", file=sys.stderr)
                ok = False
            else:
                print(f"OK {path} ({len(want)} B) byte-identical")
        return 0 if ok else 1

    for path, data in targets.items():
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
