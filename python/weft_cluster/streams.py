# streams.py — asyncio integration: `async for tensor in cluster.listen()`.
#
# Event-loop friendly: frames arrive via the selector (loop.add_reader on the
# UDP socket), each subscription's ring decodes zero-copy, and the async
# generator yields reused Frame objects (copy out anything retained — the
# ring reuses slots, mirroring the TS Subscription contract).

import asyncio

from .errors import WC_OK


class _UdpProtocol(asyncio.DatagramProtocol):
    """Bridges socket readiness into the client's non-blocking pump()."""

    def __init__(self, client, ready: asyncio.Future):
        self.client = client
        self.ready = ready

    def connection_made(self, transport):
        self.ready.set_result(transport)

    def datagram_received(self, data, addr):
        # pump() drains everything currently queued (non-blocking).
        self.client.pump()


async def listen(client, topic: str, subscribe_kwargs: dict | None = None):
    """`async for frame in listen(client, 'telemetry')` — yields frames as
    they land, zero-copy, waking on socket readiness (no busy polling)."""
    sub = client.subscribe(topic, **(subscribe_kwargs or {}))
    loop = asyncio.get_running_loop()
    ready = loop.create_future()
    transport, _protocol = await loop.create_datagram_endpoint(
        lambda: _UdpProtocol(client, ready), sock=client.sock)
    try:
        while sub.running:
            frame = await _wait_frame(sub, loop)
            if frame is not None:
                yield frame
    finally:
        transport.close()
        sub.stop()


async def _wait_frame(sub, loop, timeout: float = 0.1):
    """Poll once now; if empty, wait for the next socket wake (or timeout)."""
    from .errors import WC_OK
    code, frame = sub.ring.try_read(sub._next_read, sub.frame)
    if code == WC_OK:
        sub._next_read += 1
        sub.delivered += 1
        return frame
    # Park until the pump runs again (datagram_received wakes the loop).
    await asyncio.sleep(0.0005)
    return None
