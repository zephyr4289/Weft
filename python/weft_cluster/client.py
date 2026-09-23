# client.py — Pythonic cluster client over UDP datagrams and shared-memory
# rings. Byte-exact with packages/weft-cluster (WCN1/WGS1 + ShmRing layout).
#
# Zero-copy contract: publish() encodes into ONE preallocated bytearray;
# subscription frames are memoryview slices over ring memory — payload views
# stay valid until the next iteration step (same handle-reuse semantics as
# the TS Subscription; copy out anything retained).
#
#   client = ClusterClient(node_id=2)
#   client.start()
#   client.connect_peer("127.0.0.1", 41001)
#   for frame in client.subscribe("telemetry"):
#       arr = np.frombuffer(frame.payload, dtype=np.uint8)  # zero copy

import os
import socket
import time

from .errors import WC_OK, WC_E_TRUNCATED, WeftClusterError
from .ring import ShmRing
from . import wire


def _now_ns() -> int:
    # CLOCK_MONOTONIC — same base across processes on the same host, so
    # one-way hop latency = recv_ns - send_ns is meaningful for the demo.
    return time.monotonic_ns()


class Subscription:
    """Async+sync iterable subscription backed by a ShmRing (zero copy)."""

    def __init__(self, client, topic: str, topic_hash: int, slot_count: int,
                 payload_max: int):
        self.client = client
        self.topic = topic
        self.topic_hash = topic_hash
        self.ring = ShmRing.create(slot_count, payload_max)
        self.frame = wire.Frame()   # reused decode target
        self._next_read = 1
        self.last_seq = 0
        self.delivered = 0
        self.stale_count = 0
        self.gap_count = 0
        self.error_count = 0
        self.running = True

    def poll(self, timeout: float = 0.05) -> wire.Frame | None:
        """One frame if available, else None after waiting up to `timeout`."""
        import select
        end = time.monotonic() + timeout
        while self.running:
            f = self._try_next()
            if f is not None:
                return f
            remaining = end - time.monotonic()
            if remaining <= 0:
                return None
            # Wake early when the datagram socket has something for us.
            sock = self.client.transport_sock
            if sock is not None:
                select.select([sock], [], [], min(remaining, 0.005))
            else:
                time.sleep(0.0005)

    def _try_next(self) -> wire.Frame | None:
        self.client.pump()
        code, frame = self.ring.try_read(self._next_read, self.frame)
        if code == WC_OK:
            # No seq-equality stale check here: the ring's commit words
            # already order frames, and pump() owns the datagram replay
            # filter (it updates last_seq BEFORE we read the slot — a check
            # against it here would flag every frame stale).
            self._next_read += 1
            self.delivered += 1
            return frame
        if code == -1:
            cur = self.ring.cursor
            if cur - self._next_read >= self.ring.slot_count:
                lost = cur - self.ring.slot_count + 2 - self._next_read
                if lost > 0:
                    self.gap_count += lost
                self._next_read = cur - self.ring.slot_count + 2
            return None
        if code == 1:  # torn
            return None
        self.error_count += 1
        self._next_read += 1
        return None

    def __iter__(self):
        return self._gen()

    def _gen(self):
        while self.running:
            f = self.poll(0.05)
            if f is not None:
                yield f

    def stop(self):
        self.running = False
        self.client._drop(self)


