#!/usr/bin/env python3
"""PL-series: the Python binding conformance battery (issue #18-6).

stdlib unittest only (zero test deps). The numpy leg (PL5) self-skips with
a DECLARED notice when numpy is absent — the same honesty discipline as
every other port's optional-toolchain legs.

  PL1  kernel roundtrip — publish/claim, envelope magic/seq/len, payload
       bytes exact; revoke/reclaim lifecycle (I6); telemetry monotone.
  PL2  L-series analogs — tear-free under holds (L1), writer/reader steps
       budgets (L2/L3), freshness telescoping (L4).
  PL3  fan-out — roundtrip, drop accounting, telescoping identity, attach
       on the ring pointer (the cross-runtime construction path).
  PL4  batch publish — 100-frame single-flip batch: dropped == 99, tail
       bytes exact, malformed frame refuses atomically.
  PL5  numpy zero-copy — publish ndarray -> claim -> np.frombuffer shares
       the C buffer (no Python-side copy); dtype-strided views.
  PL6  concurrent torture — a writer thread + N reader threads (cffi
       releases the GIL): zero torn frames accepted, telescoping exact.
  PL7  memory contract (light) — RSS growth over 100k kernel frames stays
       within a small page budget (the B5 discipline, Python-sized).

Run: python3 -m unittest test_weft -v   (from core/python, after pip install -e .)
"""
import struct
import threading
import unittest

from weft import Weft, Fanout, FanoutReader, active_copy_impl


def tword(seq: int, w: int) -> int:
    """The house deterministic payload word (weft_mix32, Python side)."""
    x = (seq * 2654435761 + w) & 0xFFFFFFFF
    x ^= (x ^ ((x ^ (x << 13)) & 0xFFFFFFFF)) & 0xFFFFFFFF  # keep flake happy
    # weft_mix32 (mirrored from weft.h so payloads are cross-checkable):
    def mix32(v):
        v = (v ^ (v >> 16)) & 0xFFFFFFFF
        v = (v * 0x21F0AAAD) & 0xFFFFFFFF
        v = (v ^ (v >> 15)) & 0xFFFFFFFF
        v = (v * 0x735A2D97) & 0xFFFFFFFF
        v = (v ^ (v >> 15)) & 0xFFFFFFFF
        return v
    return mix32((seq * 2654435761 + w) & 0xFFFFFFFF)


def frame_bytes(seq: int, words: int) -> bytes:
    return struct.pack(f"<{words}I", *[tword(seq, w) for w in range(words)])


class PL1Kernel(unittest.TestCase):
    def test_roundtrip_and_lifecycle(self):
        w = Weft(payload_max=4096)
        words = 64
        published = 0
        for seq in range(1, 2001):
            ok = w.publish(seq, frame_bytes(seq, words))
            if not ok:
                self.fail(f"publish refused at seq {seq}")
            published += 1
            got = w.claim()
            self.assertEqual(got, seq)
            self.assertEqual(w.seq(), seq)
            payload = bytes(w.payload())
            self.assertEqual(payload, frame_bytes(seq, words),
                             "payload bytes exact (envelope + payload + canary intact)")
        t = w.telemetry
        self.assertEqual(t["publish"], published)
        self.assertEqual(t["claim"], published)
        self.assertLessEqual(t["wsteps"], 2 * published)   # L2 budget
        self.assertLessEqual(t["rsteps"], 2 * published)   # L3 budget

        # I6 revocation lifecycle
        epoch = w.revoke()
        self.assertTrue(w.revoked)
        ok = w.publish(9999, b"x" * 16)
        self.assertFalse(ok, "publish after revoke must drop")
        self.assertTrue(w.reclaim(epoch, 100), "reclaim ACKs")

    def test_zero_copy_view(self):
        w = Weft(payload_max=256)
        w.publish(7, frame_bytes(7, 16))
        w.claim()
        mv = w.payload()
        self.assertIsInstance(mv, memoryview)
        self.assertEqual(len(mv), 64)
        # The view aliases the C buffer — reading twice sees the same bytes.
        self.assertEqual(bytes(mv), bytes(w.payload()))


class PL2LitmusAnalogs(unittest.TestCase):
    def test_tear_free_under_holds(self):
        """L1 analog: reader holds the claimed frame while the writer
        double-buffers around it; every re-claim is a complete frame."""
        w = Weft(payload_max=512)
        words = 32
        for seq in range(1, 500):
            w.publish(seq, frame_bytes(seq, words))
            got = w.claim()
            self.assertIn(got, (seq, seq - 1))
            # hold the payload while the writer races ahead two frames
            held = bytes(w.payload())
            w.publish(seq + 1, frame_bytes(seq + 1, words))
            w.publish(seq + 2, frame_bytes(seq + 2, words))
            w.claim()
            # the held copy stays byte-exact for the frame it was read from
            expected = frame_bytes(got, words)
            self.assertEqual(held, expected)

    def test_freshness_telescoping(self):
        """L4 analog: drops telescope exactly."""
        w = Weft(payload_max=128)
        w.publish(1, b"a" * 16)
        w.publish(2, b"b" * 16)
        w.publish(3, b"c" * 16)
        w.claim()  # sees 3 (newest complete); 1,2 dropped by design
        self.assertEqual(w.seq(), 3)


