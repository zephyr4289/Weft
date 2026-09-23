"""ingest.py — Python-side producers: camera/audio bridges that commit into
WTR1 rings with zero per-frame temporary buffers (Law 1).

AudioPcmFeeder mirrors packages/weft-tensor/src/ingest/audio.js: any chunk
size, slots roll at the DECLARED quantum (chunkSamples), carry state is
preallocated, segments are bounded loops over precreated memoryviews.
"""
from __future__ import annotations

import struct
from typing import Optional

import numpy as np

from .layout import LayoutError, DLPACK_FLOAT, fourcc_from_str
from .ring import WeftRing


class AudioPcmFeeder:
    """Interleaved float32 PCM -> ring. Chunks of any size; zero-alloc steady state."""

    def __init__(self, ring: WeftRing, channels: int = 1, chunk_samples: int = 128,
                 sample_hz: int = 48000):
        L = ring.layout
        if (L.dtype_code, L.dtype_bits) != (DLPACK_FLOAT, 32):
            raise LayoutError("WTR1_INGEST_DTYPE", "AudioPcmFeeder needs an f32 ring")
        if chunk_samples < 1 or chunk_samples * 4 > L.payload_cap:
            raise LayoutError("WTR1_AUDIO_CHUNK",
                              f"chunk_samples {chunk_samples} exceeds slot cap {L.payload_cap}")
        self._ring = ring
        self._channels = channels
        self._chunk_samples = chunk_samples
        self._sample_hz = sample_hz
        self._fourcc = fourcc_from_str("PCM ")
        self._open_seq = 0      # 0 = no open slot
        self._open_pos = 0      # samples written into the open slot
        self.stats = {"chunks": 0, "samples": 0, "slot_rolls": 0, "partials": 0}

    @property
    def channels(self) -> int:
        return self._channels

    @property
    def chunk_samples(self) -> int:
        return self._chunk_samples

    @property
    def sample_hz(self) -> int:
        return self._sample_hz

    def feed(self, chunk, *, ts: Optional[int] = None) -> int:
        """Feed interleaved f32 samples (numpy array or buffer). Returns last seq."""
        arr = np.ascontiguousarray(chunk, dtype=np.float32).reshape(-1)
        ring = self._ring
        mv = ring._mv  # writable check happens in commit
        pos = 0
        last_seq = self._open_seq
        n_total = int(arr.shape[0])
        while pos < n_total:
            if self._open_seq == 0:
                self._open_seq = ring.producer_seq + 1
                self._open_pos = 0
            room = self._chunk_samples - self._open_pos
            take = min(room, n_total - pos)
            slot = (self._open_seq - 1) % ring.layout.slot_count
            base = ring.layout.header_size + slot * ring.layout.slot_stride
            payload_base = base + 64
            # Bounded segment copy into the ring (precreated region) — no temps.
            dst = np.ndarray((take,), dtype=np.float32, buffer=mv, offset=payload_base + self._open_pos * 4)
            dst[:] = arr[pos:pos + take]
            self._open_pos += take
            pos += take
            ring.stats["commits"] = ring.stats["commits"]  # counters live on the ring
            if self._open_pos == self._chunk_samples:
                last_seq = self._finish_slot(ts)
            else:
                self.stats["partials"] += 1
        self.stats["chunks"] += 1
        self.stats["samples"] += n_total
        return last_seq

    def _finish_slot(self, ts: Optional[int], samples: Optional[int] = None) -> int:
        ring = self._ring
        seq = self._open_seq
        base = ring._slot_base(seq)
        live = samples if samples is not None else self._chunk_samples
        n = live * 4
        mv = ring._mv
        mv[base:base + 4] = b"WFRM"  # slot magic (seqlock validates it)
        struct.pack_into("<I", mv, base + 4, n)
        struct.pack_into("<Q", mv, base + 8, seq)
        struct.pack_into("<Q", mv, base + 16, ts if ts is not None else 0)
        struct.pack_into("<I", mv, base + 24,
                         int(round((live / self._sample_hz) * 1e6)))
        struct.pack_into("<I", mv, base + 32, self._fourcc)
        mv[base + 36] = 1  # rank
        mv[base + 37] = 1  # planes
        struct.pack_into("<I", mv, base + 28, 1)  # COMMITTED
        struct.pack_into("<I", mv, 96 + 4, (seq >> 32) & 0xFFFFFFFF)
        struct.pack_into("<I", mv, 96, seq & 0xFFFFFFFF)
        ring.stats["commits"] += 1
        self.stats["slot_rolls"] += 1
        self._open_seq = 0
        self._open_pos = 0
        return seq

    def flush(self, *, ts: Optional[int] = None) -> Optional[int]:
        """Commit the trailing partial slot (end of stream) with live payload_len."""
        if self._open_seq == 0 or self._open_pos == 0:
            return None
        return self._finish_slot(ts, samples=self._open_pos)