class ClusterClient:
    """Managed cluster endpoint (datagram mode)."""

    def __init__(self, node_id: int, data_port: int = 0, host: str = "127.0.0.1",
                 payload_max: int = 1024, crc: bool = True,
                 slot_count: int = 256):
        self.node_id = node_id
        self.host = host
        self.data_port = data_port
        self.payload_max = payload_max
        self.crc_enabled = crc
        self.slot_count = slot_count
        self.sock = None
        self.transport_sock = None  # alias used by Subscription.poll
        self.topics = {}            # name -> topic_hash
        self.seq_by_topic = {}      # name -> u64
        self.subs = {}              # topic_hash -> [Subscription]
        self.peers = []             # (addr, port) for data + control
        self.subscribed_hashes = set()
        self.published = 0
        self.rx_count = 0
        self.decode_errors = 0
        # One staging bytearray (Law 1: encode in place, never reallocate).
        self._staging = bytearray(64 + payload_max)
        self._staging_view = memoryview(self._staging)
        self._frame = wire.Frame()
        self._rxbuf = bytearray(65536)

    # -- lifecycle ------------------------------------------------------------

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
        self.sock.bind((self.host, self.data_port))
        self.data_port = self.sock.getsockname()[1]
        self.sock.setblocking(False)
        self.transport_sock = self.sock

    def stop(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None
            self.transport_sock = None

    def connect_peer(self, addr: str, port: int):
        self.peers.append((addr, port))
        # Re-announce active subscriptions to the new peer.
        for h in self.subscribed_hashes:
            self._send_control(h, wire.CTL_SUB, addr, port)

    # -- hot path ---------------------------------------------------------------

    def _topic(self, topic: str) -> int:
        h = self.topics.get(topic)
        if h is None:
            h = wire.topic_hash64(topic)
            self.topics[topic] = h
        return h

    def publish(self, topic: str, payload) -> int:
        """Encode + send one WCN1 datagram to every subscribed peer.
        Returns the frame seq. Hot path: no per-frame allocation beyond the
        socket send itself (the OS copies the staging bytes into the kernel)."""
        th = self._topic(topic)
        seq = (self.seq_by_topic.get(topic, 0) + 1) & 0xFFFFFFFFFFFFFFFF
        self.seq_by_topic[topic] = seq
        flags = wire.WC_FLAG_INLINE_PAYLOAD
        crc = 0
        if self.crc_enabled:
            flags |= wire.WC_FLAG_CRC_PRESENT
            crc = wire.crc32_ieee(payload)
        head = wire._WCN1.pack(wire.WCN1_MAGIC, wire.WCN1_VERSION,
                               wire.WCN1_HEADER_SIZE, flags, self.node_id, th,
                               seq, _now_ns(), len(payload), 1, crc, 0, 0)
        staging = self._staging
        staging[0:64] = head
        staging[64:64 + len(payload)] = payload
        total = 64 + len(payload)
        for (addr, port) in self.peers:
            self.sock.sendto(memoryview(staging)[:total], (addr, port))
        self.published += 1
        return seq

    def subscribe(self, topic: str, slot_count: int | None = None,
                  payload_max: int | None = None) -> Subscription:
        th = self._topic(topic)
        sub = Subscription(self, topic, th, slot_count or self.slot_count,
                           payload_max or self.payload_max)
        self.subs.setdefault(th, []).append(sub)
        self.subscribed_hashes.add(th)
        for (addr, port) in self.peers:
            self._send_control(th, wire.CTL_SUB, addr, port)
        return sub

    def _drop(self, sub: Subscription):
        subs = self.subs.get(sub.topic_hash)
        if subs and sub in subs:
            subs.remove(sub)

    # -- rx pump ------------------------------------------------------------------

    def pump(self) -> int:
        """Drain the socket: control frames update routing, data frames are
        copied once into each matching subscription's ring. Returns frames."""
        n = 0
        if self.sock is None:
            return 0
        buf = self._rxbuf
        while True:
            try:
                data, addr = self.sock.recvfrom(len(buf))
            except (BlockingIOError, InterruptedError):
                return n
            self.rx_count += 1
            n += 1
            mv = memoryview(bytes(data))  # socket -> one owned buffer
            frame = self._frame
            code = wire.decode_wcn1_into(mv, 0, frame)
            if code != WC_OK:
                self.decode_errors += 1
                continue
            if frame.flags & wire.WC_FLAG_CONTROL:
                ctl = mv[frame.header_size]
                self._apply_control(ctl, frame.topic_hash, addr)
                continue
            subs = self.subs.get(frame.topic_hash)
            if not subs:
                continue
            for sub in subs:
                if (frame.seq <= sub.last_seq and
                        sub.last_seq - frame.seq < 0x8000000000000000):
                    sub.stale_count += 1
                    continue
                sub.last_seq = frame.seq
                n = sub.ring._next_write + 1
                sub.ring._next_write = n
                sub.ring.publish(n, mv, 0, frame.payload_len + 64)
                sub.delivered += 1

    def _apply_control(self, ctl: int, topic_hash: int, addr):
        # Python clients as data sources: remote SUB adds the peer for fan-out.
        peer = (addr[0], addr[1])
        if ctl == wire.CTL_SUB and peer not in self.peers:
            self.peers.append(peer)
            self.subscribed_hashes.add(topic_hash)  # remember interest map

    def _send_control(self, topic_hash: int, ctl: int, addr: str, port: int):
        datagram = bytearray(65)
        wire._WCN1.pack_into(datagram, 0, wire.WCN1_MAGIC, wire.WCN1_VERSION,
                             wire.WCN1_HEADER_SIZE, wire.WC_FLAG_CONTROL,
                             self.node_id, topic_hash, 0, 0, 1, 0, 0, 0, 0)
        datagram[64] = ctl
        self.sock.sendto(bytes(datagram), (addr, port))

    # -- shared-memory attach ------------------------------------------------------

    @staticmethod
    def acquire_latest_frame(ring: ShmRing) -> wire.Frame:
        """Read the newest committed frame from an attached ring into the
        ring's reusable Frame. Raises WeftClusterError on torn/unknown state
        only after retries; returns None when the ring is empty."""
        f = ring.latest()
        if f is None:
            raise WeftClusterError(WC_E_TRUNCATED, "ring is empty")
        return f