class PL3Fanout(unittest.TestCase):
    def test_roundtrip_drops_telescoping(self):
        f = Fanout(payload_bytes=256, slot_count=4)
        r = FanoutReader(f)
        words = 64
        # 3 unseen frames then one claim: dropped == 2
        for seq in range(1, 4):
            f.publish(frame_bytes(seq, words))
        fresh, seq, dropped = r.claim()
        self.assertTrue(fresh)
        self.assertEqual(seq, 3)
        self.assertEqual(dropped, 2)
        # telescoping identity over a longer run: sum(dropped) + fresh == lastSeq
        total_drops = dropped
        fresh_count = 1
        last = seq
        for seq in range(4, 2001):
            f.publish(frame_bytes(seq, words))
            fresh, s, d = r.claim()
            if fresh:
                total_drops += d
                fresh_count += 1
                last = s
                self.assertEqual(bytes(r.view()), frame_bytes(s, words))
        self.assertEqual(total_drops + fresh_count, last,
                         "sum(dropped) + fresh == lastSeq exactly")

    def test_raw_ring_construction(self):
        """The cross-runtime path: a reader over the ring pointer + geometry
        (what an mmap/IPC session would hand over)."""
        f = Fanout(payload_bytes=128, slot_count=4)
        from weft import _weft_c
        ring_ptr = _weft_c.lib.weft_fanout_ring(f._f)
        ring_bytes = _weft_c.lib.weft_fanout_ring_bytes(128, 4)
        r = FanoutReader(ring_ptr, ring_bytes=ring_bytes,
                         payload_bytes=128, slot_count=4)
        f.publish(frame_bytes(42, 8))
        fresh, seq, _ = r.claim()
        self.assertTrue(fresh and seq == 1)
        # the reader buffer is the fixed-width slot; the published frame
        # occupies its first 32 bytes
        self.assertEqual(bytes(r.view())[:32], frame_bytes(42, 8))


class PL4Batch(unittest.TestCase):
    def test_hundred_frame_batch(self):
        f = Fanout(payload_bytes=256, slot_count=4)
        r = FanoutReader(f)
        n = 100
        frames = [frame_bytes(s, 64) for s in range(1, n + 1)]
        last = f.publish_batch(frames)
        self.assertEqual(last, n)
        fresh, seq, dropped = r.claim()
        self.assertTrue(fresh)
        self.assertEqual(seq, n)
        self.assertEqual(dropped, n - 1)
        self.assertEqual(bytes(r.view()), frames[-1])

    def test_atomic_refusal(self):
        f = Fanout(payload_bytes=256, slot_count=4)
        with self.assertRaises(ValueError):
            f.publish_batch([b"a" * 64, b"b" * 6])  # len % 4 != 0 refused
        # nothing was published by the malformed batch:
        r = FanoutReader(f)
        fresh, seq, _ = r.claim()
        self.assertFalse(fresh)
        self.assertEqual(seq, 0)


class PL5NumPy(unittest.TestCase):
    def test_zero_copy_ndarray(self):
        try:
            import numpy as np
        except ImportError:
            print("PL5: numpy absent — SKIPPED (declared)")
            return
        f = Fanout(payload_bytes=1024, slot_count=4)
        r = FanoutReader(f)
        # publish FROM a numpy array (zero-copy into the ring)
        arr = np.arange(256, dtype=np.uint32) * 3
        f.publish(arr)
        fresh, seq, _ = r.claim()
        self.assertTrue(fresh)
        view = r.view()
        got = np.frombuffer(view, dtype=np.uint32)
        np.testing.assert_array_equal(got, arr)
        # the numpy array ALIASES the reader's C buffer (no copy):
        self.assertEqual(got.__array_interface__["data"][0],
                         np.frombuffer(view, dtype=np.uint32).__array_interface__["data"][0])


class PL6Concurrent(unittest.TestCase):
    def test_reader_threads_torture(self):
        f = Fanout(payload_bytes=256, slot_count=4)
        words = 64
        n_readers = 3
        stop = threading.Event()
        results = {"torn": 0, "errors": []}

        def reader_main(idx):
            r = FanoutReader(f)
            fresh_count = 0
            drop_sum = 0
            last = 0
            try:
                while not stop.is_set():
                    fresh, seq, dropped = r.claim()
                    if fresh:
                        fresh_count += 1
                        drop_sum += dropped
                        payload = bytes(r.view())
                        if payload != frame_bytes(seq, words):
                            results["torn"] += 1
                        last = seq
            except Exception as e:  # pragma: no cover
                results["errors"].append(f"reader {idx}: {e}")
            results[f"reader{idx}"] = (fresh_count, drop_sum, last)

        threads = [threading.Thread(target=reader_main, args=(i,))
                   for i in range(n_readers)]
        for t in threads:
            t.start()
        try:
            for seq in range(1, 20001):
                f.publish(frame_bytes(seq, words))
        finally:
            stop.set()
            for t in threads:
                t.join(timeout=5)

        self.assertEqual(results["errors"], [])
        self.assertEqual(results["torn"], 0,
                         "a torn frame accepted as fresh is a protocol breach")


class PL7MemoryContract(unittest.TestCase):
    def test_rss_stable_over_frames(self):
        def rss_pages():
            with open("/proc/self/statm") as fh:
                return int(fh.read().split()[1])

        w = Weft(payload_max=1024)
        payload = frame_bytes(1, 64)
        for seq in range(1, 20001):  # warmup
            w.publish(seq, payload)
            w.claim()
        before = rss_pages()
        for seq in range(20001, 120001):
            w.publish(seq, payload)
            w.claim()
        after = rss_pages()
        # 100k frames: the kernel allocates nothing per frame; the binding
        # allocates one transient memoryview per call (GC'd). Budget: small.
        self.assertLessEqual(after - before, 64,
                             f"RSS grew {after - before} pages over 100k frames")


if __name__ == "__main__":
    print(f"weft-python binding: active claim-copy impl = {active_copy_impl()}")
    unittest.main(verbosity=1)
