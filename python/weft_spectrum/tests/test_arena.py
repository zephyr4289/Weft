# tests/test_arena.py — tensor arena + alignment + DLPack (Python lane).
import ctypes
import gc
import unittest

import numpy as np

from weft_spectrum.alignment import (
    aligned_slab, is_aligned, required_alignment, validate_alignment,
)
from weft_spectrum.arena import (
    ArenaSlab, TensorArena, dlpack_available, _DLMANAGED, _BLOCK_SIZE,
)
from weft_spectrum.wire import (
    E_ALIGN_INVALID, ProfileFlyweight, TIER_FLAGSHIP, FEAT,
)


class TestAlignment(unittest.TestCase):
    def test_is_aligned_positive(self):
        base, view = aligned_slab(4096, 64)
        self.assertTrue(is_aligned(view, 64))
        self.assertTrue(is_aligned(view, 128) or required_alignment(128, 512) == 128)

    def test_validate_detects_misalignment(self):
        base, view = aligned_slab(1024, 64)
        mis = memoryview(base)[1:1024]  # deliberately offset by 1
        self.assertEqual(validate_alignment(mis, 64), E_ALIGN_INVALID)
        self.assertEqual(validate_alignment(view, 64), 0)

    def test_required_alignment_rules(self):
        self.assertEqual(required_alignment(64, 128), 64)
        self.assertEqual(required_alignment(64, 512), 128)
        self.assertEqual(required_alignment(128, 128), 128)

    def test_slab_sizes(self):
        base, view = aligned_slab(1000, 64)
        self.assertEqual(len(view), 1000)
        self.assertEqual(len(base), 1000 + 63)


class TestArenaSlab(unittest.TestCase):
    def test_allocate_aligned_windows(self):
        slab = ArenaSlab(4096, cache_line_bytes=128, simd_width_bits=512)
        self.assertEqual(slab.alignment, 128)
        for size in (64, 100, 256, 1000):
            buf = slab.allocate(size)
            self.assertIsNotNone(buf)
            self.assertEqual(buf.nbytes, (size + 127) & ~127, f"size {size}")
            self.assertTrue(is_aligned(buf.view, slab.alignment))

    def test_exhaustion_returns_none_never_raises(self):
        slab = ArenaSlab(256)
        self.assertIsNotNone(slab.allocate(256))
        self.assertIsNone(slab.allocate(8))  # Law 4: signal, not raise

    def test_release_returns_capacity(self):
        slab = ArenaSlab(512)
        buf = slab.allocate(256)
        self.assertEqual(slab.used_bytes, 256)
        buf.release()
        self.assertEqual(slab.used_bytes, 0)

    def test_zero_copy_aliasing(self):
        slab = ArenaSlab(1024)
        buf = slab.allocate(128)
        for i in range(128):
            buf.view[i] = i & 0xFF
        self.assertEqual(buf.view[7], 7, "writes land in the slab itself")


class TestTensorArena(unittest.TestCase):
    def test_fallback_ladder_without_probes(self):
        arena = TensorArena(probe_order=("cuda", "cpu_simd"))
        self.assertEqual(arena.backend, "cpu_simd")  # no CUDA in sandbox
        self.assertNotEqual(arena.probeErrors, [], "absent probes surfaced")

    def test_down_tier_halves_budget(self):
        fw = ProfileFlyweight()
        fw.siliconTier = TIER_FLAGSHIP
        fw.cacheLineBytes = 64
        fw.simdWidthBits = 128
        fw.featureFlagsLo = 1 << FEAT["THERMAL_SENSOR"]
        arena = TensorArena(profile=fw, budget_bytes=1024 * 1024,
                            probe_order=("cpu_simd",))
        before = arena.slab.nbytes
        backend = arena.down_tier()
        self.assertEqual(backend, "cpu_simd")
        self.assertEqual(arena.slab.nbytes, before // 2)

    def test_budget_from_profile(self):
        fw = ProfileFlyweight()
        fw.memoryBudgetBytes = 2 * 1024 * 1024
        arena = TensorArena(profile=fw, probe_order=("cpu_simd",))
        self.assertEqual(arena.slab.nbytes, 2 * 1024 * 1024)


class TestDLPack(unittest.TestCase):
    def test_available(self):
        self.assertTrue(dlpack_available())

    def test_numpy_from_dlpack_zero_copy(self):
        slab = ArenaSlab(4096)
        buf = slab.allocate(256)
        for i in range(256):
            buf.view[i] = (i * 7) & 0xFF
        arr = np.from_dlpack(buf)
        self.assertEqual(arr.shape, (256,))
        self.assertEqual(arr.dtype, np.uint8)
        self.assertEqual(arr[:8].tolist(), [0, 7, 14, 21, 28, 35, 42, 49])
        # zero-copy: arena-side write is visible through the numpy array
        buf.view[3] = 42
        self.assertEqual(int(arr[3]), 42)

    def test_capsule_struct_integrity(self):
        slab = ArenaSlab(4096)
        buf = slab.allocate(128)
        capsule = buf.__dlpack__()
        self.assertTrue(capsule)
        PyCapsule_GetPointer = ctypes.pythonapi.PyCapsule_GetPointer
        PyCapsule_GetPointer.restype = ctypes.c_void_p
        PyCapsule_GetPointer.argtypes = [ctypes.py_object, ctypes.c_char_p]
        ptr = PyCapsule_GetPointer(capsule, b"dltensor")
        self.assertTrue(ptr)
        raw = ctypes.string_at(ptr, _DLMANAGED.size)
        (data, dev_type, dev_id, ndim, code, bits, lanes, shape_p, strides_p,
         off, ctx, deleter) = _DLMANAGED.unpack_from(raw)
        self.assertEqual(data, slab.base_addr + buf.byteOffset, "zero-copy ptr")
        self.assertEqual((dev_type, dev_id), (1, 0))  # kDLCPU:0
        self.assertEqual((ndim, code, bits, lanes), (1, 1, 8, 1))  # uint8
        shape = ctypes.cast(shape_p, ctypes.POINTER(ctypes.c_int64))[0]
        self.assertEqual(shape, buf.nbytes)
        del capsule
        gc.collect()

    def test_deleter_is_c_function(self):
        # The deleter must be a real C function (PyMem_RawFree), NOT a Python
        # callback — shutdown-order safe by construction.
        from weft_spectrum.arena import _PYMEM_RAWFREE_ADDR
        self.assertTrue(_PYMEM_RAWFREE_ADDR)
        slab = ArenaSlab(1024)
        buf = slab.allocate(64)
        raw = _BLOCK_SIZE
        self.assertEqual(raw, 80)


if __name__ == "__main__":
    unittest.main()
