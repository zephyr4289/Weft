# weft_spectrum/wire.py — SHP1 zero-copy decoder for weft_hw_profile_t (P5).
#
# Normative layout: docs/spectrum/SPECTRUM-WIRE-V1.md (byte-frozen, shared
# with packages/spectrum-managed/src/wire.js — offsets MUST stay identical;
# ci stage 1 + tests/spectrum/managed/parity.mjs enforce this).
#
# Law 1: the steady-state loop decodes into a __slots__ flyweight via ONE
# struct.unpack_from call; transient CPython objects are freelist-reused and
# RETAINED heap growth over 1,000,000 cycles is < 64 KiB (probe: tests).
# Law 2: explicit '<' little-endian struct — no native sizing, ever.
# Law 4: integrity violations surface as integer codes; torn records can
# never silently decode as garbage.

import zlib
from struct import Struct

RECORD_SIZE = 192
CRC_OFFSET = 188
MAGIC = b"SHP1"
LAYOUT_VERSION = 1

# §6 Law 4 error taxonomy (frozen; identical to wire.js)
E_BAD_MAGIC = 1
E_BAD_VERSION = 2
E_BAD_SIZE = 3
E_CRC_MISMATCH = 4
E_RESERVED_DIRTY = 5
E_PROBE_UNAVAILABLE = 6
E_DEVICE_LOST = 7
E_FFI_TIMEOUT = 8
E_HEAP_PRESSURE = 9
E_LISTENER_LEAK = 10
E_ALIGN_INVALID = 11
E_HUD_CONTEXT_LOST = 12
E_UNMARSHAL_FAILED = 13
E_TIER_EXHAUSTED = 14
E_HUD_RECOVERED = 15

ERROR_NAMES = {
    1: "E_BAD_MAGIC", 2: "E_BAD_VERSION", 3: "E_BAD_SIZE", 4: "E_CRC_MISMATCH",
    5: "E_RESERVED_DIRTY", 6: "E_PROBE_UNAVAILABLE", 7: "E_DEVICE_LOST",
    8: "E_FFI_TIMEOUT", 9: "E_HEAP_PRESSURE", 10: "E_LISTENER_LEAK",
    11: "E_ALIGN_INVALID", 12: "E_HUD_CONTEXT_LOST", 13: "E_UNMARSHAL_FAILED",
    14: "E_TIER_EXHAUSTED", 15: "E_HUD_RECOVERED",
}

TIER_UNKNOWN, TIER_FLAGSHIP, TIER_MID, TIER_BUDGET = 0, 1, 2, 3
THERMAL_NOMINAL, THERMAL_LIGHT, THERMAL_MODERATE = 0, 1, 2
THERMAL_SEVERE, THERMAL_CRITICAL = 3, 4
VIS_VISIBLE, VIS_HIDDEN, VIS_UNKNOWN = 0, 1, 2
CHARGING_NO, CHARGING_YES, CHARGING_UNKNOWN = 0, 1, 2
BATTERY_UNKNOWN = 0xFFFF

# §3 feature bits (identical to wire.js FEAT)
FEAT = {
    "WASM_SIMD128": 0, "SHARED_ARRAY_BUFFER": 1, "WEBGPU": 2, "WEBGL2": 3,
    "AVX512": 4, "AVX2": 5, "SSE42": 6, "NEON": 7, "SVE2": 8, "RVV": 9,
    "METAL_3": 10, "CUDA": 11, "APPLE_MPS": 12, "OPENVINO": 13,
    "FASTRPC_DSP": 14, "NEUROPILOT": 15, "MULTILANE_DMA": 16,
    "BIG_LITTLE": 17, "THERMAL_SENSOR": 18, "DLPACK_EXPORT": 19,
}
FEAT_BY_BIT = {v: k for k, v in FEAT.items()}

# One-call record decode: '<4s 2H 8I 3Q 2I Q 6I 84x I' = 4+4+32+24+8+8+24+84+4
# = 192 bytes. '<' pins little-endian + no padding (Law 2).
_RECORD = Struct("<4s2H8I3Q2IQ6I84xI")
_RECORD_UNPACK = _RECORD.unpack_from

_RESERVED_ZERO = bytes(84)
_crc32 = zlib.crc32


class ProfileFlyweight:
    """Reusable decode target (Law 1). One instance, mutated in place forever."""
    __slots__ = (
        "layoutVersion", "recordSize", "featureFlagsLo", "featureFlagsHi",
        "siliconTier", "thermalState", "perfCores", "effCores", "gpuFamily",
        "cacheLineBytes", "cpuMaxClockKhz", "memoryTotalBytes",
        "memoryBudgetBytes", "simdWidthBits", "frameBudgetUs",
        "maxFrameRateMilliHz", "batteryPermille", "batteryCharging",
        "visibility", "dmaLaneCount", "vendorId", "deviceId",
    )

    def __init__(self):
        self.layoutVersion = 0
        self.recordSize = 0
        self.featureFlagsLo = 0
        self.featureFlagsHi = 0
        self.siliconTier = TIER_UNKNOWN
        self.thermalState = THERMAL_NOMINAL
        self.perfCores = 0
        self.effCores = 0
        self.gpuFamily = 0
        self.cacheLineBytes = 64
        self.cpuMaxClockKhz = 0
        self.memoryTotalBytes = 0
        self.memoryBudgetBytes = 0
        self.simdWidthBits = 0
        self.frameBudgetUs = 0
        self.maxFrameRateMilliHz = 0
        self.batteryPermille = BATTERY_UNKNOWN
        self.batteryCharging = CHARGING_UNKNOWN
        self.visibility = VIS_UNKNOWN
        self.dmaLaneCount = 0
        self.vendorId = 0
        self.deviceId = 0

    def features(self):
        """Init-time convenience (allocates; NOT for the steady-state loop)."""
        flags = (self.featureFlagsHi << 32) | self.featureFlagsLo
        return [FEAT_BY_BIT[b] for b in sorted(FEAT_BY_BIT) if (flags >> b) & 1]


