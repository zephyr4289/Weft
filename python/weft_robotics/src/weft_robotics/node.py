# node.py — RoboticsNode: rmw_weft managed shim (Python side).
#
# Engineer 2 ships the native `rmw_weft` RMW (shared-memory transport).
# This module is the MANAGED integration surface over it:
#
#   node = RoboticsNode()
#   node.attach_ring(ring_bytes)                     # RNG1 attach
#   sub = node.subscribe_zerocopy('lidar', 'points_f32')
#   for record in sub:
#       mv, n = record.points_view()                 # 0-copy
#       np.frombuffer(mv, dtype='<f4').reshape(n, 3) # SAME memory
#
# subscribe_zerocopy NEVER copies, blocks or allocates per record (the
# generator reuses the reader's single RecordView; numpy/polars/torch
# consumers share the ring bytes directly).

from .ring import (FMT_NAMES, Rng1Error, RingReader,
                   FMT_IMU6DOF, FMT_POINTS_F32, FMT_FRAME_DESC, FMT_BOXES_F32)

_NAME_TO_FMT = {v: k for k, v in FMT_NAMES.items()}
_NAME_TO_FMT.update({'imu': FMT_IMU6DOF, 'points': FMT_POINTS_F32,
                     'frame': FMT_FRAME_DESC, 'boxes': FMT_BOXES_F32})


def _resolve_type(topic_type):
    if topic_type in (None, 'any'):
        return None
    if isinstance(topic_type, int):
        return topic_type
    t = str(topic_type).lower()
    if t in _NAME_TO_FMT:
        return _NAME_TO_FMT[t]
    raise Rng1Error(-1)  # unknown managed type name (fail-closed)


class _ZeroCopySubscription:
    __slots__ = ('_reader', '_topic', '_max')

    def __init__(self, reader, topic, max_records):
        self._reader = reader
        self._topic = topic
        self._max = max_records

    def __iter__(self):
        return self

    def __next__(self):
        rec = self._reader.acquire(topic_id=self._topic)
        if rec is None:
            raise StopIteration
        return rec


class RoboticsNode:
    """Managed node: attach the RNG1 ring, subscribe zero-copy.

    Topics are numeric topic_ids on the wire (RNG1 §5 topic table).
    A name may be bound via bind_topic(name, topic_id) for ergonomics;
    an UNBOUND string topic fails closed (Rng1Error -3).
    """

    def __init__(self):
        self._reader = None
        self._camera = None
        self._topics = {}

    @property
    def attached(self):
        return self._reader is not None

    def attach_ring(self, buffer):
        """Attach an RNG1 ring (bytes / bytearray / mmap). Fail-closed on
        the header decision order (short -> magic -> version -> geometry)."""
        self._reader = RingReader(buffer)
        return self

    def attach_camera(self, camera_source):
        """Bind a CameraSource (its FRM1 frames ride the same ring as
        FRAME_DESC records; pixels stay in the arena, never in the ring)."""
        self._camera = camera_source
        return self

    def bind_topic(self, name, topic_id):
        """Bind a topic NAME to its numeric RNG1 topic_id."""
        self._topics[str(name)] = int(topic_id)
        return self

    def topic_ids(self):
        self._require()
        return self._reader.topic_table()

    def subscribe_zerocopy(self, topic, topic_type=None, max_records=None):
        """Zero-copy subscription. Yields the newest continuous record per
        iteration pass; the RecordView is REUSED and only valid within the
        iteration step (documented lifetime).

        topic: int topic_id, or a bound topic NAME.
        topic_type: 'imu6dof' | 'points_f32' | 'frame_desc' |
                    'boxes_f32' | int fmt code | None/'any'.
        """
        self._require()
        fmt = _resolve_type(topic_type)
        if isinstance(topic, str):
            if topic not in self._topics:
                raise Rng1Error(-3)  # unbound topic name (fail-closed)
            topic = self._topics[topic]
        elif topic is not None:
            topic = int(topic)
        return _ZeroCopySubscription(self._reader, topic, max_records)

    def stats(self):
        self._require()
        r = self._reader
        return {
            'acquires': r.acquires,
            'torn': r.torn,
            'skipped': r.skipped,
            'filtered': r.filtered,
            'committed_seq': r.committed_seq(),
            'header_drops': r.header_drop_count(),
        }

    def close(self):
        if self._reader is not None:
            self._reader.close()
            self._reader = None

    def _require(self):
        if self._reader is None:
            raise Rng1Error(-2)  # attach_ring() required first (fail-closed)
