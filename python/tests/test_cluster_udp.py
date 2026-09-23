# test_cluster_udp.py — live cross-language cluster over localhost UDP.
#   Stage A: TS producer -> Python consumer (SUB control plane + data).
#   Stage B: Python producer -> TS consumer.
# Both directions must see monotonic sequences and intact payloads.
import json
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from weft_cluster import ClusterClient, wire  # noqa: E402

PKG = Path(__file__).resolve().parents[2] / "packages" / "weft-cluster"


def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_ts_producer_python_consumer():
    ts_port = _free_port()
    client = ClusterClient(node_id=2, data_port=_free_port())
    client.start()
    got = []
    sub = client.subscribe("xlang", slot_count=64)

    def consume():
        deadline = time.monotonic() + 15
        while len(got) < 10 and time.monotonic() < deadline:
            f = sub.poll(0.2)
            if f is not None:
                got.append((f.seq, bytes(f.payload)))

    th = threading.Thread(target=consume)
    th.start()
    # SUB must be re-announced: the first datagram can hit the port before
    # the TS producer binds it (UDP drops it) — retry until it lands.
    def announce():
        for _ in range(60):
            client.connect_peer("127.0.0.1", ts_port)
            time.sleep(0.05)
    ann = threading.Thread(target=announce)
    ann.start()
    out = subprocess.run(
        ["node", str(PKG / "test" / "crosslang_udp_produce.mjs"),
         str(ts_port), "10"],
        check=True, capture_output=True, text=True, timeout=30).stdout
    th.join(timeout=20)
    ann.join(timeout=5)
    meta = json.loads(out.strip().splitlines()[-1])
    assert meta["subs"] >= 1, "TS producer never received the Python SUB"
    assert len(got) >= 10, f"only {len(got)} frames from TS producer"
    seqs = [s for s, _ in got]
    assert seqs == sorted(seqs), "seq monotonicity"
    assert got[0][1].startswith(b"ts-frame-")
    client.stop()


def test_python_producer_ts_consumer():
    port = _free_port()
    producer = ClusterClient(node_id=5, data_port=port)
    producer.start()
    producer.publish("xlang", b"pre-warm")  # register topic before SUB arrives
    proc = subprocess.Popen(
        ["node", str(PKG / "test" / "crosslang_udp_consume.mjs"), str(port), "10"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # Pump until the TS consumer's SUB registers us as its peer (run() would
    # deadlock: the consumer exits only after the burst it is waiting for).
    sub_deadline = time.monotonic() + 8
    while not producer.peers and time.monotonic() < sub_deadline:
        producer.pump()
        time.sleep(0.01)
    assert producer.peers, "TS SUB never arrived at the Python producer"
    for i in range(10):
        producer.publish("xlang", f"py-frame-{i}".encode())
        time.sleep(0.02)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        producer.pump()
        time.sleep(0.01)
    out, _ = proc.communicate(timeout=20)
    result = json.loads(out.strip().splitlines()[-1])
    assert result["got"] == 10, f"TS consumer got {result}"
    assert result["seqs"] == sorted(result["seqs"])
    assert result["first"].startswith("py-frame-")
    assert result["stale"] == 0
    assert result["decodeErrors"] == 0
    producer.stop()
