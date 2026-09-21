# weft_spectrum/detector.py — host capability detection (P5, mandate D).
#
# Init-time probing only: results are written ONCE into a ProfileFlyweight.
# The steady-state telemetry loop afterwards reads primitives only (Law 1).
#
# Detection matrix (all probes fail-soft -> E_PROBE_UNAVAILABLE semantics):
#   x86 ISA      : /proc/cpuinfo flags (AVX-512 / AVX2 / SSE4.2)
#   ARM ISA      : 'neon'/'asimd' + 'sve' flags from /proc/cpuinfo
#   CUDA         : ctypes CUDA driver availability (libcuda) — no hard dep
#   Apple MPS    : platform == darwin + arm64 (MPS is an Apple-Silicon lane)
#   OpenVINO     : 'openvino' import probe (guarded)
#   DLPack       : structural (this package exports capsules; CPU lane always)

import ctypes.util
import platform
from pathlib import Path

from .wire import (
    FEAT, TIER_FLAGSHIP, TIER_MID, TIER_BUDGET, THERMAL_NOMINAL,
    BATTERY_UNKNOWN, CHARGING_UNKNOWN, VIS_UNKNOWN, ProfileFlyweight,
)

_PROC_CPUINFO = Path("/proc/cpuinfo")


def _cpu_flags():
    try:
        text = _PROC_CPUINFO.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.lower().startswith("flags") or line.lower().startswith("features"):
                return set(line.split(":", 1)[1].lower().split())
    except OSError:
        pass
    return set()


def detect_isa_flags(flags=None):
    """Returns the set of SHP1 ISA feature names present on this host."""
    f = flags if flags is not None else _cpu_flags()
    isa = set()
    if "avx512f" in f:
        isa.add("AVX512")
    if "avx2" in f:
        isa.add("AVX2")
    if "sse4_2" in f or "sse4.2" in f:
        isa.add("SSE42")
    if "neon" in f or "asimd" in f:
        isa.add("NEON")
    if "sve2" in f or ("sve" in f and "sve2" in f):
        isa.add("SVE2")
    if any(x.startswith("rvv") or x == "v" for x in f):
        isa.add("RVV")
    return isa


def detect_cuda():
    """CUDA driver via libcuda (init-time, fail-soft). Returns bool."""
    try:
        return ctypes.util.find_library("cuda") is not None
    except Exception:
        return False


def detect_apple_mps():
    return platform.system() == "Darwin" and platform.machine() == "arm64"


def detect_openvino():
    try:
        __import__("openvino")
        return True
    except Exception:
        return False


def detect_worker_count():
    import os
    try:
        return max(1, len(os.sched_getaffinity(0)))  # honest usable cores
    except (AttributeError, OSError):
        return max(1, os.cpu_count() or 1)


def detect_memory_total_bytes():
    import os
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (ValueError, OSError, AttributeError):
        return 0


def simd_width_from_features(flags_lo):
    if flags_lo & (1 << FEAT["AVX512"]):
        return 512
    if flags_lo & (1 << FEAT["AVX2"]):
        return 256
    return 128


def detect_into(dst, opts=None):
    """Fill a ProfileFlyweight from live host probes (E_PROBE_UNAVAILABLE path)."""
    o = opts or {}
    isa = o.get("isa") or detect_isa_flags()
    for name in isa:
        dst.featureFlagsLo |= (1 << FEAT[name])

    workers = o.get("workerCount") or detect_worker_count()
    mem = o.get("memoryTotalBytes") or detect_memory_total_bytes()

    if o.get("cuda", detect_cuda()):
        dst.featureFlagsLo |= (1 << FEAT["CUDA"])
    if o.get("mps", detect_apple_mps()):
        dst.featureFlagsLo |= (1 << FEAT["APPLE_MPS"])
    if o.get("openvino", detect_openvino()):
        dst.featureFlagsLo |= (1 << FEAT["OPENVINO"])
    if o.get("dlpack", True):
        dst.featureFlagsLo |= (1 << FEAT["DLPACK_EXPORT"])
    if workers >= 6:
        dst.featureFlagsLo |= (1 << FEAT["BIG_LITTLE"])
    dst.featureFlagsLo |= (1 << FEAT["THERMAL_SENSOR"])

    if dst.siliconTier == TIER_BUDGET or dst.siliconTier == 0:
        tier = TIER_BUDGET
        if workers >= 10 and mem >= 32 * 1024 ** 3:
            tier = TIER_FLAGSHIP
        elif workers >= 4 and mem >= 8 * 1024 ** 3:
            tier = TIER_MID
        dst.siliconTier = o.get("siliconTier", tier)

    dst.thermalState = o.get("thermalState", THERMAL_NOMINAL)
    dst.perfCores = o.get("perfCores", max(1, workers // 2))
    dst.effCores = o.get("effCores", max(0, workers - workers // 2))
    dst.cacheLineBytes = o.get("cacheLineBytes", 64)
    dst.memoryTotalBytes = mem
    dst.memoryBudgetBytes = o.get("memoryBudgetBytes", mem // 8 if mem else 512 * 1024 * 1024)
    dst.simdWidthBits = simd_width_from_features(dst.featureFlagsLo)
    dst.frameBudgetUs = {TIER_FLAGSHIP: 4166, TIER_MID: 8333}.get(dst.siliconTier, 16666)
    dst.maxFrameRateMilliHz = {TIER_FLAGSHIP: 240000, TIER_MID: 120000}.get(dst.siliconTier, 60000)
    dst.batteryPermille = o.get("batteryPermille", BATTERY_UNKNOWN)
    dst.batteryCharging = o.get("batteryCharging", CHARGING_UNKNOWN)
    dst.visibility = o.get("visibility", VIS_UNKNOWN)
    dst.dmaLaneCount = o.get("dmaLaneCount", 2 if workers >= 6 else 1)
    dst.vendorId = o.get("vendorId", 0)
    dst.deviceId = o.get("deviceId", 0)
    return dst


def make_fallback_profile(opts=None):
    return detect_into(ProfileFlyweight(), opts)
