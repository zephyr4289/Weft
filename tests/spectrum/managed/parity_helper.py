#!/usr/bin/env python3
"""parity_helper.py — Python decode projection for the parity harness.

Decodes the golden SHP1 fixtures + runs the frozen governor vector, printing
a single JSON document on stdout. parity.mjs compares this against the TS
projection field-by-field (cross-language byte/behavior parity mandate).
Zero environment assumptions beyond the stdlib.
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / ".." / ".." / ".." / "python" / "weft_spectrum"))

from weft_spectrum.wire import (  # noqa: E402
    ProfileView, ProfileFlyweight, decode_profile, E_CRC_MISMATCH,
)
from weft_spectrum.governor import (  # noqa: E402
    CadenceState, GovernorInput, cadence_tick, tier_tick,
)

FIX = HERE / "fixtures"


def decode_all():
    out = {}
    for name in ("flagship", "mid", "budget"):
        buf = (FIX / f"hw-profile-{name}.bin").read_bytes()
        code, fw, view = decode_profile(buf)
        out[name] = {
            "code": code,
            "fields": {k: (view.crc32 if k == "crc32" else getattr(fw, k)) for k in (
                "layoutVersion", "recordSize", "featureFlagsLo",
                "featureFlagsHi", "siliconTier", "thermalState", "perfCores",
                "effCores", "gpuFamily", "cacheLineBytes", "cpuMaxClockKhz",
                "memoryTotalBytes", "memoryBudgetBytes", "simdWidthBits",
                "frameBudgetUs", "maxFrameRateMilliHz", "batteryPermille",
                "batteryCharging", "visibility", "dmaLaneCount", "vendorId",
                "deviceId", "crc32")},
        }
    torn = (FIX / "hw-profile-torn.bin").read_bytes()
    code, _, _ = decode_profile(torn)
    out["torn"] = {"code": code, "expected": E_CRC_MISMATCH}
    return out


def governor_projection():
    vec = json.loads((FIX / "governor_vector.json").read_text())
    st = CadenceState()
    inp = GovernorInput()
    inp.tierMaxHz = vec["profileMaxHz"]
    caps = []
    for t in vec["ticks"]:
        inp.thermalState = t["thermal"]
        inp.batteryPermille = t["batteryPermille"]
        inp.batteryCharging = t["batteryCharging"]
        inp.visibility = t["visibility"]
        inp.heapPressure = t["heapPressure"]
        cadence_tick(st, inp)
        caps.append(st.capHz)
    tier_vec = json.loads((FIX / "tier_vector.json").read_text())
    ts = CadenceState()
    tinp = GovernorInput()
    tinp.profileBudgetBytes = 8589934592
    stages = []
    for t in tier_vec["ticks"]:
        tinp.heapPressure = t["heapPressure"]
        tier_tick(ts, tinp)
        stages.append(ts.tierStage)
    return {"caps": caps, "tierStages": stages}


def main():
    print(json.dumps({
        "language": "python",
        "profiles": decode_all(),
        "governor": governor_projection(),
    }, separators=(",", ":")))


if __name__ == "__main__":
    main()
