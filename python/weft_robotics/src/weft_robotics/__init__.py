# __init__.py — weft_robotics: RNG1 ring reader, rmw_weft managed shim,
# zero-copy DLPack camera frames (Pillar 6 deliverable B, Python side).

from .ring import (RingReader, RecordView, Rng1Error,
                   RNG1_MAGIC, RNG1_VERSION, RNG1_HEADER_SIZE, SLOT_HDR,
                   FMT_IMU6DOF, FMT_POINTS_F32, FMT_FRAME_DESC, FMT_BOXES_F32,
                   FMT_NAMES,
                   E_SHORT, E_MAGIC, E_VERSION, E_HEADER, E_GEOMETRY)
from .camera import (CameraSource, CameraFrame, MappedArena, Frm1View,
                     Frm1Error, FRM1_MAGIC, FRM1_SIZE,
                     FMT_RGB8, FMT_BGR8, FMT_NV12, FMT_GRAY8)
from .node import RoboticsNode

__version__ = '0.1.0'

__all__ = [
    'RingReader', 'RecordView', 'Rng1Error',
    'RNG1_MAGIC', 'RNG1_VERSION', 'RNG1_HEADER_SIZE', 'SLOT_HDR',
    'FMT_IMU6DOF', 'FMT_POINTS_F32', 'FMT_FRAME_DESC', 'FMT_BOXES_F32',
    'FMT_NAMES', 'E_SHORT', 'E_MAGIC', 'E_VERSION', 'E_HEADER', 'E_GEOMETRY',
    'CameraSource', 'CameraFrame', 'MappedArena', 'Frm1View', 'Frm1Error',
    'FRM1_MAGIC', 'FRM1_SIZE', 'FMT_RGB8', 'FMT_BGR8', 'FMT_NV12', 'FMT_GRAY8',
    'RoboticsNode',
]
