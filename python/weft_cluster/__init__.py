# __init__.py — python/weft_cluster: managed Python cluster SDK.
#
#   from weft_cluster import ClusterClient
#   import numpy as np, torch
#
#   client = ClusterClient(node_id=2)
#   client.start()
#   client.connect_peer("127.0.0.1", 41001)
#   for frame in client.subscribe("telemetry"):
#       tensor = torch.from_dlpack(frame)          # zero copy (DLPack)
#       arr = np.frombuffer(frame.payload, dtype=np.uint8)  # zero copy

from .errors import (WC_OK, WeftClusterError, wc_name)  # noqa: F401
from .wire import (WCN1_HEADER_SIZE, WGS1_HEADER_SIZE, topic_hash64,  # noqa: F401
                   fnv1a64, crc32_ieee, parse_wcn1, encode_wcn1,
                   Frame, decode_wcn1_into, verify_frame_crc)
from .ring import ShmRing  # noqa: F401
from .client import ClusterClient, Subscription  # noqa: F401
from .topology import ShardRouter  # noqa: F401
from .streams import listen  # noqa: F401

__version__ = "0.1.0"


# --- NumPy / PyTorch stream hooks (zero-copy) ---------------------------------

def as_numpy(frame: Frame):
    """NumPy array VIEW over the frame payload — shares memory with the ring;
    no copy. The view is valid until the next frame reuses the slot."""
    import numpy as np
    return np.frombuffer(frame.payload, dtype=np.uint8)


def frame_dlpack(frame: Frame):
    """DLPack capsule view over the frame payload so
    `torch.from_dlpack(frame)` / `torch.from_dlpack(as_numpy(frame))` land on
    shared ring memory without a copy. frame must be retained while in use."""
    return as_numpy(frame)  # NumPy implements __dlpack__ (torch consumes it)


def as_torch(frame: Frame):
    """PyTorch tensor over the frame payload — zero copy via NumPy's
    __array__/buffer protocol; requires torch (imported lazily)."""
    import torch  # lazy: optional dependency
    return torch.from_numpy(as_numpy(frame))


# Give Frame a native DLPack surface so the lead's canonical snippet works:
#   tensor_view = wt.acquire_latest_frame(ring)
#   torch_tensor = torch.from_dlpack(tensor_view)
def _frame_dlpack__(self, stream=None):
    return as_numpy(self).__dlpack__(stream=stream)


def _frame_dlpack_device__(self, stream=None):
    return as_numpy(self).__dlpack_device__()


Frame.__dlpack__ = _frame_dlpack__
Frame.__dlpack_device__ = _frame_dlpack_device__
