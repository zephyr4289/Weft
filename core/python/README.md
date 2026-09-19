# weft (Python bindings)

Python bindings for the **Weft** lock-free streaming ring — the frozen C
core (`core/c`) compiled into a cffi extension, wrapped in a zero-copy,
GIL-releasing pythonic API. Data-science / ML users get `pip install weft`
access to the same protocol every other port speaks (issue #18-6).

## Install (from the repo)

```sh
pip install core/python          # or: cd core/python && pip install .
python -c "import weft; print(weft.active_copy_impl())"
```

Requires a C compiler at build time (the extension compiles
`weft.c`, `fanout.c`, `fanout_simd.c`, `fanout_batch.c` — the exact sources
the JNI/Dart bindings use). Optional: `pip install weft[numpy]`.

## The kernel (Triad: 1 writer, 1 reader, 3 buffers, one atomic)

```python
from weft import Weft

w = Weft(payload_max=4096)
w.publish(seq=1, payload=b"frame bytes")   # returns False only if revoked
seq = w.claim()                            # freshest complete frame's seq
payload = w.payload()                      # zero-copy memoryview
```

## The fan-out ring (RFC-0004: 1 writer, N readers, M slots)

```python
from weft import Fanout, FanoutReader

f = Fanout(payload_bytes=1024, slot_count=4)
r1 = FanoutReader(f)          # + r2, r3 ... every consumer independent

f.publish(b"...")             # single frame
f.publish_batch([b"f1", b"f2", ..., b"f100"])   # ONE publication flip (#17-3)

fresh, seq, dropped = r1.claim()
view = r1.view()              # zero-copy memoryview of the slot
```

## NumPy, zero-copy both ways

```python
import numpy as np
arr = np.arange(256, dtype=np.uint32)
f.publish(arr)                        # publish FROM an ndarray (no copy into Python)
fresh, seq, _ = r.claim()
got = np.frombuffer(r.view(), dtype=np.uint32)   # ALIASES the C buffer
```

## Concurrency

cffi releases the GIL around every C call, so reader THREADS are real
threads — the PL6 torture (1 writer + 3 readers, 20k frames, byte-exact
claims) is part of the battery, not an aspiration.

## Battery

`python3 test_weft.py` — PL-series, stdlib `unittest` only:
PL1 kernel roundtrip + I6 revoke/reclaim · PL2 L-series analogs (tear-free,
steps budgets, telescoping) · PL3 fan-out roundtrip/drops/raw-ring ·
PL4 100-frame batch + atomic refusal · PL5 numpy zero-copy aliasing ·
PL6 concurrent torture · PL7 RSS stability over 100k frames.

Honesty notes: PyPI publication (`pip install weft` from the index) is a
maintainer step (credentials) — the packaging (`pyproject.toml`) is ready
and CI-gated; the SIMD claim-copy dispatcher reports its active
implementation (`active_copy_impl()`) so evidence lines stay honest.
