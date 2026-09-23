"""crosslang_consume.py — Python consumer: attach the ring at argv[1]
(produced by EITHER language) and validate byte-exactly; then prove the
DLPack bridge aliases the SAME memory (numpy data pointer inside the ring
buffer range). CI shard stage 6."""
import json
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 2)[0])  # python/ package root

from weft_tensor import WeftRing  # noqa: E402

path = sys.argv[1] if len(sys.argv) > 1 else None
if not path:
    print("usage: python3 crosslang_consume.py <ring-path>", file=sys.stderr)
    sys.exit(2)

data = open(path, "rb").read()
ring = WeftRing.attach(data)
assert ring.producer_seq == 10, f"producer_seq {ring.producer_seq} != 10"
assert ring.layout.schema_id == 0xFE77000000000001, "schema id mismatch"
assert ring.layout.tick_hz == 120, "tick_hz mismatch"
for seq in range(7, 11):
    v = ring.acquire_frame(seq)
    assert v is not None, f"frame {seq} missing"
    assert v.timestamp_ns == seq * 1_000_000, f"frame {seq} ts"
    assert v.fourcc == "F32 ", f"frame {seq} fourcc"
    arr = v.as_numpy()
    want = np.array([(seq * 10 + i) * 0.25 for i in range(6)],
                    dtype=np.float32).reshape(2, 3)
    assert np.array_equal(arr, want), f"frame {seq} payload"
    # DLPack alias proof over the SAME produced ring (stage 6 core check)
    t = np.from_dlpack(v)
    assert np.shares_memory(t, arr), "DLPack tensor must alias the ring slot"
    assert np.array_equal(t, want)

base_addr = arr.__array_interface__["data"][0]
buf_addr = np.frombuffer(data, dtype=np.uint8).__array_interface__["data"][0]
assert 0 <= base_addr - buf_addr < len(data), "numpy view must live inside ring memory"

print(json.dumps({
    "consumer": "python", "path": path, "framesValidated": [7, 8, 9, 10],
    "dlpackAlias": True, "ok": True,
}))
