"""crosslang_produce.py — Python producer: write a deterministic WTR1 ring to
argv[1] (same canonical pattern as crosslang_produce.mjs — consumed by the
TypeScript suite in CI shard stage 5)."""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 2)[0])  # python/ package root

from weft_tensor import WeftRing, DLPACK_FLOAT  # noqa: E402

out = sys.argv[1] if len(sys.argv) > 1 else None
if not out:
    print("usage: python3 crosslang_produce.py <out-path>", file=sys.stderr)
    sys.exit(2)

ring = WeftRing.create(slot_count=4, payload_cap=24, dtype_code=DLPACK_FLOAT,
                       dtype_bits=32, shape=[2, 3], schema_id=0xFE77000000000001,
                       tick_hz=120, fourcc="F32 ")
import numpy as np  # noqa: E402

src = np.zeros(6, dtype=np.float32)
for seq in range(1, 11):
    for i in range(6):
        src[i] = (seq * 10 + i) * 0.25
    ring.commit(src.tobytes(), ts=seq * 1_000_000)

with open(out, "wb") as f:
    f.write(ring._mv)

import json  # noqa: E402

print(json.dumps({
    "producer": "python", "path": out, "byteLength": ring.byte_length,
    "producerSeq": ring.producer_seq, "slotStride": ring.layout.slot_stride,
    "schemaId": f"{ring.layout.schema_id:x}",
}))
