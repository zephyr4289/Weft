"""view.py — WeftTensorView: reused flyweight over one committed slot.

Implements the DLPack producer protocol (`__dlpack__` / `__dlpack_device__`),
the array interface (`__array_interface__`) and `as_numpy()` — all sharing
THE SAME ring memory (zero-copy, Law 1/2). The lead's canonical bridge:

    import torch, weft_tensor as wt
    view = ring.acquire_latest()
    t = torch.from_dlpack(view)   # zero-copy straight from Weft shared memory
"""
from __future__ import annotations

from typing import Optional

from .layout import SLOT_HEADER_SIZE, fourcc_to_str
from .dlpack_capi import build_capsule, DLCPU


class WeftTensorView:
    """One instance per ring, re-bound per acquire (Pillar 1 flyweight contract)."""

    def __init__(self, ring):
        self._ring = ring
        self._slot_base = -1
        self._slot = -1
        self.seq = 0
        self.seq_lo = 0
        self.seq_hi = 0
        self.payload_len = 0
        self.timestamp_ns = 0
        self.duration_us = 0
        self.flags = 0
        self.fourcc = "RAW "
        self.rank = 0
        self.planes = 1

    def _bind(self, slot_base: int, slot: int, meta: dict) -> None:
        self._slot_base = slot_base
        self._slot = slot
        self.seq = meta["seq"]
        self.seq_lo = meta["seq_lo"]
        self.seq_hi = meta["seq_hi"]
        self.payload_len = meta["payload_len"]
        self.timestamp_ns = meta["timestamp_ns"]
        self.duration_us = meta["duration_us"]
        self.flags = meta["flags"]
        self.fourcc = fourcc_to_str(meta["fourcc"])
        self.rank = meta["rank"]
        self.planes = meta["planes"]

    @property
    def ring(self) -> "WeftRing":
        return self._ring

    @property
    def payload_byte_offset(self) -> int:
        return self._slot_base + SLOT_HEADER_SIZE

    @property
    def shape(self) -> tuple:
        L = self._ring.layout
        return tuple(int(L.shape[d]) for d in range(L.rank))

    @property
    def strides(self) -> tuple:
        """Element strides (DLPack convention) — the RING's static layout."""
        L = self._ring.layout
        return tuple(int(L.strides[d]) for d in range(L.rank))

    @property
    def dtype(self) -> tuple:
        L = self._ring.layout
        return (L.dtype_code, L.dtype_bits)

    @property
    def schema_id(self) -> int:
        return self._ring.layout.schema_id

    @property
    def memory(self) -> memoryview:
        """Precreated memoryview over this slot's payload capacity (zero-alloc)."""
        return self._ring._slot_mv[self._slot]

    # -- zero-copy NumPy --------------------------------------------------------

    def as_numpy(self):
        """Direct strided ndarray view over the ring slot — SHARED memory alias.

        The SAME precreated ndarray is returned every acquire for a given slot
        (zero allocation); do not retain it across ring wraps. Read-only when
        the underlying buffer is read-only.
        """
        return self._ring._slot_ndarray_for(self._slot)

    def __array_interface__(self):  # pragma: no cover - exercised via np.asarray
        arr = self.as_numpy()
        return arr.__array_interface__

    @property
    def __array_interface__(self):  # noqa: D105 — numpy protocol
        arr = self.as_numpy()
        return arr.__array_interface__

    # -- DLPack -------------------------------------------------------------------

    def __dlpack_device__(self):
        return (DLCPU, 0)

    def __dlpack__(self, *, stream: Optional[int] = None, max_version=None,
                   dl_device=None, copy=None, **kwargs):
        """Produce a "dltensor" PyCapsule over the slot payload.

        - stream: ignored on CPU (consumers pass None on kDLCPU).
        - max_version/dl_device/copy: accepted per the 1.0 protocol; V1 always
          serves the legacy unversioned capsule over existing CPU memory
          (copy semantics are the CONSUMER's choice; we never copy).
        - The capsule's deleter frees the DLPack scratch and drops the ring
          reference when the consumer releases the tensor.
        """
        if dl_device is not None and tuple(dl_device) != (DLCPU, 0):
            raise BufferError("weft_tensor V1 serves kDLCPU only")
        if copy:
            raise BufferError("copy=True not supported by weft_tensor V1 (zero-copy by law)")
        L = self._ring.layout
        # Strides: serve the ring's real strides; NULL (C-contiguous) when contiguous.
        contiguous = all(
            L.strides[d] == _c_contig_stride(L.shape, L.strides, d, L.rank)
            for d in range(L.rank)
        )
        return build_capsule(
            data_address=self._ring._slot_addr_for(self._slot),
            ndim=L.rank,
            dtype_code=L.dtype_code,
            dtype_bits=L.dtype_bits,
            shape=L.shape,
            strides_elems=None if contiguous else tuple(
                int(L.strides[d]) for d in range(L.rank)),
            elem_size=L.elem_size,
            owner=self._ring,
        )

    # -- misc -----------------------------------------------------------------

    def describe(self) -> str:
        return (f"WeftTensorView(seq={self.seq}, ts={self.timestamp_ns}ns, "
                f"len={self.payload_len}B, fourcc={self.fourcc!r}, "
                f"shape={self.shape}, dtype={self.dtype})")


def _c_contig_stride(shape, strides, d, rank) -> int:
    """Expected row-major stride (elements) for dim d."""
    acc = 1
    for k in range(d + 1, rank):
        acc *= shape[k]
    return acc
