# weft_spectrum — Weft Spectrum Managed (Pillar 5).
#
# Hardware-aware runtime intelligence for Python data science:
#   wire      — SHP1 zero-copy weft_hw_profile_t decode (byte-frozen layout)
#   detector  — host capability probes (ISA/CUDA/MPS/OpenVINO) -> flyweight
#   arena     — auto-selecting tensor arena (zero-copy views + DLPack)
#   alignment — 64B/128B cache-line alignment validation + aligned slabs
#   governor  — reactive cadence governor (240/120/60/30) + tier staging
#   hud       — WeftSpectrumHud terminal diagnostic overlay
#
# Laws (docs/spectrum/SPECTRUM-WIRE-V1.md):
#   1. Zero retained allocation in steady-state telemetry
#   2. Explicit little-endian layout determinism
#   3. core/c byte-frozen (this package is pure Python; zero native code)
#   4. Honest boundaries — fail-soft fallbacks, never a crash

from .wire import (  # noqa: F401
    RECORD_SIZE, CRC_OFFSET, MAGIC, LAYOUT_VERSION,
    ERROR_NAMES, E_BAD_MAGIC, E_BAD_VERSION, E_BAD_SIZE, E_CRC_MISMATCH,
    E_RESERVED_DIRTY, E_PROBE_UNAVAILABLE, E_DEVICE_LOST, E_FFI_TIMEOUT,
    E_HEAP_PRESSURE, E_LISTENER_LEAK, E_ALIGN_INVALID, E_HUD_CONTEXT_LOST,
    E_UNMARSHAL_FAILED, E_TIER_EXHAUSTED, E_HUD_RECOVERED,
    TIER_UNKNOWN, TIER_FLAGSHIP, TIER_MID, TIER_BUDGET,
    THERMAL_NOMINAL, THERMAL_LIGHT, THERMAL_MODERATE, THERMAL_SEVERE,
    THERMAL_CRITICAL, VIS_VISIBLE, VIS_HIDDEN, VIS_UNKNOWN,
    CHARGING_NO, CHARGING_YES, CHARGING_UNKNOWN, BATTERY_UNKNOWN,
    FEAT, ProfileFlyweight, ProfileView, crc32_ref, decode_profile,
)
from .governor import (  # noqa: F401
    CADENCE_LADDER, SUSTAINED_TICKS, RECOVERY_TICKS, BACKGROUND_CAP,
    LOW_BATTERY_PERMILLE, MAX_TIER_STAGES, TIER_MAX_HZ,
    CadenceState, GovernorInput, cadence_tick, tier_tick,
    effective_budget_bytes,
)
from .arena import (  # noqa: F401
    TensorArena, ArenaSlab, ArenaBuffer, dlpack_available,
)
from .alignment import (  # noqa: F401
    is_aligned, validate_alignment, aligned_slab, required_alignment,
    address_of,
)
from .hud import SpectrumHudTerminal  # noqa: F401

__version__ = "0.1.0"
