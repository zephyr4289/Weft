# weft_spectrum/alignment.py — cache-line alignment validator (P5, mandate D).
#
# Hardware DMA engines and SIMD kernels demand 64B (NEON/grace) or 128B
# (Apple AMX / AVX-512 aggregation) aligned slabs. This module VALIDATES and
# PROVIDES aligned buffers without any native code:
#   * is_aligned(buf, align)  — pure arithmetic on the buffer address
#   * aligned_slab(size, align) — over-allocated bytearray + offset rounding;
#     the returned memoryview is guaranteed aligned (validated by tests)
#   * E_ALIGN_INVALID -> re-slab, never a crash (Law 4 taxonomy code 11)

import ctypes
from collections.abc import Buffer

E_ALIGN_INVALID = 11


def address_of(buf) -> int:
    """Data-pointer address of any PEP-688 buffer (zero-copy)."""
    return ctypes.addressof(ctypes.c_char.from_buffer(buf))


def is_aligned(buf, align=64) -> bool:
    if align <= 1:
        return True
    return (address_of(buf) % align) == 0


def validate_alignment(buf, align=64):
    """Returns 0 when aligned, else E_ALIGN_INVALID (never raises)."""
    try:
        return 0 if is_aligned(buf, align) else E_ALIGN_INVALID
    except (TypeError, ValueError):
        return E_ALIGN_INVALID


def aligned_slab(size, align=64):
    """Allocate `size` bytes at a guaranteed `align` boundary.

    Implementation: over-allocate by align-1, find the aligned offset, and
    expose a zero-copy memoryview over exactly `size` bytes. The base
    bytearray keeps the memory alive; the view is what callers use.
    """
    if size <= 0:
        raise ValueError("size must be positive")
    if align & (align - 1):
        raise ValueError("align must be a power of two")
    base = bytearray(size + align - 1)
    addr = address_of(base)
    off = (-addr) % align
    view = memoryview(base)[off:off + size]
    assert (addr + off) % align == 0  # structural guarantee; tests double-check
    return base, view


def required_alignment(cache_line_bytes, simd_width_bits):
    """Normative slab alignment: cache line, widened by SIMD vector width."""
    a = max(cache_line_bytes, 64)
    if simd_width_bits >= 512:
        a = max(a, 128)
    return a