class ProfileView:
    """Zero-copy window over ONE 192-byte SHP1 record (bytes/bytearray)."""

    __slots__ = ("_buf",)

    def __init__(self, buf):
        self._buf = buf  # zero-copy: holds the caller's buffer, no snapshot

    def validate(self):
        """0 when intact, else a §6 code. Cheapest checks first."""
        buf = self._buf
        if len(buf) < RECORD_SIZE:
            return E_BAD_SIZE
        if bytes(buf[0:4]) != MAGIC:
            return E_BAD_MAGIC
        if (buf[4] | (buf[5] << 8)) != LAYOUT_VERSION:
            return E_BAD_VERSION
        if (buf[6] | (buf[7] << 8)) != RECORD_SIZE:
            return E_BAD_SIZE
        if buf[104:188] != _RESERVED_ZERO:
            return E_RESERVED_DIRTY
        if _crc32(memoryview(buf)[0:CRC_OFFSET]) != (
            buf[188] | (buf[189] << 8) | (buf[190] << 16) | (buf[191] << 24)
        ):
            return E_CRC_MISMATCH
        return 0

    def snapshot_into(self, dst):
        """Decode all fields into the flyweight (returns dst; no new objects)."""
        (magic, version, size, lo, hi, tier, thermal, perf, eff, gpu, cache,
         clock, mem_total, mem_budget, simd, fbudget, max_rate, batt, charg,
         vis, dma, vendor, device, crc) = _RECORD_UNPACK(self._buf, 0)
        dst.layoutVersion = version
        dst.recordSize = size
        dst.featureFlagsLo = lo
        dst.featureFlagsHi = hi
        dst.siliconTier = tier
        dst.thermalState = thermal
        dst.perfCores = perf
        dst.effCores = eff
        dst.gpuFamily = gpu
        dst.cacheLineBytes = cache
        dst.cpuMaxClockKhz = clock
        dst.memoryTotalBytes = mem_total
        dst.memoryBudgetBytes = mem_budget
        dst.simdWidthBits = simd
        dst.frameBudgetUs = fbudget
        dst.maxFrameRateMilliHz = max_rate
        dst.batteryPermille = batt
        dst.batteryCharging = charg
        dst.visibility = vis
        dst.dmaLaneCount = dma
        dst.vendorId = vendor
        dst.deviceId = device
        return dst

    # primitive byte-offset getters (mirrors wire.js offsets exactly)
    @property
    def featureFlagsLo(self):
        return self._buf[8] | (self._buf[9] << 8) | (self._buf[10] << 16) | (self._buf[11] << 24)

    @property
    def featureFlagsHi(self):
        return self._buf[12] | (self._buf[13] << 8) | (self._buf[14] << 16) | (self._buf[15] << 24)

    @property
    def siliconTier(self):
        return self._buf[16] | (self._buf[17] << 8) | (self._buf[18] << 16) | (self._buf[19] << 24)

    @property
    def thermalState(self):
        return self._buf[20] | (self._buf[21] << 8) | (self._buf[22] << 16) | (self._buf[23] << 24)

    @property
    def memoryBudgetBytes(self):
        b = self._buf
        return b[56] | (b[57] << 8) | (b[58] << 16) | (b[59] << 24) | \
            (b[60] << 32) | (b[61] << 40) | (b[62] << 48) | (b[63] << 56)

    @property
    def maxFrameRateMilliHz(self):
        b = self._buf
        return b[72] | (b[73] << 8) | (b[74] << 16) | (b[75] << 24) | \
            (b[76] << 32) | (b[77] << 40) | (b[78] << 48) | (b[79] << 56)

    @property
    def crc32(self):
        b = self._buf
        return b[188] | (b[189] << 8) | (b[190] << 16) | (b[191] << 24)

    def has_feature_bit(self, bit):
        if bit < 32:
            return (self.featureFlagsLo & (1 << bit)) != 0
        return (self.featureFlagsHi & (1 << (bit - 32))) != 0


def crc32_ref(data):
    """Reference CRC (identical arithmetic to wire.js/generate.mjs)."""
    return _crc32(memoryview(data)) & 0xFFFFFFFF


def decode_profile(buf, dst=None):
    """Init-path convenience: validate + snapshot. Returns (code, fw, view)."""
    view = ProfileView(buf)
    code = view.validate()
    if dst is None:
        dst = ProfileFlyweight()
    if code == 0:
        view.snapshot_into(dst)
    return code, dst, view
