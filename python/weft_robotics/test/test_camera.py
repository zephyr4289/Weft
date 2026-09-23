# test_camera.py — Stage 6 acceptance: DLPack/NumPy ZERO-COPY pointer
# identity over the arena slot + FRM1 descriptor fail-closed matrix.

import struct

import pytest

from weft_robotics import (CameraSource, Frm1View, Frm1Error, FRM1_MAGIC,
                           FMT_RGB8, FMT_GRAY8)

np = pytest.importorskip('numpy')


def test_frm1_fail_closed():
    with pytest.raises(Frm1Error) as e:
        Frm1View(b'FRM')
    assert e.value.code == 1
    with pytest.raises(Frm1Error) as e:
        Frm1View(b'XRM1' + bytes(28))
    assert e.value.code == 2


def test_frm1_decode():
    payload = struct.pack('<4s7I', b'FRM1', 3840, 2160, 11520, FMT_RGB8,
                          7, 0, 0)
    v = Frm1View(payload)
    assert v.width == 3840 and v.height == 2160 and v.stride == 11520
    assert v.format == FMT_RGB8 and v.handle == (7, 0)


def test_numpy_pointer_identity():
    """np.frombuffer over the slot window MUST alias the arena slot."""
    cam = CameraSource(width=64, height=48, fmt=FMT_GRAY8)
    frame, window = cam.grab_into_slot()
    window[0:4] = b'\xDE\xAD\xBE\xEF'
    arr = frame.get_numpy()
    assert arr.shape == (48, 64)
    assert arr[0, 0] == 0xDE and arr[0, 3] == 0xEF
    slot_addr = frame.address()
    window_addr = arr.ctypes.data
    assert window_addr == slot_addr, (
        f'numpy addr {window_addr:#x} != slot addr {slot_addr:#x}')
    cam.close()


def test_dlpack_pointer_identity():
    """Stage 6 mandate: the DLPack capsule's data pointer IS the arena
    slot address — np.from_dlpack consumes it with ZERO copies."""
    cam = CameraSource(width=64, height=48, fmt=FMT_GRAY8)
    frame, window = cam.grab_into_slot()
    window[0:8] = bytes(range(8))
    slot_addr = frame.address()

    tensor = np.from_dlpack(frame)  # frame implements __dlpack__
    assert tensor.dtype == np.uint8 and tensor.shape == (48, 64)
    assert tensor[0, 0] == 0 and tensor[0, 7] == 7
    assert tensor.ctypes.data == slot_addr, (
        f'DLPack addr {tensor.ctypes.data:#x} != slot addr {slot_addr:#x} '
        '(Stage 6 pointer-identity mandate)')

    # mutate the arena window -> the tensor reads it back (SAME memory;
    # numpy's DLPack view is read-only by protocol, identity is the proof)
    window[64] = 0xAB
    assert tensor[1, 0] == 0xAB
    cam.close()


def test_dlpack_2d_geometry():
    cam = CameraSource(width=32, height=16, fmt=FMT_GRAY8)
    frame, _ = cam.grab_into_slot()
    tensor = np.from_dlpack(frame)
    assert tensor.shape == (16, 32)
    strides = tensor.strides
    assert strides[0] == 32 and strides[1] == 1  # packed uint8
    cam.close()


def test_drop_oldest_slot_recycle():
    cam = CameraSource(width=64, height=48, fmt=FMT_GRAY8, slots=2)
    f1, _ = cam.grab_into_slot()
    f2, _ = cam.grab_into_slot()
    f3, _ = cam.grab_into_slot()  # drops f1 (drop-not-block): slot recycles
    assert f3.seq == 3
    assert f3.byteOffset == f1.byteOffset  # recycled slot
    assert f2.byteOffset != f3.byteOffset
    assert len(cam.arena._live) == 2
    cam.close()
